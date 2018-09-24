#include <locale.h>
#include <stdlib.h>
#include <sys/utsname.h>
#include <glib.h>

extern GPtrArray *fus_depsolve(const char *, const char *, const GStrv, const GStrv, const GStrv, GError **);

static inline void
print_package (void *package, gpointer user_data)
{
  g_print ("%s\n", (const char *)package);
}

static inline void G_GNUC_NORETURN
exiterr (GError *error)
{
  g_printerr ("Error: %s\n", error->message);
  exit (EXIT_FAILURE);
}

int
main (int   argc,
      char *argv[])
{
  setlocale (LC_ALL, "");
  g_autoptr(GError) err = NULL;

  static char *arch = NULL;
  static char *platform = NULL;
  GStrv static solvables = NULL;
  GStrv static repos = NULL;
  GStrv static exclude_packages = NULL;
  static gboolean verbose = FALSE;
  static const GOptionEntry opts[] = {
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Show extra debugging information", NULL },
    { "arch", 'a', 0, G_OPTION_ARG_STRING, &arch, "Architecture to work with", "ARCH" },
    { "repo", 'r', 0, G_OPTION_ARG_STRING_ARRAY, &repos, "Information about repo (id,type,path)", "REPO" },
    { "platform", 'p', 0, G_OPTION_ARG_STRING, &platform, "Emulate this stream of a platform", "STREAM" },
    { "exclude", 0, 0, G_OPTION_ARG_STRING_ARRAY, &exclude_packages, "Exclude this package", "NAME" },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &solvables, "Things to resolve", "SOLVABLE…" },
    { NULL }
  };

  g_autoptr(GOptionContext) opt_ctx = g_option_context_new ("- Funny solver");
  g_option_context_add_main_entries (opt_ctx, opts, NULL);
  if (!g_option_context_parse (opt_ctx, &argc, &argv, &err))
    exiterr (err);

  if (!solvables)
    {
      g_set_error_literal (&err,
                           G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                           "At least one solvable should be specified");
      exiterr (err);
    }

  if (verbose)
    g_setenv ("G_MESSAGES_DEBUG", "fus", FALSE);

  if (!arch)
    {
      struct utsname un;
      uname (&un);
      arch = un.machine;
    }
  g_debug ("Setting architecture to %s", arch);

  g_autoptr(GPtrArray) packages = NULL;
  packages = fus_depsolve (arch, platform, exclude_packages, repos, solvables, &err);
  if (!packages || err)
    exiterr (err);

  /* Output resolved packages */
  g_ptr_array_foreach (packages, print_package, NULL);

  return EXIT_SUCCESS;
}
