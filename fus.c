#include <errno.h>
#include <stdint.h>
#include <glib.h>
#include <gio/gio.h>
#include <modulemd.h>
#include <solv/chksum.h>
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
#include <libsoup/soup.h>

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
parse_module_stream_requires (Pool       *pool,
                              const char *module,
                              GStrv       streams)
{
  Id req_neg = 0, req_pos = 0;
  for (GStrv ss = streams; *ss; ss++)
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

      g_autofree char *nsprov = g_strdup_printf (TMPL_NSPROV, module, s);
      *r = dep_or_rel (pool, *r, pool_str2id (pool, nsprov, 1), REL_OR);
    }

  g_autofree char *nprov = g_strdup_printf (TMPL_NPROV, module);
  Id req = pool_str2id (pool, nprov, 1);
  if (req_pos)
    req = dep_or_rel (pool, req, req_pos, REL_WITH);
  else if (req_neg)
    req = dep_or_rel (pool, req, req_neg, REL_WITHOUT);

  return req;
}

typedef GStrv (*module_func_t)(ModulemdDependencies *);
typedef GStrv (*stream_func_t)(ModulemdDependencies *, const char *);

static Id
parse_module_requires (Pool                 *pool,
                       ModulemdDependencies *deps,
                       module_func_t         modules_get,
                       stream_func_t         streams_get)
{
  Id require = 0;
  g_auto(GStrv) namesv = modules_get (deps);
  for (GStrv n = namesv; n && *n; n++)
    {
      g_auto(GStrv) reqv = streams_get (deps, *n);
      Id r = parse_module_stream_requires (pool, *n, reqv);
      require = dep_or_rel (pool, require, r, REL_AND);
    }

  return require;
}

const module_func_t buildtime_modules_get =
  modulemd_dependencies_get_buildtime_modules_as_strv;
const stream_func_t buildtime_streams_get =
  modulemd_dependencies_get_buildtime_streams_as_strv;

static void
add_source_package (Repo                 *repo,
                    ModulemdDependencies *deps,
                    const char           *name)
{
  Pool *pool = repo->pool;
  Solvable *solvable = pool_id2solvable (pool, repo_add_solvable(repo));
  solvable->name = pool_str2id (pool, name, 1);
  solvable->evr = ID_EMPTY;
  solvable->arch = ARCH_SRC;

  Id requires = parse_module_requires (pool,
                                       deps,
                                       buildtime_modules_get,
                                       buildtime_streams_get);
  solvable_add_deparray (solvable, SOLVABLE_REQUIRES, requires, 0);
}

const module_func_t runtime_modules_get =
  modulemd_dependencies_get_runtime_modules_as_strv;
const stream_func_t runtime_streams_get =
  modulemd_dependencies_get_runtime_streams_as_strv;

static void
add_module_dependencies (Pool      *pool,
                         Solvable  *solvable,
                         GPtrArray *deps)
{
  Id requires = 0;
  for (unsigned int i = 0; i < deps->len; i++)
    {
      ModulemdDependencies *dep = g_ptr_array_index (deps, i);
      Id require = parse_module_requires (pool,
                                          dep,
                                          runtime_modules_get,
                                          runtime_streams_get);
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
add_module_rpm_artifacts (Pool                   *pool,
                          ModulemdModuleStreamV2 *module,
                          Id                      sdep)
{
  g_auto(GStrv) rpm_artifacts = modulemd_module_stream_v2_get_rpm_artifacts_as_strv (module);
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

/* TODO: implement usage of REPO_EXTEND_SOLVABLES, but modulemd doesn't store chksums */
static void
add_module_solvables (Repo                 *repo,
                      ModulemdModuleStream *module)
{
  Pool *pool = repo->pool;

  const char *n = modulemd_module_stream_get_module_name (module);
  g_autofree char *nprov = g_strdup_printf (TMPL_NPROV, n);
  const char *s = modulemd_module_stream_get_stream_name (module);
  g_autofree char *nsprov = g_strdup_printf (TMPL_NSPROV, n, s);
  const uint64_t v = modulemd_module_stream_get_version (module);
  g_autofree char *vs = g_strdup_printf ("%" G_GUINT64_FORMAT, v);
  const char *c = modulemd_module_stream_get_context (module);
  const char *a = modulemd_module_stream_v2_get_arch ((ModulemdModuleStreamV2 *) module);
  if (!a)
    a = "noarch";

  GPtrArray *deps = modulemd_module_stream_v2_get_dependencies ((ModulemdModuleStreamV2 *) module);

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
      add_module_rpm_artifacts (pool, (ModulemdModuleStreamV2 *) module, sdep);
    }

  /* Add source packages */
  for (unsigned int i = 0; i < deps->len; i++)
    {
      g_autofree char *name = g_strdup_printf ("module:%s:%s:%s:%u", n, s, vs, i);
      ModulemdDependencies *req = g_ptr_array_index (deps, i);

      add_source_package (repo, req, name);
    }
}

static void
_repo_add_modulemd_streams (Repo       *repo,
                            GPtrArray  *streams,
                            const char *language,
                            int         flags)
{
  for (unsigned int i = 0; i < streams->len; i++)
    add_module_solvables (repo, g_ptr_array_index (streams, i));

  pool_createwhatprovides (repo->pool);
}

static void
_repo_add_modulemd_defaults (Repo             *repo,
                             ModulemdDefaults *defaults)
{
  Pool *pool = repo->pool;
  const char *n = modulemd_defaults_get_module_name (defaults);
  const char *s = modulemd_defaults_v1_get_default_stream ((ModulemdDefaultsV1 *)defaults, NULL);
  g_autofree char *mprov = g_strdup_printf ("module(%s:%s)", n, s);

  Id dep = pool_str2id (pool, mprov, 0);
  if (!dep)
    return;

  Id *pp = pool_whatprovides_ptr (pool, dep);
  for (; *pp; pp++)
  {
    Solvable *s = pool_id2solvable (pool, *pp);
    solvable_add_deparray (s, SOLVABLE_PROVIDES,
                           pool_str2id (pool, "module-default()", 1),
                           0);
  }
}

static gboolean
repo_add_modulemd (Repo        *repo,
                   FILE        *fp,
                   const char  *language,
                   int          flags,
                   GError     **error)
{
  g_autoptr(GPtrArray) failures = NULL;
  g_autoptr(ModulemdModuleIndex) index = modulemd_module_index_new ();
  if (!modulemd_module_index_update_from_stream (index, fp, TRUE, &failures, error))
    {
      if (*error)
        return FALSE;

      for (unsigned int i = 0; i < failures->len; i++)
        {
          ModulemdSubdocumentInfo *info = g_ptr_array_index (failures, i);
          const GError *e = modulemd_subdocument_info_get_gerror (info);
          g_warning ("Failed reading from stream: %s", e->message);
        }

      return FALSE;
    }

  /* Make sure we are working with the expected version of modulemd documents */
  if (!modulemd_module_index_upgrade_streams (index, MD_MODULESTREAM_VERSION_TWO, error) ||
      !modulemd_module_index_upgrade_defaults (index, MD_DEFAULTS_VERSION_ONE, error))
    return FALSE;

  g_auto(GStrv) modnames = modulemd_module_index_get_module_names_as_strv (index);
  for (GStrv names = modnames; names && *names; names++)
    {
      ModulemdModule *mod = modulemd_module_index_get_module (index, *names);

      GPtrArray *streams = modulemd_module_get_all_streams (mod);
      _repo_add_modulemd_streams (repo, streams, language, flags);

      ModulemdDefaults *defaults = modulemd_module_get_defaults (mod);
      if (defaults)
        _repo_add_modulemd_defaults (repo, defaults);
    }

  return TRUE;
}

#ifdef FUS_TESTING
static Repo *
create_test_repo (Pool        *pool,
                  const char  *name,
                  const char  *type,
                  const char  *path,
                  GError     **error)
{
  FILE *fp = fopen (path, "r");
  if (!fp)
    {
      g_set_error (error,
                   G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   "Could not open %s: %s",
                   path, g_strerror (errno));
      return NULL;
    }

  Repo *repo = repo_create (pool, name);

  /* Open a file with module metadata and load the content to the repo */
  if (g_strcmp0 (type, "modular") == 0)
    {
      if (!repo_add_modulemd (repo, fp, NULL, 0, error))
        {
          fclose (fp);
          return NULL;
        }
    }
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

static gboolean
checksum_matches (const char          *filepath,
                  const unsigned char *chksum,
                  Id                   chksum_type)
{
  int ret = 0;
  gsize len = 0;
  Chksum *file_sum, *md_sum = NULL;
  g_autoptr(GFile) file = g_file_new_for_path (filepath);
  g_autoptr(GBytes) bytes = g_file_load_bytes (file, NULL, NULL, NULL);
  gconstpointer bp = g_bytes_get_data (bytes, &len);

  file_sum = solv_chksum_create (chksum_type);
  solv_chksum_add (file_sum, bp, len);

  md_sum = solv_chksum_create_from_bin (chksum_type, chksum);

  ret = solv_chksum_cmp (file_sum, md_sum);

  solv_chksum_free (file_sum, NULL);
  solv_chksum_free (md_sum, NULL);

  return ret == 1;
}

static gboolean
download_to_path (SoupSession  *session,
                  const char   *url,
                  const char   *path,
                  GError      **error)
{
  g_autoptr(SoupURI) parsed = soup_uri_new (url);
  if (!SOUP_URI_VALID_FOR_HTTP (parsed))
    {
      g_debug ("%s is already a local file", url);
      return TRUE;
    }

  g_debug ("Downloading %s to %s", url, path);

  g_autoptr(SoupMessage) msg = soup_message_new_from_uri ("GET", parsed);
  g_autoptr(GInputStream) istream = soup_session_send (session, msg, NULL, error);
  if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    return FALSE;

  g_autoptr(GFile) file = g_file_new_for_path (path);
  g_autoptr(GFileOutputStream) ostream = g_file_replace (file,
                                                         NULL,
                                                         FALSE,
                                                         G_FILE_CREATE_REPLACE_DESTINATION,
                                                         NULL,
                                                         error);
  if (!ostream)
    return FALSE;

  if (g_output_stream_splice (G_OUTPUT_STREAM (ostream), istream, 0, NULL, error) == -1)
    return FALSE;

  return TRUE;
}

static const char *
download_repo_metadata (SoupSession *session,
                        Repo        *repo,
                        const char  *type,
                        const char  *repo_url,
                        const char  *cachedir)
{
  Id chksumtype;
  const char *fpath, *fname;
  const unsigned char *chksum;

  fname = repomd_find (repo, type, &chksum, &chksumtype);
  if (!fname)
    return NULL;

  fpath = pool_tmpjoin (repo->pool, cachedir, "/", fname);
  if (!g_file_test (fpath, G_FILE_TEST_IS_REGULAR) ||
      !checksum_matches (fpath, chksum, chksumtype))
    {
      g_autoptr(GError) error = NULL;
      const char *furl = pool_tmpjoin (repo->pool, repo_url, "/", fname);
      if (!download_to_path (session, furl, fpath, &error))
        {
          g_warning ("Could not download %s: %s", furl, error->message);
          return NULL;
        }
    }

  return fpath;
}

static int
filelist_loadcb (Pool     *pool,
                 Repodata *data,
                 void     *cdata)
{
  FILE *fp;
  const char *path, *type, *fname;
  Repo *repo = data->repo;
  SoupSession *session = (SoupSession *) cdata;

  type = repodata_lookup_str (data, SOLVID_META, REPOSITORY_REPOMD_TYPE);
  if (g_strcmp0 (type, "filelists") != 0)
    return 0;

  path = repodata_lookup_str (data, SOLVID_META, REPOSITORY_REPOMD_LOCATION);
  if (!path)
    return 0;

  g_autofree gchar *cachedir = g_build_filename (g_get_user_cache_dir (),
                                                 "fus", repo->name, NULL);

  fname = download_repo_metadata (session, repo, type, path, cachedir);
  fp = solv_xfopen (fname, 0);
  if (!fp)
    {
      g_warning ("Could not open filelists %s: %s", fname, g_strerror (errno));
      return 0;
    }
  repo_add_rpmmd (repo, fp, NULL, REPO_USE_LOADING | REPO_LOCALPOOL | REPO_EXTEND_SOLVABLES);
  fclose (fp);

  return 1;
}

static Repo *
create_repo (Pool         *pool,
             SoupSession  *session,
             const char   *name,
             const char   *path,
             GError      **error)
{
  FILE *fp;
  Id chksumtype;
  const unsigned char *chksum;
  const char *fname, *url, *destdir;

  g_autofree gchar *cachedir = g_build_filename (g_get_user_cache_dir (),
                                                 "fus", name, NULL);

  destdir = pool_tmpjoin (pool, cachedir, "/", "repodata");
  if (g_mkdir_with_parents (destdir, 0700) == -1)
    {
      g_set_error (error,
                   G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   "Could not create cache dir %s: %s",
                   destdir, g_strerror (errno));
      return NULL;
    }

  /* We can't use `download_repo_metadata` for repomd.xml because it's a
   * special case: it's always downloaded if the path provided is a repo URL.
   */
  url = pool_tmpjoin (pool, path, "/", "repodata/repomd.xml");
  fname = pool_tmpjoin (pool, destdir, "/", "repomd.xml");
  if (!download_to_path (session, url, fname, error))
    return NULL;

  fp = solv_xfopen (fname, "r");
  if (!fp)
    {
      g_set_error (error,
                   G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
                   "Could not open repomd.xml for %s: %s",
                   path, g_strerror (errno));
      return NULL;
    }

  Repo *repo = repo_create (pool, name);
  repo_add_repomdxml (repo, fp, 0);
  fclose (fp);

  fname = download_repo_metadata (session, repo, "primary", path, cachedir);
  fp = solv_xfopen (fname, "r");
  if (fp != NULL)
    {
      repo_add_rpmmd (repo, fp, NULL, 0);
      fclose (fp);
    }

  fname = download_repo_metadata (session, repo, "group_gz", path, cachedir);
  if (!fname)
    fname = download_repo_metadata (session, repo, "group", path, cachedir);
  fp = solv_xfopen (fname, "r");
  if (fp != NULL)
    {
      repo_add_comps (repo, fp, 0);
      fclose (fp);
    }

  /* filelists metadata will only be downloaded if/when needed */
  fname = repomd_find (repo, "filelists", &chksum, &chksumtype);
  if (fname)
    {
      Repodata *data = repo_add_repodata (repo, 0);
      repodata_extend_block (data, repo->start, repo->end - repo->start);
      Id handle = repodata_new_handle (data);
      repodata_set_poolstr (data, handle, REPOSITORY_REPOMD_TYPE, "filelists");
      repodata_set_str (data, handle, REPOSITORY_REPOMD_LOCATION, path);
      repodata_set_bin_checksum (data, handle, REPOSITORY_REPOMD_CHECKSUM, chksumtype, chksum);
      repodata_add_idarray (data, handle, REPOSITORY_KEYS, SOLVABLE_FILELIST);
      repodata_add_idarray (data, handle, REPOSITORY_KEYS, REPOKEY_TYPE_DIRSTRARRAY);
      repodata_add_flexarray (data, SOLVID_META, REPOSITORY_EXTERNAL, handle);
      repodata_internalize (data);
      repodata_create_stubs (repo_last_repodata (repo));
    }

  pool_createwhatprovides (pool);

  fname = download_repo_metadata (session, repo, "modules", path, cachedir);
  fp = solv_xfopen (fname, "r");
  if (fp != NULL)
    {
      g_autoptr(GError) e = NULL;
      if (!repo_add_modulemd (repo, fp, NULL, REPO_LOCALPOOL | REPO_EXTEND_SOLVABLES, &e))
        g_warning ("Could not add modules from repo %s: %s", name, e->message);
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
  g_autoptr(ModulemdModuleStream) module =
    modulemd_module_stream_new (MD_MODULESTREAM_VERSION_TWO, "platform", platform);
  modulemd_module_stream_set_version (module, 0);
  modulemd_module_stream_set_context (module, "00000000");
  modulemd_module_stream_v2_set_arch ((ModulemdModuleStreamV2 *) module, arch);
  add_module_solvables (system, module);

  g_autoptr(ModulemdDefaultsV1) defaults = modulemd_defaults_v1_new ("platform");
  modulemd_defaults_v1_set_default_stream (defaults, platform, NULL);
  _repo_add_modulemd_defaults (system, (ModulemdDefaults *)defaults);
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

  return output;
}
