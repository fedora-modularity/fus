#include <stdint.h>
#include <glib.h>
#include <gio/gio.h>
#include <modulemd.h>
#include <solv/policy.h>
#include <solv/pool.h>
#include <solv/poolarch.h>
#include <solv/selection.h>
#include <solv/testcase.h>
#include <solv/transaction.h>
#include <solv/repo_comps.h>
#include <solv/repo_repomdxml.h>
#include <solv/repo_rpmmd.h>
#include <solv/solv_xfopen.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Pool, pool_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Solver, solver_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Transaction, transaction_free);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Queue, queue_free);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Map, map_free);

#define TMPL_NPROV "module(%s)"
#define TMPL_NSPROV "module(%s:%s)"
#define MODPKG_PROV "modular-package()"

static inline Id
dep_or_rel (Pool *pool, Id dep, Id rel, Id op)
{
  return dep ? pool_rel2id (pool, dep, rel, op, 1) : rel;
}

static Id
parse_module_requires (Pool       *pool,
                       GHashTable *reqs)
{
  GHashTableIter iter;
  g_hash_table_iter_init (&iter, reqs);
  gpointer key, value;
  Id require = 0;
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *n = key;
      g_auto(GStrv) reqv = modulemd_simpleset_dup (value);

      Id req_neg = 0, req_pos = 0;
      for (GStrv ss = reqv; *ss; ss++)
        {
          const char *s = *ss;

          Id *r;
          if (s[0] == '-')
            {
              r = &req_neg;
              s++;
            }
          else
            r = &req_pos;

          g_autofree char *nsprov = g_strdup_printf (TMPL_NSPROV, n, s);
          *r = dep_or_rel (pool, *r, pool_str2id (pool, nsprov, 1), REL_OR);
        }

      g_autofree char *nprov = g_strdup_printf (TMPL_NPROV, n);
      Id req = pool_str2id (pool, nprov, 1);
      if (req_pos)
        req = dep_or_rel (pool, req, req_pos, REL_WITH);
      else if (req_neg)
        req = dep_or_rel (pool, req, req_neg, REL_WITHOUT);

      require = dep_or_rel (pool, require, req, REL_AND);
    }

  return require;
}

static void
add_source_package (Repo       *repo,
                    GHashTable *req,
                    const char *name)
{
  Pool *pool = repo->pool;
  Solvable *solvable = pool_id2solvable (pool, repo_add_solvable(repo));
  solvable->name = pool_str2id (pool, name, 1);
  solvable->evr = ID_EMPTY;
  solvable->arch = ARCH_SRC;

  Id requires = parse_module_requires (pool, req);
  solvable_add_deparray (solvable, SOLVABLE_REQUIRES, requires, 0);
}

static void
add_module_dependencies (Pool      *pool,
                         Solvable  *solvable,
                         GPtrArray *deps)
{
  Id requires = 0;
  for (unsigned int i = 0; i < deps->len; i++)
    {
      GHashTable *req = modulemd_dependencies_peek_requires (g_ptr_array_index (deps, i));
      Id require = parse_module_requires (pool, req);
      requires = dep_or_rel (pool, requires, require, REL_OR);
    }
  solvable_add_deparray (solvable, SOLVABLE_REQUIRES, requires, 0);
}

static void
add_artifacts_dependencies (Pool  *pool,
                            Queue *sel,
                            Id     sdep)
{
  g_auto(Queue) rpms;
  queue_init (&rpms);
  selection_solvables (pool, sel, &rpms);
  for (int i = 0; i < rpms.count; i++)
    {
      Solvable *s = pool_id2solvable (pool, rpms.elements[i]);

      /* Req: module:$n:$s:$v:$c . $a */
      solvable_add_deparray (s, SOLVABLE_REQUIRES, sdep, 0);

      /* Prv: modular-package() */
      Id modpkg = pool_str2id (pool, MODPKG_PROV, 1);
      solvable_add_deparray (s, SOLVABLE_PROVIDES, modpkg, 0);
    }
}

static void
add_module_rpm_artifacts (Pool           *pool,
                          ModulemdModule *module,
                          Id              sdep)
{
  g_auto(GStrv) rpm_artifacts = modulemd_simpleset_dup (modulemd_module_peek_rpm_artifacts (module));
  g_auto(Queue) sel;
  queue_init (&sel);
  for (GStrv artifact = rpm_artifacts; *artifact; artifact++)
    {
      const char *nevra = *artifact;

      const char *evr_delimiter = NULL;
      const char *rel_delimiter = NULL;
      const char *arch_delimiter = NULL;
      const char *end;

      for (end = nevra; *end; ++end)
        {
          if (*end == '-')
            {
              evr_delimiter = rel_delimiter;
              rel_delimiter = end;
            }
          else if (*end == '.')
            arch_delimiter = end;
        }

      if (!evr_delimiter || evr_delimiter == nevra)
        continue;

      size_t name_len = evr_delimiter - nevra;

      /* Strip "0:" epoch if present */
      if (evr_delimiter[1] == '0' && evr_delimiter[2] == ':')
        evr_delimiter += 2;

      if (rel_delimiter - evr_delimiter <= 1 ||
          !arch_delimiter || arch_delimiter <= rel_delimiter + 1 || arch_delimiter == end - 1)
        continue;

      Id nid, evrid, aid;
      if (!(nid = pool_strn2id (pool, nevra, name_len, 0)))
        continue;
      evr_delimiter++;
      if (!(evrid = pool_strn2id (pool, evr_delimiter, arch_delimiter - evr_delimiter, 0)))
        continue;
      arch_delimiter++;
      if (!(aid = pool_strn2id (pool, arch_delimiter, end - arch_delimiter, 0)))
        continue;

      /* $n.$a = $evr */
      Id rid = pool_rel2id (pool, nid, aid, REL_ARCH, 1);
      rid = pool_rel2id (pool, rid, evrid, REL_EQ, 1);

      queue_push2 (&sel, SOLVER_SOLVABLE_NAME | SOLVER_SETEVR | SOLVER_SETARCH, rid);
    }

  add_artifacts_dependencies (pool, &sel, sdep);
}

static void
add_module_solvables (Repo           *repo,
                      ModulemdModule *module)
{
  Pool *pool = repo->pool;

  const char *n = modulemd_module_peek_name (module);
  g_autofree char *nprov = g_strdup_printf (TMPL_NPROV, n);
  const char *s = modulemd_module_peek_stream (module);
  g_autofree char *nsprov = g_strdup_printf (TMPL_NSPROV, n, s);
  const uint64_t v = modulemd_module_peek_version (module);
  g_autofree char *vs = g_strdup_printf ("%" G_GUINT64_FORMAT, v);
  const char *c = modulemd_module_peek_context (module);
  const char *a = modulemd_module_peek_arch (module);
  if (!a)
    a = "noarch";

  GPtrArray *deps = modulemd_module_peek_dependencies (module);

  /* If context is defined, then it's built artefact */
  if (c)
    {
      Solvable *solvable = pool_id2solvable (pool, repo_add_solvable (repo));
      g_autofree char *name = g_strdup_printf ("module:%s:%s:%s:%s", n, s, vs, c);
      solvable->name = pool_str2id (pool, name, 1);
      solvable->evr = ID_EMPTY;
      solvable->arch = pool_str2id (pool, a, 1);

      /* Prv: module:$n:$s:$v:$c . $a */
      Id sdep = pool_rel2id (pool, solvable->name, solvable->arch, REL_ARCH, 1);
      solvable_add_deparray (solvable, SOLVABLE_PROVIDES, sdep, 0);

      /* Prv: module() */
      solvable_add_deparray (solvable, SOLVABLE_PROVIDES,
                             pool_str2id (pool, "module()", 1),
                             0);

      /* Prv: module($n) */
      solvable_add_deparray (solvable, SOLVABLE_PROVIDES,
                             pool_str2id (pool, nprov, 1),
                             0);

      /* Prv: module($n:$s) = $v */
      solvable_add_deparray (solvable, SOLVABLE_PROVIDES,
                             pool_rel2id (pool,
                                          pool_str2id (pool, nsprov, 1),
                                          pool_str2id (pool, vs, 1),
                                          REL_EQ,
                                          1),
                             0);

      /* Con: module($n) */
      solvable_add_deparray (solvable, SOLVABLE_CONFLICTS,
                             pool_str2id (pool, nprov, 1),
                             0);

      add_module_dependencies (pool, solvable, deps);
#ifdef FUS_TESTING
      /* This is needed when running tests because add_module_rpm_artifacts
       * relies on provides being created
       */
      pool_createwhatprovides (pool);
#endif
      add_module_rpm_artifacts (pool, module, sdep);
    }

  /* Add source packages */
  for (unsigned int i = 0; i < deps->len; i++)
    {
      g_autofree char *name = g_strdup_printf ("module:%s:%s:%s:%u", n, s, vs, i);
      GHashTable *req = modulemd_dependencies_peek_buildrequires (g_ptr_array_index (deps, i));

      add_source_package (repo, req, name);
    }
}

static void
_repo_add_modulemd_from_objects (Repo       *repo,
                                 GPtrArray  *objects,
                                 const char *language,
                                 int         flags)
{
  Pool *pool = repo->pool;

  /* TODO: implement usage of REPO_EXTEND_SOLVABLES, but modulemd doesn't store chksums */

  for (unsigned int i = 0; i < objects->len; i++)
    {
      GObject *object = g_ptr_array_index (objects, i);
      if (!MODULEMD_IS_MODULE (object))
        continue;

      ModulemdModule *module = (ModulemdModule *)object;
      add_module_solvables (repo, module);
    }
  pool_createwhatprovides (pool);

  for (unsigned int i = 0; i < objects->len; i++)
    {
      GObject *object = g_ptr_array_index (objects, i);
      if (!MODULEMD_IS_DEFAULTS (object))
        continue;

      ModulemdDefaults *defaults = (ModulemdDefaults *)object;
      const char *n = modulemd_defaults_peek_module_name (defaults);
      const char *s = modulemd_defaults_peek_default_stream (defaults);
      g_autofree char *mprov = g_strdup_printf ("module(%s:%s)", n, s);

      Id dep = pool_str2id (pool, mprov, 0);
      if (!dep)
        continue;

      Id *pp = pool_whatprovides_ptr (pool, dep);
      for (; *pp; pp++)
        {
          Solvable *s = pool_id2solvable (pool, *pp);
          solvable_add_deparray (s, SOLVABLE_PROVIDES,
                                 pool_str2id (pool, "module-default()", 1),
                                 0);
        }
    }
}

static int
repo_add_modulemd (Repo       *repo,
                   FILE       *fp,
                   const char *language,
                   int         flags)
{
  g_autoptr(GPtrArray) objects = modulemd_objects_from_stream (fp, NULL);

  _repo_add_modulemd_from_objects (repo, objects, language, flags);

  return 0;
}

#ifdef FUS_TESTING
static Repo *
create_test_repo (Pool       *pool,
                  const char *name,
                  const char *type,
                  const char *path)
{
  Repo *repo = repo_create (pool, name);

  FILE *fp = fopen (path, "r");
  /* Open a file with module metadata and load the content to the repo */
  if (g_strcmp0 (type, "modular") == 0)
    repo_add_modulemd (repo, fp, NULL, 0);
  else
    testcase_add_testtags (repo, fp, REPO_LOCALPOOL | REPO_EXTEND_SOLVABLES);

  fclose (fp);

  return repo;
}

#else

static const char *
repomd_find (Repo                 *repo,
             const char           *what,
             const unsigned char **chksump,
             Id                   *chksumtypep)
{
  Pool *pool = repo->pool;
  Dataiterator di;

  dataiterator_init (&di, pool, repo, SOLVID_META, REPOSITORY_REPOMD_TYPE, what, SEARCH_STRING);
  dataiterator_prepend_keyname (&di, REPOSITORY_REPOMD);

  const char *filename = NULL;
  *chksump = NULL;
  *chksumtypep = 0;

  if (dataiterator_step (&di))
    {
      dataiterator_setpos_parent (&di);
      filename = pool_lookup_str (pool, SOLVID_POS, REPOSITORY_REPOMD_LOCATION);
      *chksump = pool_lookup_bin_checksum (pool, SOLVID_POS, REPOSITORY_REPOMD_CHECKSUM, chksumtypep);
    }

  dataiterator_free (&di);

  if (filename && !*chksumtypep)
    {
      g_warning ("No %s file checksum", what);
      filename = NULL;
    }

  return filename;
}

static int
filelist_loadcb (Pool     *pool,
                 Repodata *data,
                 void     *cdata)
{
  FILE *fp;
  const char *fpath, *type;
  Repo *repo = data->repo;

  type = repodata_lookup_str (data, SOLVID_META, REPOSITORY_REPOMD_TYPE);
  if (g_strcmp0 (type, "filelists") != 0)
    return 0;

  fpath = repodata_lookup_str (data, SOLVID_META, REPOSITORY_REPOMD_LOCATION);

  fp = solv_xfopen (fpath, 0);
  repo_add_rpmmd (repo, fp, NULL, REPO_USE_LOADING | REPO_LOCALPOOL | REPO_EXTEND_SOLVABLES);
  fclose (fp);

  return 1;
}

static Repo *
create_repo (Pool       *pool,
             const char *name,
             const char *path)
{
  FILE *fp;
  Id chksumtype;
  const unsigned char *chksum;
  const char *fname;

  Repo *repo = repo_create (pool, name);

  fp = solv_xfopen (pool_tmpjoin (pool, path, "/", "repodata/repomd.xml"), "r");
  repo_add_repomdxml (repo, fp, 0);
  fclose (fp);

  fname = repomd_find (repo, "primary", &chksum, &chksumtype);
  if (fname)
    {
      fp = solv_xfopen (pool_tmpjoin (pool, path, "/", fname), 0);
      repo_add_rpmmd (repo, fp, NULL, 0);
      fclose (fp);
    }

  fname = repomd_find (repo, "group_gz", &chksum, &chksumtype);
  if (!fname)
    fname = repomd_find (repo, "group", &chksum, &chksumtype);
  if (fname)
    {
      fp = solv_xfopen (pool_tmpjoin (pool, path, "/", fname), 0);
      repo_add_comps (repo, fp, 0);
      fclose (fp);
    }

  fname = repomd_find (repo, "filelists", &chksum, &chksumtype);
  if (fname)
    {
      Repodata *data = repo_add_repodata (repo, 0);
      repodata_extend_block (data, repo->start, repo->end - repo->start);
      Id handle = repodata_new_handle (data);
      repodata_set_poolstr (data, handle, REPOSITORY_REPOMD_TYPE, "filelists");
      repodata_set_str (data, handle, REPOSITORY_REPOMD_LOCATION, pool_tmpjoin (pool, path, "/", fname));
      repodata_set_bin_checksum (data, handle, REPOSITORY_REPOMD_CHECKSUM, chksumtype, chksum);
      repodata_add_idarray (data, handle, REPOSITORY_KEYS, SOLVABLE_FILELIST);
      repodata_add_idarray (data, handle, REPOSITORY_KEYS, REPOKEY_TYPE_DIRSTRARRAY);
      repodata_add_flexarray (data, SOLVID_META, REPOSITORY_EXTERNAL, handle);
      repodata_internalize (data);
      repodata_create_stubs (repo_last_repodata (repo));
    }

  pool_createwhatprovides (pool);

  fname = repomd_find (repo, "modules", &chksum, &chksumtype);
  if (fname)
    {
      fp = solv_xfopen (pool_tmpjoin (pool, path, "/", fname), 0);
      repo_add_modulemd (repo, fp, NULL, REPO_LOCALPOOL | REPO_EXTEND_SOLVABLES);
      fclose (fp);
    }

  pool_createwhatprovides (pool);

  return repo;
}
#endif

static Solver *
solve (Pool *pool, Queue *jobs)
{
  Solver *solver = solver_create (pool);

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
      selection_make (pool, &sel, *exclude, SELECTION_NAME | SELECTION_DOTARCH);
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

static void
add_platform_module (const char *platform,
                     const char *arch,
                     Repo       *system)
{
  g_autoptr(GPtrArray) mmd_objects = g_ptr_array_new ();

  g_autoptr(ModulemdModule) module = modulemd_module_new ();
  modulemd_module_set_name (module, "platform");
  modulemd_module_set_stream (module, platform);
  modulemd_module_set_version (module, 0);
  modulemd_module_set_context (module, "00000000");
  modulemd_module_set_arch (module, arch);
  g_ptr_array_add (mmd_objects, module);

  g_autoptr(ModulemdDefaults) defaults = modulemd_defaults_new ();
  modulemd_defaults_set_module_name (defaults, "platform");
  modulemd_defaults_set_default_stream (defaults, platform);
  g_ptr_array_add (mmd_objects, defaults);

  _repo_add_modulemd_from_objects (system, mmd_objects, NULL, 0);
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
      g_debug ("%*c - %s", indent, ' ', pool_solvid2str (pool, p));
    }

  return TRUE;
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

              if (!_install_transaction (pool, pile, &job, 2))
                solv_failed = TRUE;
            }
          else
            {
              g_debug ("Searching combinations for %s", pool_solvid2str (pool, p));
              g_autoptr(GArray) transactions = gather_alternatives (pool, &job);

              if (transactions->len == 0)
                solv_failed = TRUE;

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
                      map_clr (pool->considered, *pp);

                  Queue pjobs = pool->pooljobs;
                  pool->pooljobs = job;
                  for (int j = 0; j < t.count; j++)
                    {
                      Id p = t.elements[j];
                      Solvable *s = pool_id2solvable (pool, p);

                      g_auto(Queue) q;
                      queue_init (&q);
                      Id dep = pool_rel2id (pool, s->name, s->arch, REL_ARCH, 1);
                      pool_whatcontainsdep (pool, SOLVABLE_REQUIRES, dep, &q, 0);

                      g_auto(Queue) j;
                      queue_init (&j);
                      for (int k = 0; k < q.count; k++)
                        {
                          Id p = q.elements[k];
                          queue_empty (&j);
                          queue_push2 (&j, SOLVER_SOLVABLE | SOLVER_INSTALL, p);
                          g_debug ("    Installing %s:", pool_solvid2str (pool, p));

                          if (!_install_transaction (pool, pile, &j, 6))
                            solv_failed = TRUE;
                        }
                    }
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
  static int sel_flags = SELECTION_NAME | SELECTION_PROVIDES | SELECTION_GLOB |
                         SELECTION_CANON | SELECTION_DOTARCH;
  g_auto(Queue) sel;
  queue_init (&sel);
  selection_make (pool, &sel, solvable, sel_flags);
  selection_subtract (pool, &sel, exclude);

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
        {
          static int sel_flags = SELECTION_CANON;
          g_auto(Queue) remove;
          queue_init (&remove);
          selection_make (pool, &remove, pool_solvid2str (pool, q.elements[i]), sel_flags);
          selection_add (pool, &selection, &remove);
        }
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
  pool_setloadcallback (pool, filelist_loadcb, 0);
#endif

  pool_setarch (pool, arch);

  Repo *system = repo_create (pool, "@system");
  pool_set_installed (pool, system);
  if (platform)
    {
      add_platform_module (platform, arch, system);
    }

  g_autoptr(GHashTable) lookaside_repos = g_hash_table_new (g_direct_hash, NULL);
  g_hash_table_add (lookaside_repos, system);
  for (GStrv repo = repos; repo && *repo; repo++)
    {
      g_auto(GStrv) strv = g_strsplit (*repo, ",", 3);
      Repo *r = NULL;
#ifdef FUS_TESTING
        r = create_test_repo (pool, strv[0], strv[1], strv[2]);
#else
        r = create_repo (pool, strv[0], strv[2]);
#endif
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
  g_ptr_array_add (output, NULL);

  return output;
}
