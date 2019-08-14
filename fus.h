#pragma once

#include <glib.h>
#include <libsoup/soup.h>
#include <solv/pool.h>
#include <solv/repo.h>
#include <solv/selection.h>
#include <solv/solver.h>
#include <solv/transaction.h>

#define TMPL_NPROV "module(%s)"
#define TMPL_NSPROV "module(%s:%s)"
#define MODPKG_PROV "modular-package()"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(Pool, pool_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Solver, solver_free);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Transaction, transaction_free);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Queue, queue_free);
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Map, map_free);

Repo *create_repo (Pool *pool, SoupSession *session, const char *name, const char *path, GError **error);
Repo *create_test_repo (Pool *pool, const char *name, const char *type, const char *path, GError **error);
Repo *create_system_repo (Pool *pool, const char *platform, const char *arch);
int filelist_loadcb (Pool *pool, Repodata *data, void *cdata);

GPtrArray *fus_depsolve (const char *arch, const char *platform, const GStrv exclude_packages, const GStrv repos, const GStrv solvables, GError **error);
