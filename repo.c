#include "fus.h"

#include <errno.h>
#include <modulemd.h>
#include <solv/chksum.h>
#include <solv/repo_comps.h>
#include <solv/repo_repomdxml.h>
#include <solv/repo_rpmmd.h>
#include <solv/repo_solv.h>
#include <solv/repo_write.h>
#include <solv/solv_xfopen.h>
#include <solv/testcase.h>
#include <stdint.h>

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
Repo *
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

/* Returns checksum as a hexadecimal string which we can use as filename */
static gchar *
chksum_string_for_filepath (GChecksumType type, const char *filepath)
{
  gsize len = 0;
  g_autofree gchar *data = NULL;
  if (!g_file_get_contents (filepath, &data, &len, NULL))
    return NULL;

  return g_compute_checksum_for_string (type, data, len);
}

static gboolean
checksum_matches (const char          *filepath,
                  const unsigned char *chksum,
                  Id                   chksumtype)
{
  gsize len = 0;
  g_autofree gchar *contents = NULL;
  if (!g_file_get_contents (filepath, &contents, &len, NULL))
    return FALSE;

  Chksum *filechksum = solv_chksum_create (chksumtype);
  if (!filechksum)
    return FALSE;
  solv_chksum_add (filechksum, contents, len);

  int clen;
  const unsigned char *binsum = solv_chksum_get (filechksum, &clen);
  int ret = memcmp (chksum, binsum, clen);
  solv_chksum_free (filechksum, NULL);

  return ret == 0;
}

static gboolean
download_to_path (SoupSession  *session,
                  const char   *url,
                  const char   *path,
                  GError      **error)
{
  g_autoptr(SoupURI) parsed = soup_uri_new (url);
  g_autoptr(GFile) file = g_file_new_for_path (path);

  if (!SOUP_URI_VALID_FOR_HTTP (parsed))
    {
      g_autoptr(GFile) local = g_file_new_for_path (url);
      return g_file_copy (local, file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error);
    }

  g_debug ("Downloading %s to %s", url, path);

  g_autoptr(SoupMessage) msg = soup_message_new_from_uri ("GET", parsed);
  g_autoptr(GInputStream) istream = soup_session_send (session, msg, NULL, error);
  if (!SOUP_STATUS_IS_SUCCESSFUL (msg->status_code))
    return FALSE;

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
  const unsigned char *chksum;

  const char *mdname = repomd_find (repo, type, &chksum, &chksumtype);
  if (!mdname)
    return NULL;

  /* mdname should be "repodata/$(chksum)-$(type).xml.gz" */
  const char *ext = strchr (mdname, '.');
  const char *fpath = pool_tmpjoin (repo->pool, cachedir, "/", type);
  fpath = pool_tmpappend (repo->pool, fpath, ext, 0);

  if (!g_file_test (fpath, G_FILE_TEST_IS_REGULAR) ||
      !checksum_matches (fpath, chksum, chksumtype))
    {
      g_autoptr(GError) error = NULL;
      const char *mdurl = pool_tmpjoin (repo->pool, repo_url, "/", mdname);
      if (!download_to_path (session, mdurl, fpath, &error))
        {
          g_warning ("Could not download %s: %s", mdurl, error->message);
          return NULL;
        }
    }

  return fpath;
}

static void
switch_to_cached_repo (Repo        *repo,
                       Repodata    *repodata,
                       const char  *repoext,
                       const char  *cachepath)
{
  int i;
  FILE *fp;

  if (!repoext && repodata)
    return;

  for (i = repo->start; i < repo->end; i++)
    if (repo->pool->solvables[i].repo != repo)
      break;

  if (i < repo->end)
    return; /* not a simple block */

  fp = g_fopen (cachepath, "rb");
  if (!fp)
    return;

  /* main repo */
  if (!repoext)
    {
      repo_empty (repo, 1);
      if (repo_add_solv (repo, fp, SOLV_ADD_NO_STUBS))
        {
          g_warning ("Could not add solvables from cache file");
          return;
        }
    }
  else
    {
      int flags = REPO_USE_LOADING | REPO_EXTEND_SOLVABLES | REPO_LOCALPOOL;
      /* make sure repodata contains complete repo */
      /* (this is how repodata_write saves it) */
      repodata_extend_block (repodata, repo->start, repo->end - repo->start);
      repodata->state = REPODATA_LOADING;
      repo_add_solv (repo, fp, flags);
      repodata->state = REPODATA_AVAILABLE; /* in case the load failed */
    }

  fclose (fp);
}

static gboolean
write_repo_cache (Repo         *repo,
                  Repodata     *repodata,
                  const char   *repoext,
                  const gchar  *cachename)
{
  FILE *fp = g_fopen (cachename, "wb");
  if (!fp)
    {
      g_warning ("Could not open cache file %s: %s", cachename, g_strerror (errno));
      return FALSE;
    }

  if (!repodata)
    repo_write (repo, fp); /* main repo */
  else
    repodata_write (repodata, fp);

  if (fclose (fp))
    {
      g_warning ("Error when closing %s: %s", cachename, g_strerror (errno));
      g_unlink (cachename);
      return FALSE;
    }

  /* Switch to just saved repo to activate paging and save memory */
  switch_to_cached_repo (repo, repodata, repoext, cachename);

  return TRUE;
}

static gboolean
load_cached_repo (Repo       *repo,
                  const char *cachefn,
                  const char *repoext)
{
  int ret, flags = 0;

  if (!g_file_test (cachefn, G_FILE_TEST_IS_REGULAR))
    {
      g_debug ("Cache %s for repo %s not found", cachefn, repo->name);
      return FALSE;
    }

  FILE *fp = g_fopen (cachefn, "rb");
  if (!fp)
    return FALSE;

  if (repoext)
    flags = REPO_USE_LOADING | REPO_EXTEND_SOLVABLES | REPO_LOCALPOOL;

  ret = repo_add_solv (repo, fp, flags);
  fclose (fp);

  return ret == 0;
}

static inline gchar *
get_repo_cachedir (const char *name)
{
  return g_build_filename (g_get_user_cache_dir (), "fus", name, NULL);
}

static void
remove_files_by_ext (const char *dirpath,
                     const char *ext)
{
  g_autoptr(GDir) dir = g_dir_open (dirpath, 0, NULL);
  if (!dir)
    return;

  const gchar *fname;
  while ((fname = g_dir_read_name (dir)) != NULL)
    {
      if (g_str_has_suffix (fname, ext))
        {
          g_autofree gchar *path = g_build_filename (dirpath, fname, NULL);
          g_unlink (path);
        }
    }
}

int
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

  g_autofree gchar *cachedir = get_repo_cachedir (repo->name);

  /* repo ext cache name is $(CHECKSUM(REPOMD)).solvx */
  const char *cachefn = pool_tmpjoin (pool, cachedir, "/", repo->appdata);
  cachefn = pool_tmpappend (pool, cachefn, ".solvx", 0);
  if (load_cached_repo (repo, cachefn, type))
    {
      g_debug ("Using cached repo for \"%s\" filelists", repo->name);
      return 1;
    }

  /* Cleanup old libsolv cache files (if any) */
  remove_files_by_ext (cachedir, ".solvx");

  path = repodata_lookup_str (data, SOLVID_META, REPOSITORY_REPOMD_LOCATION);
  if (!path)
    return 0;

  const char *destdir = pool_tmpjoin (pool, cachedir, "/", "repodata");
  fname = download_repo_metadata (session, repo, type, path, destdir);
  fp = solv_xfopen (fname, 0);
  if (!fp)
    {
      g_warning ("Could not open filelists %s: %s", fname, g_strerror (errno));
      return 0;
    }
  repo_add_rpmmd (repo, fp, NULL, REPO_USE_LOADING | REPO_LOCALPOOL | REPO_EXTEND_SOLVABLES);
  fclose (fp);

  if (write_repo_cache (repo, data, type, cachefn))
    g_debug ("Wrote cache file %s for repo \"%s\"", cachefn, repo->name);

  return 1;
}

Repo *
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

  g_autofree gchar *cachedir = get_repo_cachedir (name);

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

  gchar *mdchksum = chksum_string_for_filepath (G_CHECKSUM_SHA256, fname);
  if (!mdchksum)
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
  /* Save repomd checksum to the repo's appdata so we just calculate it once */
  repo->appdata = mdchksum;

  /* repo main cache name is $(CHECKSUM(REPOMD)).solv */
  const char *cachefn = pool_tmpjoin (pool, cachedir, "/", mdchksum);
  cachefn = pool_tmpappend (pool, cachefn, ".solv", 0);
  if (load_cached_repo (repo, cachefn, NULL))
    {
      g_debug ("Using cached repo for \"%s\"", name);
      return repo;
    }

  /* Cleanup old libsolv cache files (if any) */
  remove_files_by_ext (cachedir, ".solv");

  repo_add_repomdxml (repo, fp, 0);
  fclose (fp);

  fname = download_repo_metadata (session, repo, "primary", path, destdir);
  fp = solv_xfopen (fname, "r");
  if (fp != NULL)
    {
      repo_add_rpmmd (repo, fp, NULL, 0);
      fclose (fp);
    }

  fname = download_repo_metadata (session, repo, "group_gz", path, destdir);
  if (!fname)
    fname = download_repo_metadata (session, repo, "group", path, destdir);
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
    }

  pool_createwhatprovides (pool);

  fname = download_repo_metadata (session, repo, "modules", path, destdir);
  fp = solv_xfopen (fname, "r");
  if (fp != NULL)
    {
      g_autoptr(GError) e = NULL;
      if (!repo_add_modulemd (repo, fp, NULL, REPO_LOCALPOOL | REPO_EXTEND_SOLVABLES, &e))
        g_warning ("Could not add modules from repo %s: %s", name, e->message);
      fclose (fp);
    }

  pool_createwhatprovides (pool);

  if (write_repo_cache (repo, NULL, NULL, cachefn))
    g_debug ("Wrote cache file %s for repo \"%s\" filelists", cachefn, repo->name);

  repodata_create_stubs (repo_last_repodata (repo));

  return repo;
}
#endif /* FUS_TESTING */

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

Repo *
create_system_repo (Pool *pool, const char *platform, const char *arch)
{
  Repo *system = repo_create (pool, "@system");
  if (platform)
    add_platform_module (platform, arch, system);
  return system;
}
