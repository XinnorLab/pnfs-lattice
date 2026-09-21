/*
 * Copyright (c) 2026 PeakAIO
 * SPDX-License-Identifier: MIT
 *
 * harness.c -- Backend-parameterised catalogue conformance harness.
 *
 * See harness.h for the contract.  The harness deliberately knows
 * nothing about the tests: it opens a catalogue on the requested
 * backend, turns "backend unavailable" into exit code 77 and provides
 * a scratch directory so the same binaries run against a persistent
 * store without colliding.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"

/* Bounded table of scratch directories created by this process so the
 * cleanup helper can remove them by (root, name) again.  Beyond the
 * bound the cleanup falls back to the (public) child-to-name lookup. */
#define SCRATCH_MAX 256

struct scratch_entry {
    const struct mds_catalogue *cat; /**< Independent instances reuse fileids. */
    uint64_t fid;
    char     name[64];
};

static pthread_mutex_t   g_scratch_lock = PTHREAD_MUTEX_INITIALIZER;
static struct scratch_entry g_scratch[SCRATCH_MAX];
static uint32_t          g_scratch_count;
static uint32_t          g_scratch_seq;

_Static_assert(CONFORMANCE_SKIP == 77,
               "ctest SKIP_RETURN_CODE in tests/catalogue_conformance/"
               "CMakeLists.txt is 77");

const char *conformance_backend_name(void)
{
    const char *name = getenv(CONFORMANCE_BACKEND_ENV);

    if (name == NULL || name[0] == '\0') {
        return "memdb";
    }
    return name;
}

bool conformance_backend_is(const char *name)
{
    if (name == NULL) {
        return false;
    }
    return strcmp(conformance_backend_name(), name) == 0;
}

_Noreturn void conformance_skip(const char *reason)
{
    (void)fprintf(stdout, "SKIP: %s (backend=%s)\n",
                  reason != NULL ? reason : "no reason given",
                  conformance_backend_name());
    (void)fflush(stdout);
    exit(CONFORMANCE_SKIP);
}

/* memdb: the in-process reference backend, opened through the backend
 * registration table (catalogue_factory.c) exactly like the daemon
 * does for `catalogue_backend = memdb`.  The instance takes nothing
 * from the configuration; an all-zero block with the backend set is
 * the documented "defaults" input (catalogue_memdb.h). */
static enum mds_status open_memdb(struct mds_catalogue **out)
{
    struct mds_config *cfg;
    enum mds_status st;

    if (!mds_catalogue_backend_available(MDS_BACKEND_MEMDB)) {
        conformance_skip("memdb backend not compiled in "
                         "(ENABLE_MEMDB_BACKEND=OFF)");
    }
    /* struct mds_config is large (per-DS spec strings); never on the
     * stack of a test thread. */
    cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL) {
        return MDS_ERR_NOMEM;
    }
    cfg->catalogue_backend = MDS_BACKEND_MEMDB;

    st = mds_catalogue_open(cfg, out);
    free(cfg);
    return st;
}

static enum mds_status open_rondb(struct mds_catalogue **out)
{
#ifdef HAVE_RONDB
    const char *conf = getenv(CONFORMANCE_RONDB_CONF_ENV);
    struct mds_config *cfg;
    enum mds_status st;

    if (conf == NULL || conf[0] == '\0') {
        conformance_skip(CONFORMANCE_RONDB_CONF_ENV " is not set");
    }
    /* struct mds_config is large (per-DS spec strings); never on the
     * stack of a test thread. */
    cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL) {
        return MDS_ERR_NOMEM;
    }
    cfg->catalogue_backend = MDS_BACKEND_RONDB;
    (void)snprintf(cfg->catalogue_backend_conf,
                   sizeof(cfg->catalogue_backend_conf), "%s", conf);
    cfg->self.id = 1;
    (void)snprintf(cfg->self.hostname, sizeof(cfg->self.hostname),
                   "localhost");
    cfg->cluster_size = 1;
    cfg->ndb_conn_pool_size = 2;
    cfg->ndb_async_writes = false;
    cfg->catalog_image_mode = MDS_IMAGE_OFF;
    cfg->catalog_replay_mode = MDS_REPLAY_OFF;

    st = mds_catalogue_open(cfg, out);
    free(cfg);
    return st;
#else
    (void)out;
    conformance_skip("RonDB backend not compiled in (ENABLE_RONDB=OFF)");
#endif
}

static enum mds_status open_fdb(struct mds_catalogue **out)
{
    /* Gate 2.  When the FoundationDB backend exists, this opens it with
     * FDB_CLUSTER_FILE and an isolated per-run key prefix taken from
     * CATALOGUE_TEST_KEY_PREFIX (generated randomly when unset, cleared
     * at start and at exit) so concurrent runs against one fdbserver
     * never see each other's rows.  Hook only; no implementation. */
    (void)out;
    conformance_skip("FoundationDB backend is not built in gate 1");
}

enum mds_status conformance_open(struct mds_catalogue **out)
{
    const char *name;

    if (out == NULL) {
        return MDS_ERR_INVAL;
    }
    *out = NULL;
    name = conformance_backend_name();

    if (strcmp(name, "memdb") == 0) {
        return open_memdb(out);
    }
    if (strcmp(name, "rondb") == 0) {
        return open_rondb(out);
    }
    if (strcmp(name, "fdb") == 0) {
        return open_fdb(out);
    }

    (void)fprintf(stderr,
                  "conformance: unknown " CONFORMANCE_BACKEND_ENV
                  " '%s' (expected memdb|rondb|fdb)\n", name);
    exit(1);
}

struct mds_catalogue *conformance_open_checked(void)
{
    struct mds_catalogue *cat = NULL;
    enum mds_status st = conformance_open(&cat);

    if (st != MDS_OK || cat == NULL) {
        (void)fprintf(stderr, "conformance: open on backend %s failed: %d\n",
                      conformance_backend_name(), (int)st);
        exit(1);
    }
    return cat;
}

enum mds_status conformance_scratch_dir(struct mds_catalogue *cat,
                                        uint64_t *out_fid)
{
    struct mds_inode dir;
    char name[64];
    uint32_t seq;
    enum mds_status st;

    if (cat == NULL || out_fid == NULL) {
        return MDS_ERR_INVAL;
    }

    pthread_mutex_lock(&g_scratch_lock);
    seq = ++g_scratch_seq;
    pthread_mutex_unlock(&g_scratch_lock);

    (void)snprintf(name, sizeof(name), "conf-%ld-%u", (long)getpid(), seq);
    memset(&dir, 0, sizeof(dir));
    st = mds_cat_ns_create(cat, NULL, MDS_FILEID_ROOT, name, MDS_FTYPE_DIR,
                           0755, 0, 0, NULL, &dir);
    if (st != MDS_OK) {
        return st;
    }

    pthread_mutex_lock(&g_scratch_lock);
    if (g_scratch_count < SCRATCH_MAX) {
        g_scratch[g_scratch_count].cat = cat;
        g_scratch[g_scratch_count].fid = dir.fileid;
        (void)snprintf(g_scratch[g_scratch_count].name,
                       sizeof(g_scratch[g_scratch_count].name), "%s", name);
        g_scratch_count++;
    }
    pthread_mutex_unlock(&g_scratch_lock);

    *out_fid = dir.fileid;
    return MDS_OK;
}

/* Bounded name collector for the cleanup readdir. */
#define CLEANUP_PAGE 512

struct cleanup_page {
    uint32_t count;
    uint8_t  type[CLEANUP_PAGE];
    uint64_t fid[CLEANUP_PAGE];
    char     name[CLEANUP_PAGE][MDS_MAX_NAME + 1];
};

static int cleanup_collect_cb(const struct mds_cat_dirent *entry, void *arg)
{
    struct cleanup_page *page = arg;

    if (page->count >= CLEANUP_PAGE) {
        return 1;
    }
    page->type[page->count] = entry->type;
    page->fid[page->count] = entry->fileid;
    (void)snprintf(page->name[page->count], sizeof(page->name[0]), "%s",
                   entry->name);
    page->count++;
    return 0;
}

/* Remove every entry of @dir_fid; entries that are directories have
 * their own (one level of) entries removed first. */
static void cleanup_dir_entries(struct mds_catalogue *cat, uint64_t dir_fid,
                                int depth)
{
    struct cleanup_page *page = calloc(1, sizeof(*page));
    uint32_t i;

    if (page == NULL) {
        return;
    }
    if (mds_cat_ns_readdir(cat, dir_fid, NULL, CLEANUP_PAGE, NULL,
                           cleanup_collect_cb, page) != MDS_OK) {
        free(page);
        return;
    }
    for (i = 0; i < page->count; i++) {
        if (page->type[i] == (uint8_t)MDS_FTYPE_DIR && depth > 0) {
            cleanup_dir_entries(cat, page->fid[i], depth - 1);
        }
        (void)mds_cat_ns_remove(cat, NULL, dir_fid, page->name[i]);
    }
    free(page);
}

void conformance_scratch_cleanup(struct mds_catalogue *cat,
                                 uint64_t dir_fid)
{
    char name[64];
    uint32_t i;
    bool found = false;

    if (cat == NULL) {
        return;
    }
    name[0] = '\0';
    pthread_mutex_lock(&g_scratch_lock);
    for (i = 0; i < g_scratch_count; i++) {
        if (g_scratch[i].cat == cat && g_scratch[i].fid == dir_fid) {
            (void)snprintf(name, sizeof(name), "%s", g_scratch[i].name);
            found = true;
            /* Release the slot: the table bounds LIVE scratch dirs. */
            g_scratch[i] = g_scratch[g_scratch_count - 1];
            g_scratch_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_scratch_lock);

    if (!found &&
        mds_cat_ns_dirent_name_for_child(cat, MDS_FILEID_ROOT, dir_fid, name,
                                         sizeof(name)) == MDS_OK) {
        found = true;
    }

    cleanup_dir_entries(cat, dir_fid, 1);
    if (found) {
        (void)mds_cat_ns_remove(cat, NULL, MDS_FILEID_ROOT, name);
    }
}
