#include "fus.h"

#include <gio/gio.h>
#include <solv/policy.h>
#include <solv/poolarch.h>

static Solver *
solve (Pool *pool, Queue *jobs)
{
  Solver *solver = solver_create (pool);
  solver_set_flag (solver, SOLVER_FLAG_IGNORE_RECOMMENDED, 1);

  int pbcnt = solver_solve (solver, jobs);

  if (pbcnt)
    {
      pbcnt = solver_problem_count (solver);
      for (int problem = 1; problem <= pbcnt; problem++)
        {
          g_auto(Queue) rids, rinfo;
          queue_init (&rids);
          queue_init (&rinfo);

          g_print ("Problem %i / %i:\n", problem, pbcnt);

          solver_findallproblemrules (solver, problem, &rids);
          for (int i = 0; i < rids.count; i++)
            {
              Id probr = rids.elements[i];

              queue_empty (&rinfo);
              solver_allruleinfos (solver, probr, &rinfo);
              for (int j = 0; j < rinfo.count; j += 4)
                {
                  SolverRuleinfo type = rinfo.elements[j];
                  Id source = rinfo.elements[j + 1];
                  Id target = rinfo.elements[j + 2];
                  Id dep = rinfo.elements[j + 3];
                  const char *pbstr = solver_problemruleinfo2str (solver, type, source, target, dep);
                  g_print ("  - %s\n", pbstr);
                }
            }
        }

      solver_free (solver);
      return NULL;
    }

  return solver;
}

static GArray *
_gather_alternatives (Pool *pool, GArray *transactions, Queue *favor, GHashTable *tested, int level)
{
  g_auto(Queue) jobs;
  queue_init (&jobs);

  for (int i = 0; i < favor->count; i++)
    queue_push2 (&jobs, SOLVER_SOLVABLE | SOLVER_FAVOR, favor->elements[i]);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init (&iter, tested);
  while (g_hash_table_iter_next (&iter, &key, &value))
    queue_push2 (&jobs, SOLVER_SOLVABLE | SOLVER_DISFAVOR, GPOINTER_TO_INT (key));

  g_autoptr(Solver) solver = solve (pool, &jobs);

  if (!solver)
    return transactions;

  g_autoptr(Transaction) trans = solver_create_transaction (solver);
  Queue installedq;
  queue_init (&installedq);
  transaction_installedresult (trans, &installedq);
  g_array_append_val (transactions, installedq);

  int altcnt = solver_alternatives_count (solver);
  if (!altcnt)
    return transactions;

  g_auto(Queue) favor_n;
  queue_init (&favor_n);
  queue_prealloc (&favor_n, level - 1);
  g_autoptr(GHashTable) tested_n = g_hash_table_new (g_direct_hash, g_direct_equal);

  g_auto(Queue) choices;
  queue_init (&choices);
  int max_level = 0;
  Id choice = 0;
  for (int alt = 1; alt <= altcnt; alt++)
    {
      g_auto(Queue) alts;
      queue_init (&alts);
      Id chosen;
      int l;
      int type = solver_get_alternative (solver, alt, NULL, NULL, &chosen, &alts, &l);

      if (type != SOLVER_ALTERNATIVE_TYPE_RULE)
        continue;

      if (max_level < l)
        max_level = l;

      if (l <= level)
        queue_push (&favor_n, chosen);
      if (l == level + 1)
        g_hash_table_add (tested_n, GINT_TO_POINTER (chosen));
      if (l != level)
        continue;
      else
        {
          queue_prealloc (&choices, alts.count);
          for (int i = 0; i < alts.count; i++)
            {
              Id p = alts.elements[i];
              queue_push (&choices, p > 0 ? p : -p);
            }
          choice = chosen;
        }
    }

  g_hash_table_add (tested, GINT_TO_POINTER (choice));

  for (;;)
    {
      gboolean all = TRUE;
      for (int i = 0; i < choices.count; i++)
        if (!g_hash_table_contains (tested, GINT_TO_POINTER (choices.elements[i])))
          {
            all = FALSE;
            break;
          }
      if (all)
        break;
      _gather_alternatives (pool, transactions, favor, tested, level);
    }

  if (level == max_level)
    return transactions;

  _gather_alternatives (pool, transactions, &favor_n, tested_n, level + 1);

  return transactions;
}

static GArray *
gather_alternatives (Pool *pool, Queue *jobs)
{
  GArray *transactions = g_array_new (FALSE, FALSE, sizeof (Queue));
  g_array_set_clear_func (transactions, (GDestroyNotify)queue_free);
  g_auto(Queue) favor;
  queue_init (&favor);
  g_autoptr(GHashTable) tested = g_hash_table_new (g_direct_hash, g_direct_equal);

  Queue pjobs = pool->pooljobs;
  pool->pooljobs = *jobs;
  transactions = _gather_alternatives (pool, transactions, &favor, tested, 1);
  pool->pooljobs = pjobs;

  return transactions;
}

static inline gboolean
queue_contains (Queue *q, Id id)
{
  for (int i = q->count; i > 0; )
    if (q->elements[--i] == id)
      return TRUE;
  return FALSE;
}

/**
 * apply_excludes:
 * @pool: initialized pool
 * @exclude_packages: names of packages to be excluded
 * @lookaside_repos: set of repos that will not have packages excluded
 * @modular_pkgs: map of modular packages that exclusion does not apply to
 *
 * Apply excludes: this completely hides the package from any processing.
 * Packages in lookaside are not removed, and neither are modular packages.
 */
static Map
apply_excludes (Pool       *pool,
                GStrv       exclude_packages,
                GHashTable *lookaside_repos,
                Map        *modular_pkgs)
{
  Map excludes;
  map_init (&excludes, pool->nsolvables);
  /* This map has 1 for all available packages, 0 for excluded ones. */
  map_setall (&excludes);

  for (GStrv exclude = exclude_packages; exclude && *exclude; exclude++)
    {
      g_auto(Queue) sel;
      queue_init (&sel);
      selection_make (pool, &sel, *exclude, SELECTION_NAME | SELECTION_GLOB | SELECTION_DOTARCH);
      if (!sel.count)
        {
          g_warning ("Nothing matches exclude '%s'", *exclude);
          continue;
        }

      g_auto(Queue) q;
      queue_init (&q);
      selection_solvables (pool, &sel, &q);

      for (int j = 0; j < q.count; j++)
        {
          Id p = q.elements[j];
          Solvable *s = pool_id2solvable (pool, p);
          /* Ignore packages from lookaside. */
          if (g_hash_table_contains (lookaside_repos, s->repo))
            continue;

          /* Modular package, not excluding... */
          if (map_tst (modular_pkgs, p))
            continue;

          g_info ("Excluding %s (based on %s)",
                  pool_solvable2str (pool, s),
                  *exclude);
          map_clr (&excludes, p);
        }
    }

  return excludes;
}

static Map
precompute_modular_packages (Pool *pool)
{
  Map modular_pkgs;
  map_init (&modular_pkgs, pool->nsolvables);
  Id *pp = pool_whatprovides_ptr (pool, pool_str2id (pool, MODPKG_PROV, 1));
  for (; *pp; pp++)
    map_set (&modular_pkgs, *pp);

  return modular_pkgs;
}

static gboolean
_install_transaction (Pool         *pool,
                      Queue        *pile,
                      Queue        *job,
                      Map          *tested,
                      unsigned int  indent)
{
  g_autoptr(Solver) solver = solve (pool, job);
  if (!solver)
    return FALSE;

  g_autoptr(Transaction) trans = solver_create_transaction (solver);
  g_auto(Queue) installedq;
  queue_init (&installedq);
  transaction_installedresult (trans, &installedq);
  for (int x = 0; x < installedq.count; x++)
    {
      Id p = installedq.elements[x];
      queue_pushunique (pile, p);
      const gchar *solvable = pool_solvid2str (pool, p);
      g_debug ("%*c - %s", indent, ' ', solvable);
      /* Non-modules are immediately marked as resolved, since for RPM the
       * result would not change if done again. However for modules we need to
       * make sure we look at all combinations. */
      if (!g_str_has_prefix (solvable, "module:"))
        map_set (tested, p);
    }

  return TRUE;
}

/**
 * mask_bare_rpms:
 * @pool: initialized pool
 * @pile: the queue of packages for resolution
 *
 * For each available modular package, find all bare RPMs with the same name,
 * and mark them as not considered if they are not in the pile.
 */
static void
mask_bare_rpms (Pool  *pool,
                Queue *pile)
{
  /* Get array of all existing modular packages. */
  Id *modular_packages = pool_whatprovides_ptr (pool, pool_str2id (pool, MODPKG_PROV, 1));

  /* Filter it to keep only available packages. A package that is not
   * considered should not mask anything. */
  g_auto(Queue) available_modular_pkgs;
  queue_init(&available_modular_pkgs);
  for (Id *pp = modular_packages; *pp; pp++)
    if (map_tst (pool->considered, *pp))
      queue_push (&available_modular_pkgs, *pp);

  for (int i = 0; i < available_modular_pkgs.count; i++)
    {
      Id pp = available_modular_pkgs.elements[i];
      Solvable *modpkg = pool_id2solvable (pool, pp);

      g_auto(Queue) sel;
      queue_init (&sel);
      selection_make (pool, &sel, pool_id2str (pool, modpkg->name), SELECTION_NAME);
      if (!sel.count)
        {
          /* This should never happen, at least one package
           * should match (the modular one). */
          continue;
        }
      g_auto(Queue) q;
      queue_init (&q);
      selection_solvables (pool, &sel, &q);

      for (int j = 0; j < q.count; j++)
        {
          Id p = q.elements[j];
          /* A bare RPM can be in the pile if, e.g, it was requested
           * explicitly. In that case, we don't want to disconsider it,
           * otherwise libsolv will report resolution problems */
          if (!queue_contains (&available_modular_pkgs, p) &&
              !queue_contains (pile, p))
            map_clr (pool->considered, p);
        }
    }
}

/**
 * disable_module:
 * @pool: initialized pool
 * @module: Id of the module to be disabled
 *
 * Set the module and all packages in it as not considered. The packages would
 * not be pulled in anyway since that would require pulling in disabled module,
 * but if they are considered, it would cause problems with masking unavailable
 * packages since we wouldn't really know which modular packages are available.
 */
static void
disable_module (Pool *pool, Id module)
{
  map_clr (pool->considered, module);

  Solvable *s = pool_id2solvable (pool, module);
  g_auto(Queue) q;
  queue_init (&q);
  Id dep = pool_rel2id (pool, s->name, s->arch, REL_ARCH, 1);
  pool_whatcontainsdep (pool, SOLVABLE_REQUIRES, dep, &q, 0);

  for (int k = 0; k < q.count; k++)
    {
      Id p = q.elements[k];
      map_clr (pool->considered, p);
    }
}

static gboolean
add_module_and_pkgs_to_pile (Pool     *pool,
                             Queue    *pile,
                             Map      *tested,
                             Id        module,
                             gboolean  with_deps)
{
  gboolean solv_failed = FALSE;

  /* Make sure to include the module into the pile even if
   * it doesn't contain any components (e.g, an empty module)
   */
  queue_pushunique (pile, module);

  g_auto(Queue) q;
  queue_init (&q);
  Solvable *s = pool_id2solvable (pool, module);
  Id dep = pool_rel2id (pool, s->name, s->arch, REL_ARCH, 1);
  pool_whatcontainsdep (pool, SOLVABLE_REQUIRES, dep, &q, 0);

  g_auto(Queue) j;
  queue_init (&j);
  for (int k = 0; k < q.count; k++)
    {
      Id p = q.elements[k];
      /* Add modular package even if it's not installable */
      queue_pushunique (pile, p);

      if (!with_deps)
        continue;

      queue_empty (&j);
      queue_push2 (&j, SOLVER_SOLVABLE | SOLVER_INSTALL, p);
      g_debug ("    Installing %s:", pool_solvid2str (pool, p));

      if (!_install_transaction (pool, pile, &j, tested, 6))
        solv_failed = TRUE;
    }

  return solv_failed;
}

static gboolean
resolve_all_solvables (Pool  *pool,
                       Queue *pile,
                       Map   *excludes)
{
  g_auto(Map) tested;
  map_init (&tested, pool->nsolvables);
  g_auto(Queue) job;
  queue_init (&job);
  gboolean all_tested = FALSE;
  gboolean solv_failed = FALSE;

  Id ndef_modules_rel = pool_rel2id (pool,
                                     pool_str2id (pool, "module()", 1),
                                     pool_str2id (pool, "module-default()", 1),
                                     REL_WITHOUT,
                                     1);

  do
    {
      for (int i = 0; i < pile->count; i++)
        {
          Id p = pile->elements[i];
          if (map_tst (&tested, p))
            continue;
          map_set (&tested, p);

          Solvable *s = pool_id2solvable (pool, p);

          map_free (pool->considered);
          map_init_clone (pool->considered, excludes);

          queue_empty (&job);
          queue_push2 (&job, SOLVER_SOLVABLE | SOLVER_INSTALL, p);

          /* For non-modular solvables we are not interested
           * in getting all combinations */
          if (!g_str_has_prefix (pool_id2str (pool, s->name), "module:"))
            {
              g_debug ("Installing %s:", pool_solvid2str (pool, p));

              /* Disable all non-default unrelated modules */
              Id *pp = pool_whatprovides_ptr (pool, ndef_modules_rel);
              for (; *pp; pp++)
                disable_module (pool, *pp);

              mask_bare_rpms (pool, pile);

              if (!_install_transaction (pool, pile, &job, &tested, 2))
                solv_failed = TRUE;
            }
          else
            {
              g_debug ("Searching combinations for %s", pool_solvid2str (pool, p));
              g_autoptr(GArray) transactions = gather_alternatives (pool, &job);

              if (transactions->len == 0)
                {
                  solv_failed = TRUE;
                  /* Add module and its packages even if they have broken deps */
                  add_module_and_pkgs_to_pile (pool, pile, &tested, p, FALSE);
                }

              for (unsigned int i = 0; i < transactions->len; i++)
                {
                  Queue t = g_array_index (transactions, Queue, i);

                  /* install our combination */
                  queue_empty (&job);
                  g_debug ("  Transaction %i / %i:", i + 1, transactions->len);
                  for (int j = 0; j < t.count; j++)
                    {
                      Id p = t.elements[j];
                      queue_push2 (&job, SOLVER_SOLVABLE | SOLVER_INSTALL, p);
                      g_debug ("    - %s", pool_solvid2str (pool, p));
                    }

                  /* Reset considered packages to everything minus excludes. */
                  map_free (pool->considered);
                  map_init_clone (pool->considered, excludes);

                  /* Disable all non-default unrelated modules */
                  Id *pp = pool_whatprovides_ptr (pool, ndef_modules_rel);
                  for (; *pp; pp++)
                    if (!queue_contains (&t, *pp))
                      disable_module (pool, *pp);

                  mask_bare_rpms(pool, pile);

                  Queue pjobs = pool->pooljobs;
                  pool->pooljobs = job;
                  for (int j = 0; j < t.count; j++)
                    solv_failed |= add_module_and_pkgs_to_pile (pool,
                                                                pile,
                                                                &tested,
                                                                t.elements[j],
                                                                TRUE);
                  pool->pooljobs = pjobs;
                }
            }
        }

      for (int i = 0; i < pile->count; i++)
        {
          if (!map_tst (&tested, pile->elements[i]))
            break;
          all_tested = TRUE;
        }
    }
  while (!all_tested);

  return solv_failed;
}

static void
add_solvable_to_pile (const char *solvable,
                      Pool       *pool,
                      Queue      *pile,
                      Queue      *exclude)
{
  g_auto(Queue) sel;
  queue_init (&sel);
  /* First let's select packages based on name, glob or name.arch combination ... */
  selection_make (pool, &sel, solvable,
                  SELECTION_NAME | SELECTION_PROVIDES | SELECTION_GLOB | SELECTION_DOTARCH);
  /* ... then remove masked packages from the selection (either hidden in
   * non-default module stream) or bare RPMs hidden by a package in default
   * module stream) ... */
  selection_subtract (pool, &sel, exclude);
  /* ... and finally add anything that matches the exact NEVRA. No masking
   * should apply here, since if the user specified exact build, they probably
   * really want it. */
  selection_make (pool, &sel, solvable, SELECTION_CANON | SELECTION_ADD);

  g_auto(Queue) q;
  queue_init (&q);
  selection_solvables (pool, &sel, &q);
  if (!q.count)
  {
    g_warning ("Nothing matches '%s'", solvable);
    return;
  }
  pool_best_solvables (pool, &q, 0);
  for (int j = 0; j < q.count; j++)
    queue_push (pile, q.elements[j]);
}

static gboolean
add_solvables_from_file_to_pile (const char *filename,
                                 Pool       *pool,
                                 Queue      *pile,
                                 Queue      *exclude,
                                 GError    **error)
{
  g_autoptr(GIOChannel) ch = g_io_channel_new_file (filename, "r", error);
  if (ch == NULL)
    {
      g_prefix_error (error, "%s: ", filename);
      return FALSE;
    }

  GIOStatus ret;
  do
    {
      g_autofree gchar *content = NULL;
      gsize length, tpos;

      ret = g_io_channel_read_line (ch, &content, &length, &tpos, error);
      if (ret != G_IO_STATUS_NORMAL || content == NULL)
        continue;

      content[tpos] = '\0';
      if (*content)
        add_solvable_to_pile (content, pool, pile, exclude);
    }
  while (ret == G_IO_STATUS_NORMAL);

  if (ret != G_IO_STATUS_EOF)
    {
      g_prefix_error (error, "%s: ", filename);
      return FALSE;
    }

  return TRUE;
}

static gboolean
add_solvables_to_pile (Pool    *pool,
                       Queue   *pile,
                       Queue   *exclude,
                       GStrv    solvables,
                       GError **error)
{
  g_return_val_if_fail (!error || !*error, FALSE);

  for (GStrv solvable = solvables; solvable && *solvable; solvable++)
    {
      /* solvables prefixed by @ are file names */
      if (**solvable == '@')
        {
          if (!add_solvables_from_file_to_pile (*solvable + 1, pool, pile, exclude, error))
            return FALSE;
        }
      else
        add_solvable_to_pile (*solvable, pool, pile, exclude);
    }

  return TRUE;
}

static Queue
mask_non_default_module_pkgs (Pool *pool)
{
  Queue selection;
  queue_init(&selection);

  Id ndef_modules_rel = pool_rel2id (pool,
                                     pool_str2id (pool, "module()", 1),
                                     pool_str2id (pool, "module-default()", 1),
                                     REL_WITHOUT,
                                     1);
  Id *pp = pool_whatprovides_ptr (pool, ndef_modules_rel);
  for (; *pp; pp++)
    {
      Solvable *s = pool_id2solvable (pool, *pp);
      g_auto(Queue) q;
      queue_init (&q);
      Id dep = pool_rel2id (pool, s->name, s->arch, REL_ARCH, 1);
      pool_whatcontainsdep (pool, SOLVABLE_REQUIRES, dep, &q, 0);

      for (int i = 0; i < q.count; i++)
        selection_make (pool,
                        &selection,
                        pool_solvid2str (pool, q.elements[i]),
                        SELECTION_CANON | SELECTION_ADD);
    }

  return selection;
}

/*
 * Mask bare rpms if any of the default modules provides them (even if older)
 */
static Queue
mask_solvable_bare_rpms (Pool *pool)
{
  Queue selection;
  queue_init (&selection);

  Id def_modules_rel = pool_rel2id (pool,
                                    pool_str2id (pool, "module()", 1),
                                    pool_str2id (pool, "module-default()", 1),
                                    REL_WITH,
                                    1);

  Id *pp = pool_whatprovides_ptr (pool, def_modules_rel);
  for (; *pp; pp++)
    {
      Solvable *s = pool_id2solvable (pool, *pp);
      g_auto(Queue) q;
      queue_init (&q);
      Id dep = pool_rel2id (pool, s->name, s->arch, REL_ARCH, 1);
      pool_whatcontainsdep (pool, SOLVABLE_REQUIRES, dep, &q, 0);

      for (int i = 0; i < q.count; i++)
        {
          Solvable *modpkg = pool_id2solvable (pool, q.elements[i]);
          Id bare_rpms_rel = pool_rel2id (pool,
                                          modpkg->name,
                                          pool_str2id (pool, MODPKG_PROV, 1),
                                          REL_WITHOUT,
                                          1);

          Id *mp = pool_whatprovides_ptr (pool, bare_rpms_rel);
          for (; *mp; mp++)
            selection_make (pool,
                            &selection,
                            pool_solvid2str (pool, *mp),
                            SELECTION_CANON | SELECTION_ADD);
        }
    }

  return selection;
}

GPtrArray *
fus_depsolve (const char *arch,
              const char *platform,
              const GStrv exclude_packages,
              const GStrv repos,
              const GStrv solvables,
              GError    **error)
{
  g_autoptr(Pool) pool = pool_create ();
#ifndef FUS_TESTING
  /* Needed for downloading metadata from remote repos */
  g_autoptr(SoupSession) session = soup_session_new ();
  pool_setloadcallback (pool, filelist_loadcb, session);
#endif

  pool_setarch (pool, arch);

  Repo *system = create_system_repo (pool, platform, arch);
  pool_set_installed (pool, system);

  g_autoptr(GHashTable) lookaside_repos = g_hash_table_new (g_direct_hash, NULL);
  g_hash_table_add (lookaside_repos, system);
  for (GStrv repo = repos; repo && *repo; repo++)
    {
      g_auto(GStrv) strv = g_strsplit (*repo, ",", 3);
      Repo *r = NULL;
#ifdef FUS_TESTING
      r = create_test_repo (pool, strv[0], strv[1], strv[2], error);
#else
      r = create_repo (pool, session, strv[0], strv[2], error);
#endif
      if (!r)
        return NULL;

      if (g_strcmp0 (strv[1], "lookaside") == 0)
        {
          g_hash_table_add (lookaside_repos, r);
          r->subpriority = 100;
        }
    }

  pool_addfileprovides (pool);
  pool_createwhatprovides (pool);

  /* Precompute map of modular packages. */
  g_auto(Map) modular_pkgs = precompute_modular_packages (pool);

  /* Find out excluded packages */
  g_auto(Map) excludes = apply_excludes (pool, exclude_packages, lookaside_repos, &modular_pkgs);

  /* Find packages from non-default modules */
  g_auto(Queue) disconsider = mask_non_default_module_pkgs (pool);

  /* Find bare rpms masked by default modules */
  g_auto(Queue) bare_rpms = mask_solvable_bare_rpms (pool);
  selection_add (pool, &disconsider, &bare_rpms);

  g_auto(Map) considered;
  pool->considered = &considered;
  map_init_clone (pool->considered, &excludes);

  g_auto(Queue) pile;
  queue_init (&pile);
  if (!add_solvables_to_pile (pool, &pile, &disconsider, solvables, error))
    return NULL;
  if (!pile.count)
    {
      g_set_error_literal (error,
                           G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                           "No solvables matched");
      return NULL;
    }

  gboolean solv_failed = resolve_all_solvables (pool, &pile, &excludes);
  if (solv_failed)
    g_warning ("Can't resolve all solvables");

  /* Output resolved packages */
  GPtrArray *output = g_ptr_array_new_full (pile.count, g_free);
  for (int i = 0; i < pile.count; i++)
    {
      Id p = pile.elements[i];
      Solvable *s = pool_id2solvable (pool, p);
      if (g_hash_table_contains (lookaside_repos, s->repo))
        continue;
      char *name = g_strdup_printf ("%s%s@%s",
                                    map_tst (&modular_pkgs, p) ? "*" : "",
                                    pool_solvable2str (pool, s), s->repo->name);
      g_ptr_array_add (output, name);
    }

  /* We need to free the repomd checksum we saved in repo->appdata */
  int id;
  Repo *r;
  FOR_REPOS(id, r)
    if (r->appdata)
      g_free (r->appdata);

  return output;
}
