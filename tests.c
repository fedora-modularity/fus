#include <locale.h>
#include <glib.h>
#include <solv/testcase.h>

#define ARCH     "x86_64"
#define PLATFORM "f29"

#define ADD_TEST(name, dir) \
  g_test_add(name, TestData, dir, test_setup, test_run, test_teardown)

#define ADD_SOLV_FAIL_TEST(name, dir) \
  g_test_add(name, TestData, dir, test_setup, test_broken_dep, test_teardown)

typedef struct _test_data {
  GPtrArray *repos;
  GStrv solvables;
  GStrv excluded;
  gchar *expected;
} TestData;

extern GPtrArray *fus_depsolve(const char *, const char *, const GStrv, const GStrv, const GStrv, GError **);

static void
test_broken_dep (TestData *td, gconstpointer data)
{
  GStrv repos = (char **)td->repos->pdata;
  const gchar *dir = data;

  if (g_test_subprocess ())
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GPtrArray) result = NULL;
      g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                             "*Can't resolve all solvables*");
      result = fus_depsolve (ARCH, PLATFORM, NULL, repos, td->solvables, &error);
      g_assert (result != NULL);
      g_assert_no_error (error);
      g_autofree char *strres = g_strjoinv ("\n", (char **)result->pdata);
      g_autofree char *diff = testcase_resultdiff (td->expected, strres);
      g_assert_cmpstr (diff, ==, NULL);
      g_test_assert_expected_messages ();
      return;
    }

  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_passed ();

  const char *probfile = g_test_get_filename (G_TEST_DIST, dir, "problems", NULL);
  if (g_file_test (probfile, G_FILE_TEST_IS_REGULAR))
    {
      g_autofree char *content = NULL;
      g_autoptr(GError) error = NULL;
      g_file_get_contents (probfile, &content, NULL, &error);
      g_assert_no_error (error);
      g_assert (content != NULL);
      g_test_trap_assert_stdout (content);
    }
}

static void
test_fail_invalid_solvable (void)
{
  GStrv repos = NULL;
  gchar *solvables[] = {"invalid", NULL};

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) result = NULL;
  g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                         "*Nothing matches 'invalid'*");
  result = fus_depsolve (ARCH, PLATFORM, NULL, repos, solvables, &error);
  g_assert (result == NULL);
  g_assert_cmpstr (error->message, ==, "No solvables matched");
  g_test_assert_expected_messages ();
}

static void
test_fail_no_solvables (void)
{
  GStrv repos = NULL;
  GStrv solvables = NULL;

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) result = NULL;
  result = fus_depsolve (ARCH, PLATFORM, NULL, repos, solvables, &error);
  g_assert_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED);
  g_assert (result == NULL);
  g_assert_cmpstr (error->message, ==, "No solvables matched");
}

static void
test_invalid_repo (void)
{
  gchar *repos[] = {"repo,repo,invalid/packages.repo"};
  gchar *solvables[] = {"invalid"};

  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) result = NULL;
  result = fus_depsolve (ARCH, PLATFORM, NULL, repos, solvables, &error);
  g_assert (result == NULL);
  g_assert_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED);
  g_assert_cmpstr (error->message, ==,
                   "Could not open invalid/packages.repo: No such file or directory");
}

static void
test_run (TestData *td, gconstpointer data)
{
  GStrv repos = (char **)td->repos->pdata;

  if (g_test_subprocess ())
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GPtrArray) result = NULL;
      result = fus_depsolve (ARCH, PLATFORM, NULL, repos, td->solvables, &error);
      g_assert_no_error (error);
      g_assert (result != NULL);

      g_autofree char *strres = g_strjoinv ("\n", (char **)result->pdata);
      g_autofree char *diff = testcase_resultdiff (td->expected, strres);
      g_assert_cmpstr (diff, ==, NULL);

      return;
    }

  g_test_trap_subprocess (NULL, 0, 0);
  g_test_trap_assert_passed ();
  g_test_trap_assert_stdout_unmatched ("*Problem * / *");
  g_test_trap_assert_stderr_unmatched ("*Nothing matches*");
  g_test_trap_assert_stderr_unmatched ("*Can't resolve all solvables*");
}

static void
test_teardown (TestData *tdata, gconstpointer data)
{
  g_ptr_array_unref (tdata->repos);
  if (tdata->solvables)
    g_strfreev (tdata->solvables);
  if (tdata->excluded)
    g_strfreev (tdata->excluded);
  if (tdata->expected)
    g_free (tdata->expected);
}

static void
test_setup (TestData *tdata, gconstpointer data)
{
  g_autoptr(GError) error = NULL;
  const char *testname = data;

  g_debug ("Setting up test %s", testname);

  const char *testpath = g_test_get_filename (G_TEST_DIST, testname, NULL);
  g_assert (g_file_test (testpath, G_FILE_TEST_IS_DIR));

  /* Read input file */
  g_autofree char *inpath = g_build_filename (testpath, "input", NULL);
  g_assert (g_file_test (inpath, G_FILE_TEST_IS_REGULAR));
  tdata->solvables = g_malloc (2 * sizeof(tdata->solvables));
  tdata->solvables[0] = g_strconcat ("@", inpath, NULL);
  tdata->solvables[1] = NULL;

  /* Read expected output file */
  g_autofree char *outpath = g_build_filename (testpath, "expected", NULL);
  g_assert (g_file_test (outpath, G_FILE_TEST_IS_REGULAR));
  g_file_get_contents (outpath, &tdata->expected, NULL, &error);
  g_assert_no_error (error);
  g_assert (tdata->expected);

  /* Read excluded packages file */
  g_autofree char *exclude_path = g_build_filename (testpath, "excludes", NULL);
  if (g_file_test (exclude_path, G_FILE_TEST_IS_REGULAR))
    {
      g_autofree char *excluded = NULL;
      g_file_get_contents (exclude_path, &excluded, NULL, &error);
      g_assert_no_error (error);
      g_assert (excluded != NULL);
      tdata->excluded = g_strsplit (excluded, "\n", -1);
      /* Remove spurious empty string */
      size_t len = g_strv_length (tdata->excluded);
      if (len && !*tdata->excluded[len - 1])
        {
          g_free (tdata->excluded[len - 1]);
          tdata->excluded[len - 1] = NULL;
        }
    }

  /* Read local test repositories */
  tdata->repos = g_ptr_array_new_full (4, g_free);
  g_autofree char *repo_path = g_build_filename (testpath, "packages.repo", NULL);
  if (g_file_test (repo_path, G_FILE_TEST_IS_REGULAR))
    {
      g_ptr_array_add (tdata->repos, g_strdup_printf ("repo,repo,%s", repo_path));
      g_debug (" repo: %s", repo_path);
    }
  g_autofree char *lookaside_path = g_build_filename (testpath, "lookaside.repo", NULL);
  if (g_file_test (lookaside_path, G_FILE_TEST_IS_REGULAR))
    {
      g_ptr_array_add (tdata->repos, g_strdup_printf ("repo-0,lookaside,%s", lookaside_path));
      g_debug (" lookaside: %s", lookaside_path);
    }
  g_autofree char *yaml_path = g_build_filename (testpath, "modules.yaml", NULL);
  if (g_file_test (yaml_path, G_FILE_TEST_IS_REGULAR))
    {
      g_ptr_array_add (tdata->repos, g_strdup_printf ("yaml,modular,%s", yaml_path));
      g_debug (" yaml: %s", yaml_path);
    }
  g_ptr_array_add (tdata->repos, NULL);
}

int main (int argc, char **argv)
{
  setlocale (LC_ALL, "");

  g_test_init (&argc, &argv, NULL);
  g_test_bug_base ("https://github.com/fedora-modularity/fus/issues");

  /*
   * By default, g_test_init set log flags to be fatal on CRITICAL and WARNING
   * levels. We overwrite this setting here so that fus does not abort on
   * g_warning. This allows us to check both for warning messages and the
   * result of the solving.
   */
  g_log_set_always_fatal (G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

  ADD_TEST ("/ursine/simple", "ursine");
  ADD_TEST ("/ursine/masking", "masking");
  ADD_TEST ("/require/negative", "negative");
  ADD_TEST ("/require/positive", "positive");
  ADD_TEST ("/require/empty", "empty");
  ADD_TEST ("/require/alternatives", "alternatives");
  ADD_TEST ("/module/empty", "empty-module");
  ADD_TEST ("/solvable-selection/pull-bare", "pull-bare");
  ADD_TEST ("/solvable-selection/pull-from-default-stream", "pull-default-module");
  ADD_TEST ("/solvable-selection/explicit-nevra", "explicit-nevra");

  g_test_add_func ("/fail/invalid-repo", test_invalid_repo);
  g_test_add_func ("/fail/no-solvables", test_fail_no_solvables);
  g_test_add_func ("/fail/invalid-solvable", test_fail_invalid_solvable);

  ADD_TEST ("/ursine/default-stream-dep", "default-stream");
  ADD_TEST ("/ursine/prefer-over-non-default-stream", "non-default-stream");

  ADD_TEST ("/lookaside/same-repo", "input-as-lookaside");

  ADD_SOLV_FAIL_TEST ("/fail/ursine/broken", "ursine-broken");
  ADD_SOLV_FAIL_TEST ("/fail/module/broken", "module-broken");
  ADD_SOLV_FAIL_TEST ("/fail/moddep/broken", "moddep-broken");

  return g_test_run ();
}
