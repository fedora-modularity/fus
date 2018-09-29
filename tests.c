#include <locale.h>
#include <glib.h>
#include <solv/testcase.h>

#define ARCH     "x86_64"
#define PLATFORM "f29"

#define ADD_TEST(name, dir) \
  g_test_add(name, TestData, dir, test_setup, test_run, test_teardown)

typedef struct _test_data {
  GPtrArray *repos;
  GStrv solvables;
  GStrv excluded;
  gchar *expected;
} TestData;

extern GPtrArray *fus_depsolve(const char *, const char *, const GStrv, const GStrv, const GStrv, GError **);

static void
test_run (TestData *td, gconstpointer data)
{
  g_autoptr(GError) error = NULL;
  GStrv repos = (char **)td->repos->pdata;

  g_autoptr(GPtrArray) result = NULL;
  result = fus_depsolve (ARCH, PLATFORM, NULL, repos, td->solvables, &error);
  g_assert_no_error (error);
  g_assert (result != NULL);

  g_autofree char *strres = g_strjoinv ("\n", (char **)result->pdata);
  g_autofree char *diff = testcase_resultdiff (td->expected, strres);
  g_assert_cmpstr (diff, ==, NULL);
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
  g_autofree char *lookaside_path = g_build_filename (testpath, "lookaside.repo", NULL);
  if (g_file_test (lookaside_path, G_FILE_TEST_IS_REGULAR))
    {
      g_ptr_array_add (tdata->repos, g_strdup_printf ("repo-0,lookaside,%s", lookaside_path));
      g_debug (" lookaside: %s", lookaside_path);
    }
  g_autofree char *repo_path = g_build_filename (testpath, "packages.repo", NULL);
  if (g_file_test (repo_path, G_FILE_TEST_IS_REGULAR))
    {
      g_ptr_array_add (tdata->repos, g_strdup_printf ("repo,repo,%s", repo_path));
      g_debug (" repo: %s", repo_path);
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

  ADD_TEST ("/ursine/simple", "ursine");
  ADD_TEST ("/ursine/masking", "masking");
  ADD_TEST ("/require/negative", "negative");
  ADD_TEST ("/require/positive", "positive");
  ADD_TEST ("/require/empty", "empty");

  return g_test_run ();
}
