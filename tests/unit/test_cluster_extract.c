/*
 * Copyright (c) 2026 PeakAIO
 * SPDX-License-Identifier: MIT
 *
 * test_cluster_extract.c -- Unit tests for the backend-neutral cluster
 * services extraction.
 *
 * A fabricated struct mds_catalogue carrying fake struct mds_cluster_ops
 * (and fake lifecycle slots) drives the daemon-side consumers exactly
 * as the dispatchers do, without any backend:
 *   - subtree_map_init_from_catalogue / refresh_from_catalogue /
 *     seed_shards over mds_cluster_partition_list / _put (including
 *     the carried-over seed-root-on-list-failure behaviour, pinned so
 *     its later replacement is a visible test change);
 *   - cluster_membership_populate over mds_cluster_node_list;
 *   - failover_watchdog_start refusing with NOSUPPORT when the
 *     node_scan_stale slot is absent;
 *   - the mds_cluster_* / mds_catalogue_image_feed_* dispatchers'
 *     INVAL / NOSUPPORT / pass-through contract (C4);
 *   - the backend name table (pnfs_common: name <-> enum), the core's
 *     registration table (availability, open refusal) and the config
 *     parser's use of the former in both build flavours.
 */

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pnfs_mds.h"
#include "mds_catalogue.h"
#include "catalogue_backend_names.h"
#include "mds_cluster.h"
#include "catalogue_internal.h"
#include "catalog_image.h"
#include "subtree_map.h"
#include "cluster_membership.h"
#include "failover.h"
#include "failover_watchdog.h"

/* -------------------------------------------------------------------
 * Test infrastructure
 * ------------------------------------------------------------------- */

static int tests_run;
static int tests_passed;
static int test_failed;

#define ASSERT_EQ(a, b) do {                                    \
    if ((a) != (b)) {                                           \
        fprintf(stderr, "  FAIL %s:%d: %s (%lld) != %s (%lld)\n", \
                __FILE__, __LINE__, #a, (long long)(a),         \
                #b, (long long)(b));                            \
        test_failed = 1;                                        \
        return;                                                 \
    }                                                           \
} while (0)

#define ASSERT_TRUE(cond) do {                                  \
    if (!(cond)) {                                              \
        fprintf(stderr, "  FAIL %s:%d: !(%s)\n",               \
                __FILE__, __LINE__, #cond);                     \
        test_failed = 1;                                        \
        return;                                                 \
    }                                                           \
} while (0)

#define ASSERT_STREQ(a, b) do {                                 \
    if (strcmp((a), (b)) != 0) {                                \
        fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n",     \
                __FILE__, __LINE__, (a), (b));                  \
        test_failed = 1;                                        \
        return;                                                 \
    }                                                           \
} while (0)

#define RUN_TEST(fn) do {                                       \
    tests_run++;                                                \
    test_failed = 0;                                            \
    fprintf(stdout, "  %-52s", #fn);                            \
    fflush(stdout);                                             \
    fn();                                                       \
    if (!test_failed) {                                         \
        tests_passed++;                                         \
        fprintf(stdout, "PASS\n");                              \
    } else {                                                    \
        fprintf(stdout, "FAIL\n");                              \
    }                                                           \
} while (0)

/* -------------------------------------------------------------------
 * Fake cluster services
 *
 * Rows and recorded calls live in file-scope arrays with fixed
 * capacities; every test starts from fake_reset().
 * ------------------------------------------------------------------- */

#define FAKE_ROWS_MAX 8
#define FAKE_CALLS_MAX 16

struct fake_pm_row {
    uint32_t    partition_id;
    uint32_t    owner;
    uint8_t     state;
    const char *path;
};

struct fake_pm_put {
    uint32_t partition_id;
    uint32_t owner;
    uint8_t  state;
    bool     insert_only;
    char     path[64];
};

struct fake_node_row {
    uint32_t    mds_id;
    uint64_t    boot_epoch;
    const char *hostname;
    uint16_t    nfs_port;
    uint16_t    grpc_port;
    uint64_t    last_heartbeat_ns;
};

static struct fake_pm_row   fake_pm_rows[FAKE_ROWS_MAX];
static uint32_t             fake_pm_row_count;
static enum mds_status      fake_pm_list_status;
static struct fake_pm_put   fake_pm_puts[FAKE_CALLS_MAX];
static uint32_t             fake_pm_put_count;
static enum mds_status      fake_pm_put_status;

static struct fake_node_row fake_node_rows[FAKE_ROWS_MAX];
static uint32_t             fake_node_row_count;
static enum mds_status      fake_node_list_status;

static struct {
    uint32_t mds_id;
    uint64_t boot_epoch;
    char     hostname[64];
    uint16_t nfs_port;
    uint16_t grpc_port;
    uint32_t calls;
}                           fake_register;
static struct {
    uint32_t mds_id;
    uint64_t boot_epoch;
    uint32_t calls;
}                           fake_heartbeat, fake_deregister;
static enum mds_status      fake_heartbeat_status;

static void fake_reset(void)
{
    memset(fake_pm_rows, 0, sizeof(fake_pm_rows));
    fake_pm_row_count = 0;
    fake_pm_list_status = MDS_OK;
    memset(fake_pm_puts, 0, sizeof(fake_pm_puts));
    fake_pm_put_count = 0;
    fake_pm_put_status = MDS_OK;
    memset(fake_node_rows, 0, sizeof(fake_node_rows));
    fake_node_row_count = 0;
    fake_node_list_status = MDS_OK;
    memset(&fake_register, 0, sizeof(fake_register));
    memset(&fake_heartbeat, 0, sizeof(fake_heartbeat));
    memset(&fake_deregister, 0, sizeof(fake_deregister));
    fake_heartbeat_status = MDS_OK;
}

static void fake_pm_add(uint32_t id, uint32_t owner, uint8_t state,
                        const char *path)
{
    if (fake_pm_row_count < FAKE_ROWS_MAX) {
        fake_pm_rows[fake_pm_row_count].partition_id = id;
        fake_pm_rows[fake_pm_row_count].owner = owner;
        fake_pm_rows[fake_pm_row_count].state = state;
        fake_pm_rows[fake_pm_row_count].path = path;
        fake_pm_row_count++;
    }
}

static void fake_node_add(uint32_t mds_id, const char *hostname,
                          uint16_t nfs_port, uint16_t grpc_port,
                          uint64_t last_heartbeat_ns)
{
    if (fake_node_row_count < FAKE_ROWS_MAX) {
        struct fake_node_row *r = &fake_node_rows[fake_node_row_count];

        r->mds_id = mds_id;
        r->boot_epoch = 100 + mds_id;
        r->hostname = hostname;
        r->nfs_port = nfs_port;
        r->grpc_port = grpc_port;
        r->last_heartbeat_ns = last_heartbeat_ns;
        fake_node_row_count++;
    }
}

static enum mds_status fake_node_register(struct mds_catalogue *cat,
                                          uint32_t mds_id,
                                          uint64_t boot_epoch,
                                          const char *hostname,
                                          uint16_t nfs_port,
                                          uint16_t grpc_port)
{
    (void)cat;
    fake_register.mds_id = mds_id;
    fake_register.boot_epoch = boot_epoch;
    (void)snprintf(fake_register.hostname, sizeof(fake_register.hostname),
                   "%s", hostname);
    fake_register.nfs_port = nfs_port;
    fake_register.grpc_port = grpc_port;
    fake_register.calls++;
    return MDS_OK;
}

static enum mds_status fake_node_heartbeat(struct mds_catalogue *cat,
                                           uint32_t mds_id,
                                           uint64_t boot_epoch)
{
    (void)cat;
    fake_heartbeat.mds_id = mds_id;
    fake_heartbeat.boot_epoch = boot_epoch;
    fake_heartbeat.calls++;
    return fake_heartbeat_status;
}

static enum mds_status fake_node_deregister(struct mds_catalogue *cat,
                                            uint32_t mds_id,
                                            uint64_t boot_epoch)
{
    (void)cat;
    fake_deregister.mds_id = mds_id;
    fake_deregister.boot_epoch = boot_epoch;
    fake_deregister.calls++;
    return MDS_OK;
}

static enum mds_status fake_node_list(struct mds_catalogue *cat,
                                      mds_cluster_node_cb cb, void *ctx)
{
    uint32_t i;

    (void)cat;
    if (fake_node_list_status != MDS_OK) {
        return fake_node_list_status;
    }
    for (i = 0; i < fake_node_row_count; i++) {
        const struct fake_node_row *r = &fake_node_rows[i];

        if (cb(r->mds_id, r->boot_epoch, r->hostname, r->nfs_port,
               r->grpc_port, r->last_heartbeat_ns, ctx) != 0) {
            break;
        }
    }
    return MDS_OK;
}

static enum mds_status fake_node_scan_stale(struct mds_catalogue *cat,
                                            uint64_t threshold_ns,
                                            mds_cluster_stale_cb cb,
                                            void *ctx)
{
    uint32_t i;

    (void)cat;
    for (i = 0; i < fake_node_row_count; i++) {
        const struct fake_node_row *r = &fake_node_rows[i];

        if (r->last_heartbeat_ns < threshold_ns &&
            cb(r->mds_id, r->boot_epoch, r->last_heartbeat_ns, ctx) != 0) {
            break;
        }
    }
    return MDS_OK;
}

static enum mds_status fake_partition_list(struct mds_catalogue *cat,
                                           mds_cluster_partition_cb cb,
                                           void *ctx)
{
    uint32_t i;

    (void)cat;
    if (fake_pm_list_status != MDS_OK) {
        return fake_pm_list_status;
    }
    for (i = 0; i < fake_pm_row_count; i++) {
        const struct fake_pm_row *r = &fake_pm_rows[i];

        if (cb(r->partition_id, r->owner, r->state, r->path, ctx) != 0) {
            break;
        }
    }
    return MDS_OK;
}

static enum mds_status fake_partition_put(struct mds_catalogue *cat,
                                          uint32_t partition_id,
                                          uint32_t owner_mds_id,
                                          uint8_t state,
                                          const char *subtree_path,
                                          bool insert_only)
{
    (void)cat;
    if (fake_pm_put_count < FAKE_CALLS_MAX) {
        struct fake_pm_put *p = &fake_pm_puts[fake_pm_put_count];

        p->partition_id = partition_id;
        p->owner = owner_mds_id;
        p->state = state;
        p->insert_only = insert_only;
        (void)snprintf(p->path, sizeof(p->path), "%s", subtree_path);
    }
    fake_pm_put_count++;
    return fake_pm_put_status;
}

static const struct mds_cluster_ops fake_ops_full = {
    .node_register   = fake_node_register,
    .node_heartbeat  = fake_node_heartbeat,
    .node_deregister = fake_node_deregister,
    .node_list       = fake_node_list,
    .node_scan_stale = fake_node_scan_stale,
    .partition_list  = fake_partition_list,
    .partition_put   = fake_partition_put,
};

/* Everything but the stale scan: what the watchdog needs is missing. */
static const struct mds_cluster_ops fake_ops_no_stale = {
    .node_register   = fake_node_register,
    .node_heartbeat  = fake_node_heartbeat,
    .node_deregister = fake_node_deregister,
    .node_list       = fake_node_list,
    .partition_list  = fake_partition_list,
    .partition_put   = fake_partition_put,
};

/* A registry without a partition map. */
static const struct mds_cluster_ops fake_ops_registry_only = {
    .node_register   = fake_node_register,
    .node_heartbeat  = fake_node_heartbeat,
    .node_deregister = fake_node_deregister,
    .node_list       = fake_node_list,
    .node_scan_stale = fake_node_scan_stale,
};

/* -------------------------------------------------------------------
 * Fake lifecycle slots (image feed)
 * ------------------------------------------------------------------- */

static struct {
    struct mds_catalogue *cat;
    struct catalog_image *image;
    uint32_t              self_mds_id;
    uint32_t              poll_interval_ms;
    uint32_t              start_calls;
    uint32_t              stop_calls;
}                      fake_feed;
static enum mds_status fake_feed_start_status;

static enum mds_status fake_image_feed_start(struct mds_catalogue *cat,
                                             struct catalog_image *image,
                                             uint32_t self_mds_id,
                                             uint32_t poll_interval_ms)
{
    fake_feed.cat = cat;
    fake_feed.image = image;
    fake_feed.self_mds_id = self_mds_id;
    fake_feed.poll_interval_ms = poll_interval_ms;
    fake_feed.start_calls++;
    return fake_feed_start_status;
}

static void fake_image_feed_stop(struct mds_catalogue *cat)
{
    fake_feed.cat = cat;
    fake_feed.stop_calls++;
}

static const struct mds_catalogue_ops fake_lifecycle_no_feed = {
    .close = NULL,
    .probe = NULL,
};

static const struct mds_catalogue_ops fake_lifecycle_with_feed = {
    .image_feed_start = fake_image_feed_start,
    .image_feed_stop  = fake_image_feed_stop,
};

static const struct mds_catalogue_ops fake_lifecycle_start_only = {
    .image_feed_start = fake_image_feed_start,
};

/* -------------------------------------------------------------------
 * Fabricated catalogue handles
 *
 * The dispatchers touch only backend, caps, ops and cluster_ops.  The
 * handle is never passed to mds_catalogue_close(), so no backend
 * resources exist to release.
 * ------------------------------------------------------------------- */

static struct mds_catalogue fake_cat_storage;

static struct mds_catalogue *make_fake_cat(const struct mds_cluster_ops *cops,
                                           const struct mds_catalogue_ops *lops,
                                           uint32_t caps)
{
    memset(&fake_cat_storage, 0, sizeof(fake_cat_storage));
    fake_cat_storage.backend = MDS_BACKEND_MEMDB;
    fake_cat_storage.caps = caps;
    fake_cat_storage.ops = lops;
    fake_cat_storage.cluster_ops = cops;
    return &fake_cat_storage;
}

static void make_test_config(struct mds_config *cfg, uint32_t id,
                             const char *hostname)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->self.id = id;
    (void)snprintf(cfg->self.hostname, sizeof(cfg->self.hostname), "%s",
                   hostname);
    cfg->self.nfs_port = 2049;
    cfg->self.grpc_port = 50051;
    cfg->cluster_size = 1;
}

/* -------------------------------------------------------------------
 * Dispatcher contract: INVAL, NOSUPPORT, pass-through (C4)
 * ------------------------------------------------------------------- */

static void test_cluster_supported_predicates(void)
{
    struct mds_catalogue *cat;

    ASSERT_TRUE(!mds_cluster_supported(NULL));
    ASSERT_TRUE(!mds_cluster_stale_scan_supported(NULL));

    /* Every slot present AND the multi-process capability: supported. */
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    ASSERT_TRUE(mds_cluster_supported(cat));
    ASSERT_TRUE(mds_cluster_stale_scan_supported(cat));

    /* Slots present but an in-process store: never a cluster. */
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_no_feed, 0);
    ASSERT_TRUE(!mds_cluster_supported(cat));
    ASSERT_TRUE(mds_cluster_stale_scan_supported(cat));

    /* Missing partition map: not supported even with the capability;
     * the stale scan alone is still reported for what it is. */
    cat = make_fake_cat(&fake_ops_registry_only, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    ASSERT_TRUE(!mds_cluster_supported(cat));
    ASSERT_TRUE(mds_cluster_stale_scan_supported(cat));

    /* Missing stale scan only. */
    cat = make_fake_cat(&fake_ops_no_stale, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    ASSERT_TRUE(mds_cluster_supported(cat));
    ASSERT_TRUE(!mds_cluster_stale_scan_supported(cat));

    /* No cluster table at all. */
    cat = make_fake_cat(NULL, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    ASSERT_TRUE(!mds_cluster_supported(cat));
    ASSERT_TRUE(!mds_cluster_stale_scan_supported(cat));
}

static void test_cluster_dispatch_args_and_passthrough(void)
{
    struct mds_catalogue *cat;

    fake_reset();
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);

    /* Arguments are forwarded verbatim. */
    ASSERT_EQ(mds_cluster_node_register(cat, 7, 4242, "mds7.local",
                                        2049, 50057), MDS_OK);
    ASSERT_EQ(fake_register.calls, 1U);
    ASSERT_EQ(fake_register.mds_id, 7U);
    ASSERT_EQ(fake_register.boot_epoch, 4242U);
    ASSERT_STREQ(fake_register.hostname, "mds7.local");
    ASSERT_EQ(fake_register.nfs_port, 2049);
    ASSERT_EQ(fake_register.grpc_port, 50057);

    ASSERT_EQ(mds_cluster_node_deregister(cat, 7, 4242), MDS_OK);
    ASSERT_EQ(fake_deregister.calls, 1U);
    ASSERT_EQ(fake_deregister.mds_id, 7U);
    ASSERT_EQ(fake_deregister.boot_epoch, 4242U);

    /* C4: a slot's NOTFOUND reaches the caller unchanged. */
    fake_heartbeat_status = MDS_ERR_NOTFOUND;
    ASSERT_EQ(mds_cluster_node_heartbeat(cat, 7, 4242), MDS_ERR_NOTFOUND);
    ASSERT_EQ(fake_heartbeat.calls, 1U);
    ASSERT_EQ(fake_heartbeat.boot_epoch, 4242U);
    fake_heartbeat_status = MDS_ERR_STALE;
    ASSERT_EQ(mds_cluster_node_heartbeat(cat, 7, 4242), MDS_ERR_STALE);

    /* Argument validation precedes slot presence. */
    ASSERT_EQ(mds_cluster_node_register(NULL, 7, 1, "h", 1, 1),
              MDS_ERR_INVAL);
    ASSERT_EQ(mds_cluster_node_register(cat, 7, 1, NULL, 1, 1),
              MDS_ERR_INVAL);
    ASSERT_EQ(mds_cluster_node_heartbeat(NULL, 7, 1), MDS_ERR_INVAL);
    ASSERT_EQ(mds_cluster_node_deregister(NULL, 7, 1), MDS_ERR_INVAL);
    ASSERT_EQ(mds_cluster_node_list(cat, NULL, NULL), MDS_ERR_INVAL);
    ASSERT_EQ(mds_cluster_node_scan_stale(cat, 0, NULL, NULL),
              MDS_ERR_INVAL);
    ASSERT_EQ(mds_cluster_partition_list(cat, NULL, NULL), MDS_ERR_INVAL);
    ASSERT_EQ(mds_cluster_partition_put(cat, 0, 1, 0, NULL, false),
              MDS_ERR_INVAL);

    /* Absent slots -> NOSUPPORT. */
    cat = make_fake_cat(&fake_ops_registry_only, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    ASSERT_EQ(mds_cluster_partition_put(cat, 0, 1,
                                        MDS_PARTITION_STATE_ACTIVE, "/",
                                        false), MDS_ERR_NOSUPPORT);
    cat = make_fake_cat(&fake_ops_no_stale, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    ASSERT_EQ(mds_cluster_node_scan_stale(cat, 1, NULL, NULL),
              MDS_ERR_INVAL);
    cat = make_fake_cat(NULL, &fake_lifecycle_no_feed, 0);
    ASSERT_EQ(mds_cluster_node_heartbeat(cat, 1, 1), MDS_ERR_NOSUPPORT);
    ASSERT_EQ(mds_cluster_node_deregister(cat, 1, 1), MDS_ERR_NOSUPPORT);
    ASSERT_EQ(mds_cluster_node_register(cat, 1, 1, "h", 1, 1),
              MDS_ERR_NOSUPPORT);
}

/* -------------------------------------------------------------------
 * subtree_map over the partition map
 * ------------------------------------------------------------------- */

static void test_subtree_init_loads_rows(void)
{
    struct mds_catalogue *cat;
    struct subtree_map *map = NULL;
    struct subtree_entry e;

    fake_reset();
    fake_pm_add(0, 1, MDS_PARTITION_STATE_ACTIVE, "/");
    fake_pm_add(2, 2, MDS_PARTITION_STATE_ACTIVE, "/shard2");
    fake_pm_add(3, 3, MDS_PARTITION_STATE_FROZEN, "/shard3");
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);

    ASSERT_EQ(subtree_map_init_from_catalogue(cat, 1, "mds1.local", &map),
              MDS_OK);
    ASSERT_TRUE(map != NULL);
    ASSERT_EQ(subtree_map_count(map), 3U);
    ASSERT_EQ(subtree_map_lookup_exact(map, "/shard2", &e), MDS_OK);
    ASSERT_EQ(e.owner_mds_id, 2U);
    ASSERT_EQ((int)e.state, (int)SUBTREE_ACTIVE);
    ASSERT_EQ(subtree_map_lookup_exact(map, "/shard3", &e), MDS_OK);
    ASSERT_EQ((int)e.state, (int)SUBTREE_FROZEN);
    ASSERT_TRUE(subtree_map_is_local(map, "/"));
    ASSERT_TRUE(!subtree_map_is_local(map, "/shard2/x"));
    /* Root was present: nothing was written back. */
    ASSERT_EQ(fake_pm_put_count, 0U);
    /* The hostname registered for referrals. */
    {
        char host[64];

        ASSERT_EQ(subtree_map_node_hostname(map, 1, host, sizeof(host)),
                  MDS_OK);
        ASSERT_STREQ(host, "mds1.local");
    }
    subtree_map_destroy(map);

    ASSERT_EQ(subtree_map_init_from_catalogue(NULL, 1, "h", &map),
              MDS_ERR_INVAL);
    ASSERT_EQ(subtree_map_init_from_catalogue(cat, 1, "h", NULL),
              MDS_ERR_INVAL);
}

/* Pins the carried-over behaviour: a failed partition_list is treated
 * as an empty map, root is seeded for self and written back with ONE
 * upsert (id 0, insert_only == false).  Its replacement by the
 * cluster-services contract must change this test deliberately. */
static void test_subtree_init_list_failure_seeds_root(void)
{
    struct mds_catalogue *cat;
    struct subtree_map *map = NULL;
    struct subtree_entry e;

    fake_reset();
    fake_pm_add(2, 2, MDS_PARTITION_STATE_ACTIVE, "/shard2");
    fake_pm_list_status = MDS_ERR_IO;
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);

    ASSERT_EQ(subtree_map_init_from_catalogue(cat, 7, "mds7.local", &map),
              MDS_OK);
    ASSERT_EQ(subtree_map_count(map), 1U);
    ASSERT_EQ(subtree_map_lookup_exact(map, "/", &e), MDS_OK);
    ASSERT_EQ(e.owner_mds_id, 7U);
    ASSERT_EQ(fake_pm_put_count, 1U);
    ASSERT_EQ(fake_pm_puts[0].partition_id, 0U);
    ASSERT_EQ(fake_pm_puts[0].owner, 7U);
    ASSERT_EQ(fake_pm_puts[0].state, MDS_PARTITION_STATE_ACTIVE);
    ASSERT_STREQ(fake_pm_puts[0].path, "/");
    ASSERT_TRUE(!fake_pm_puts[0].insert_only);
    subtree_map_destroy(map);

    /* An empty (successful) list seeds root the same way. */
    fake_reset();
    ASSERT_EQ(subtree_map_init_from_catalogue(cat, 7, NULL, &map), MDS_OK);
    ASSERT_EQ(subtree_map_count(map), 1U);
    ASSERT_EQ(fake_pm_put_count, 1U);
    ASSERT_EQ(fake_pm_puts[0].partition_id, 0U);
    ASSERT_TRUE(!fake_pm_puts[0].insert_only);
    subtree_map_destroy(map);

    /* A failed root put is not fatal (carried over). */
    fake_reset();
    fake_pm_put_status = MDS_ERR_IO;
    ASSERT_EQ(subtree_map_init_from_catalogue(cat, 7, NULL, &map), MDS_OK);
    ASSERT_EQ(subtree_map_count(map), 1U);
    subtree_map_destroy(map);
}

static void test_subtree_refresh_from_catalogue(void)
{
    struct mds_catalogue *cat;
    struct subtree_map *map = NULL;
    struct subtree_entry e;

    fake_reset();
    fake_pm_add(0, 1, MDS_PARTITION_STATE_ACTIVE, "/");
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    ASSERT_EQ(subtree_map_init_from_catalogue(cat, 1, "mds1", &map), MDS_OK);
    ASSERT_EQ(subtree_map_count(map), 1U);

    /* Another node changed root's owner and added a shard. */
    fake_pm_rows[0].owner = 3;
    fake_pm_add(2, 2, MDS_PARTITION_STATE_MIGRATING, "/shard2");
    ASSERT_EQ(subtree_map_refresh_from_catalogue(map, cat), MDS_OK);
    ASSERT_EQ(subtree_map_count(map), 2U);
    ASSERT_EQ(subtree_map_lookup_exact(map, "/", &e), MDS_OK);
    ASSERT_EQ(e.owner_mds_id, 3U);
    ASSERT_EQ(subtree_map_lookup_exact(map, "/shard2", &e), MDS_OK);
    ASSERT_EQ((int)e.state, (int)SUBTREE_MIGRATING);

    /* A refresh failure changes nothing and passes the status through. */
    fake_pm_list_status = MDS_ERR_DELAY;
    ASSERT_EQ(subtree_map_refresh_from_catalogue(map, cat), MDS_ERR_DELAY);
    ASSERT_EQ(subtree_map_count(map), 2U);

    /* Refresh over a backend without a partition map: NOSUPPORT. */
    cat = make_fake_cat(&fake_ops_registry_only, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    ASSERT_EQ(subtree_map_refresh_from_catalogue(map, cat),
              MDS_ERR_NOSUPPORT);

    ASSERT_EQ(subtree_map_refresh_from_catalogue(NULL, cat), MDS_ERR_INVAL);
    ASSERT_EQ(subtree_map_refresh_from_catalogue(map, NULL), MDS_ERR_INVAL);
    subtree_map_destroy(map);
}

static void test_subtree_seed_shards(void)
{
    struct mds_catalogue *cat;
    struct subtree_map *map = NULL;
    struct subtree_entry e;
    const char *peers[2] = { "mds1.local", "mds2.local" };
    char host[64];
    uint32_t i;

    fake_reset();
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    ASSERT_EQ(subtree_map_init(NULL, NULL, 1, "mds1.local", NULL, &map),
              MDS_OK);
    ASSERT_EQ(subtree_map_count(map), 1U);

    /* Single-node cluster: nothing to seed, nothing written. */
    ASSERT_EQ(subtree_map_seed_shards(map, cat, 1, peers, 2), MDS_OK);
    ASSERT_EQ(subtree_map_count(map), 1U);
    ASSERT_EQ(fake_pm_put_count, 0U);

    /* Three nodes: /shard1../shard3 owned by 1..3, each persisted with
     * partition_id == mds_id as an upsert (insert_only == false). */
    ASSERT_EQ(subtree_map_seed_shards(map, cat, 3, peers, 2), MDS_OK);
    ASSERT_EQ(subtree_map_count(map), 4U);
    ASSERT_EQ(fake_pm_put_count, 3U);
    for (i = 0; i < 3; i++) {
        char path[16];

        (void)snprintf(path, sizeof(path), "/shard%u", (unsigned)(i + 1));
        ASSERT_EQ(fake_pm_puts[i].partition_id, i + 1);
        ASSERT_EQ(fake_pm_puts[i].owner, i + 1);
        ASSERT_EQ(fake_pm_puts[i].state, MDS_PARTITION_STATE_ACTIVE);
        ASSERT_STREQ(fake_pm_puts[i].path, path);
        ASSERT_TRUE(!fake_pm_puts[i].insert_only);
        ASSERT_EQ(subtree_map_lookup_exact(map, path, &e), MDS_OK);
        ASSERT_EQ(e.owner_mds_id, i + 1);
    }
    /* Peer hostnames register referral nodes; the third has none. */
    ASSERT_EQ(subtree_map_node_hostname(map, 2, host, sizeof(host)), MDS_OK);
    ASSERT_STREQ(host, "mds2.local");
    ASSERT_EQ(subtree_map_node_hostname(map, 3, host, sizeof(host)),
              MDS_ERR_NOTFOUND);

    /* A map that already has shards is left alone (idempotent). */
    ASSERT_EQ(subtree_map_seed_shards(map, cat, 3, peers, 2), MDS_OK);
    ASSERT_EQ(fake_pm_put_count, 3U);

    ASSERT_EQ(subtree_map_seed_shards(NULL, cat, 3, NULL, 0), MDS_ERR_INVAL);
    ASSERT_EQ(subtree_map_seed_shards(map, NULL, 3, NULL, 0), MDS_ERR_INVAL);
    subtree_map_destroy(map);

    /* Persist failures keep the in-memory seed for this boot. */
    fake_reset();
    fake_pm_put_status = MDS_ERR_IO;
    ASSERT_EQ(subtree_map_init(NULL, NULL, 1, "mds1.local", NULL, &map),
              MDS_OK);
    ASSERT_EQ(subtree_map_seed_shards(map, cat, 2, NULL, 0), MDS_OK);
    ASSERT_EQ(subtree_map_count(map), 3U);
    ASSERT_EQ(fake_pm_put_count, 2U);
    subtree_map_destroy(map);
}

/* -------------------------------------------------------------------
 * cluster_membership over the node registry
 * ------------------------------------------------------------------- */

static void test_membership_populate(void)
{
    struct mds_catalogue *cat;
    struct mds_config cfg;
    struct subtree_map *map = NULL;
    struct cluster_membership *cm = NULL;
    struct cluster_member m;
    char host[64];

    fake_reset();
    fake_node_add(1, "mds1.local", 2049, 50051, 1000);
    fake_node_add(2, "mds2.local", 2050, 50052, 2000);
    fake_node_add(3, "mds3.local", 2051, 50053, 3000);
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    make_test_config(&cfg, 1, "mds1.local");
    ASSERT_EQ(subtree_map_init(NULL, NULL, 1, "mds1.local", NULL, &map),
              MDS_OK);
    ASSERT_EQ(cluster_membership_init(&cfg, map, NULL, &cm), MDS_OK);
    ASSERT_EQ(cluster_membership_count(cm), 1U);

    ASSERT_EQ(cluster_membership_populate(cm, cat), MDS_OK);
    ASSERT_EQ(cluster_membership_count(cm), 3U);
    ASSERT_EQ(cluster_membership_get(cm, 2, &m), MDS_OK);
    ASSERT_STREQ(m.hostname, "mds2.local");
    ASSERT_EQ(m.nfs_port, 2050);
    ASSERT_EQ(m.grpc_port, 50052);
    ASSERT_EQ((int)m.role, (int)NODE_ACTIVE);
    ASSERT_EQ((int)m.lifecycle, (int)NODE_ACTIVE_SERVING);
    /* Peer hostnames flow into the subtree map for referrals. */
    ASSERT_EQ(subtree_map_node_hostname(map, 3, host, sizeof(host)), MDS_OK);
    ASSERT_STREQ(host, "mds3.local");

    /* Re-populating is an upsert, not a duplicate insert. */
    ASSERT_EQ(cluster_membership_populate(cm, cat), MDS_OK);
    ASSERT_EQ(cluster_membership_count(cm), 3U);

    /* Scan failure passes through unchanged (C4). */
    fake_node_list_status = MDS_ERR_IO;
    ASSERT_EQ(cluster_membership_populate(cm, cat), MDS_ERR_IO);
    fake_node_list_status = MDS_OK;

    /* No registry slot -> NOSUPPORT; NULL arguments -> INVAL. */
    cat = make_fake_cat(NULL, &fake_lifecycle_no_feed, 0);
    ASSERT_EQ(cluster_membership_populate(cm, cat), MDS_ERR_NOSUPPORT);
    ASSERT_EQ(cluster_membership_populate(NULL, cat), MDS_ERR_INVAL);
    ASSERT_EQ(cluster_membership_populate(cm, NULL), MDS_ERR_INVAL);

    cluster_membership_destroy(cm);
    subtree_map_destroy(map);
}

/* -------------------------------------------------------------------
 * failover watchdog: refuses without the stale scan, runs with it
 * ------------------------------------------------------------------- */

static void test_watchdog_start_contract(void)
{
    struct mds_catalogue *cat;
    struct subtree_map *map = NULL;
    struct failover_ctx *fo = NULL;
    struct failover_watchdog *wd = (struct failover_watchdog *)&fake_feed;
    struct failover_cfg fo_cfg;
    struct failover_watchdog_cfg wd_cfg;

    fake_reset();
    cat = make_fake_cat(&fake_ops_no_stale, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    ASSERT_EQ(subtree_map_init(NULL, NULL, 2, "standby", NULL, &map), MDS_OK);
    memset(&fo_cfg, 0, sizeof(fo_cfg));
    fo_cfg.self_id = 2;
    fo_cfg.partner_id = 1;
    fo_cfg.map = map;
    fo_cfg.cat = cat;
    ASSERT_EQ(failover_init(&fo_cfg, &fo), MDS_OK);

    memset(&wd_cfg, 0, sizeof(wd_cfg));
    wd_cfg.fo = fo;
    wd_cfg.cat = cat;
    wd_cfg.partner_id = 1;

    /* No node_scan_stale slot: nothing starts, *out is cleared. */
    ASSERT_EQ(failover_watchdog_start(&wd_cfg, &wd), MDS_ERR_NOSUPPORT);
    ASSERT_TRUE(wd == NULL);

    /* Argument validation comes first and is independent of slots. */
    ASSERT_EQ(failover_watchdog_start(NULL, &wd), MDS_ERR_INVAL);
    ASSERT_EQ(failover_watchdog_start(&wd_cfg, NULL), MDS_ERR_INVAL);
    wd_cfg.partner_id = 0;
    ASSERT_EQ(failover_watchdog_start(&wd_cfg, &wd), MDS_ERR_INVAL);
    wd_cfg.partner_id = 1;
    wd_cfg.cat = NULL;
    ASSERT_EQ(failover_watchdog_start(&wd_cfg, &wd), MDS_ERR_INVAL);
    wd_cfg.cat = cat;
    wd_cfg.fo = NULL;
    ASSERT_EQ(failover_watchdog_start(&wd_cfg, &wd), MDS_ERR_INVAL);
    wd_cfg.fo = fo;

    /* With the slot the watchdog starts; a short poll interval and a
     * long observation grace keep the thread from promoting anything
     * before it is stopped and joined. */
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_no_feed,
                        MDS_CAT_CAP_MULTI_PROCESS);
    wd_cfg.cat = cat;
    wd_cfg.poll_interval_ms = 10;
    wd_cfg.min_observe_ms = 60000;
    ASSERT_EQ(failover_watchdog_start(&wd_cfg, &wd), MDS_OK);
    ASSERT_TRUE(wd != NULL);
    failover_watchdog_stop(wd);
    failover_watchdog_stop(NULL);
    ASSERT_EQ((int)failover_get_role(fo), (int)FAILOVER_STANDBY);

    failover_destroy(fo);
    subtree_map_destroy(map);
}

/* -------------------------------------------------------------------
 * Image feed lifecycle dispatchers
 * ------------------------------------------------------------------- */

static void test_image_feed_dispatch(void)
{
    struct mds_catalogue *cat;
    struct catalog_image *img = NULL;

    ASSERT_EQ(catalog_image_create(&img), 0);
    memset(&fake_feed, 0, sizeof(fake_feed));
    fake_feed_start_status = MDS_OK;

    /* NULL slots -> NOSUPPORT, not supported. */
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_no_feed, 0);
    ASSERT_TRUE(!mds_catalogue_image_feed_supported(cat));
    ASSERT_EQ(mds_catalogue_image_feed_start(cat, img, 1, 50),
              MDS_ERR_NOSUPPORT);
    ASSERT_EQ(mds_catalogue_image_feed_stop(cat), MDS_ERR_NOSUPPORT);
    ASSERT_TRUE(!mds_catalogue_image_feed_supported(NULL));
    ASSERT_EQ(mds_catalogue_image_feed_start(NULL, img, 1, 50),
              MDS_ERR_INVAL);
    ASSERT_EQ(mds_catalogue_image_feed_stop(NULL), MDS_ERR_INVAL);

    /* Half a pair is not "supported" (both slots or neither). */
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_start_only, 0);
    ASSERT_TRUE(!mds_catalogue_image_feed_supported(cat));
    ASSERT_EQ(mds_catalogue_image_feed_stop(cat), MDS_ERR_NOSUPPORT);

    /* Both slots: arguments forwarded, status passed through. */
    cat = make_fake_cat(&fake_ops_full, &fake_lifecycle_with_feed, 0);
    ASSERT_TRUE(mds_catalogue_image_feed_supported(cat));
    ASSERT_EQ(mds_catalogue_image_feed_start(cat, NULL, 1, 50),
              MDS_ERR_INVAL);
    ASSERT_EQ(fake_feed.start_calls, 0U);
    ASSERT_EQ(mds_catalogue_image_feed_start(cat, img, 9, 75), MDS_OK);
    ASSERT_EQ(fake_feed.start_calls, 1U);
    ASSERT_TRUE(fake_feed.cat == cat);
    ASSERT_TRUE(fake_feed.image == img);
    ASSERT_EQ(fake_feed.self_mds_id, 9U);
    ASSERT_EQ(fake_feed.poll_interval_ms, 75U);
    fake_feed_start_status = MDS_ERR_IO;
    ASSERT_EQ(mds_catalogue_image_feed_start(cat, img, 9, 75), MDS_ERR_IO);
    ASSERT_EQ(mds_catalogue_image_feed_stop(cat), MDS_OK);
    ASSERT_EQ(fake_feed.stop_calls, 1U);

    catalog_image_destroy(img);
}

/* -------------------------------------------------------------------
 * stderr capture (bounded) for message assertions
 * ------------------------------------------------------------------- */

struct stderr_capture {
    int  saved_fd;
    int  fd;
    char path[128];
};

/* Redirect fd 2 into a temp file; returns 0 on success. */
static int stderr_capture_begin(struct stderr_capture *c)
{
    (void)snprintf(c->path, sizeof(c->path),
                   "/tmp/pnfs-cluster-extract-%d.err", (int)getpid());
    (void)fflush(stderr);
    c->saved_fd = dup(STDERR_FILENO);
    c->fd = open(c->path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (c->saved_fd < 0 || c->fd < 0) {
        if (c->saved_fd >= 0) {
            (void)close(c->saved_fd);
        }
        if (c->fd >= 0) {
            (void)close(c->fd);
        }
        return -1;
    }
    (void)dup2(c->fd, STDERR_FILENO);
    return 0;
}

/* Restore fd 2 and copy what was captured (NUL-terminated) into out. */
static void stderr_capture_end(struct stderr_capture *c,
                               char *out, size_t cap)
{
    FILE *ef;
    size_t nread;

    out[0] = '\0';
    (void)fflush(stderr);
    (void)dup2(c->saved_fd, STDERR_FILENO);
    (void)close(c->saved_fd);
    (void)close(c->fd);
    ef = fopen(c->path, "r");
    if (ef != NULL) {
        nread = fread(out, 1, cap - 1, ef);
        out[nread] = '\0';
        (void)fclose(ef);
    }
    (void)unlink(c->path);
}

/* -------------------------------------------------------------------
 * Backend name table (pnfs_common) and registry (core)
 * ------------------------------------------------------------------- */

static bool rondb_compiled_in(void)
{
#ifdef HAVE_RONDB
    return true;
#else
    return false;
#endif
}

static void test_backend_names(void)
{
    enum mds_catalogue_backend be = MDS_BACKEND_NONE;
    enum mds_catalogue_backend two[2] = { MDS_BACKEND_FDB, MDS_BACKEND_RONDB };
    char names[128];

    ASSERT_EQ(mds_catalogue_backend_from_name("rondb", &be), MDS_OK);
    ASSERT_EQ((int)be, (int)MDS_BACKEND_RONDB);
    ASSERT_EQ(mds_catalogue_backend_from_name("memdb", &be), MDS_OK);
    ASSERT_EQ((int)be, (int)MDS_BACKEND_MEMDB);
    ASSERT_EQ(mds_catalogue_backend_from_name("fdb", &be), MDS_OK);
    ASSERT_EQ((int)be, (int)MDS_BACKEND_FDB);

    /* Unknown names leave *out untouched; the lookup is exact. */
    be = MDS_BACKEND_NONE;
    ASSERT_EQ(mds_catalogue_backend_from_name("bogus", &be), MDS_ERR_INVAL);
    ASSERT_EQ(mds_catalogue_backend_from_name("RonDB", &be), MDS_ERR_INVAL);
    ASSERT_EQ(mds_catalogue_backend_from_name("", &be), MDS_ERR_INVAL);
    ASSERT_EQ((int)be, (int)MDS_BACKEND_NONE);
    ASSERT_EQ(mds_catalogue_backend_from_name(NULL, &be), MDS_ERR_INVAL);
    ASSERT_EQ(mds_catalogue_backend_from_name("rondb", NULL), MDS_ERR_INVAL);

    /* enum -> name round trip; the sentinel and garbage have none. */
    ASSERT_STREQ(mds_catalogue_backend_name(MDS_BACKEND_RONDB), "rondb");
    ASSERT_STREQ(mds_catalogue_backend_name(MDS_BACKEND_MEMDB), "memdb");
    ASSERT_STREQ(mds_catalogue_backend_name(MDS_BACKEND_FDB), "fdb");
    ASSERT_TRUE(mds_catalogue_backend_name(MDS_BACKEND_NONE) == NULL);
    ASSERT_TRUE(mds_catalogue_backend_name((enum mds_catalogue_backend)77)
                == NULL);

    /* Every known name, in table order, independent of the build. */
    ASSERT_EQ(mds_catalogue_backend_known_names(names, sizeof(names)), 3U);
    ASSERT_STREQ(names, "rondb, memdb, fdb");

    /* join: order preserved, empty set spelled out, bad ids marked. */
    ASSERT_EQ(mds_catalogue_backend_join_names(two, 2, names, sizeof(names)),
              2U);
    ASSERT_STREQ(names, "fdb, rondb");
    ASSERT_EQ(mds_catalogue_backend_join_names(two, 0, names, sizeof(names)),
              0U);
    ASSERT_STREQ(names, "(none)");
    two[0] = (enum mds_catalogue_backend)77;
    ASSERT_EQ(mds_catalogue_backend_join_names(two, 1, names, sizeof(names)),
              1U);
    ASSERT_STREQ(names, "?");
    /* Truncation stays inside the buffer and keeps the count. */
    memset(names, 'X', sizeof(names));
    ASSERT_EQ(mds_catalogue_backend_known_names(names, 4), 3U);
    ASSERT_TRUE(strlen(names) < 4);
    ASSERT_EQ(mds_catalogue_backend_known_names(NULL, 8), 0U);
    ASSERT_EQ(mds_catalogue_backend_known_names(names, 0), 0U);
}

static void test_backend_registry(void)
{
    char names[128];
    size_t n;
    size_t expect = 0;

    /* fdb is known but never available; NONE and garbage never are. */
    ASSERT_TRUE(!mds_catalogue_backend_available(MDS_BACKEND_FDB));
    ASSERT_TRUE(!mds_catalogue_backend_available(MDS_BACKEND_NONE));
    ASSERT_TRUE(!mds_catalogue_backend_available(
                    (enum mds_catalogue_backend)77));
    /* rondb availability follows the build flavour exactly. */
    ASSERT_EQ((int)mds_catalogue_backend_available(MDS_BACKEND_RONDB),
              (int)rondb_compiled_in());

    /* The name list agrees with the availability predicate. */
    if (mds_catalogue_backend_available(MDS_BACKEND_RONDB)) {
        expect++;
    }
    if (mds_catalogue_backend_available(MDS_BACKEND_MEMDB)) {
        expect++;
    }
    n = mds_catalogue_backend_available_names(names, sizeof(names));
    ASSERT_EQ(n, expect);
    if (n == 0) {
        ASSERT_STREQ(names, "(none)");
    } else {
        ASSERT_TRUE(names[0] != '\0');
        ASSERT_EQ((int)(strstr(names, "rondb") != NULL),
                  (int)mds_catalogue_backend_available(MDS_BACKEND_RONDB));
        ASSERT_EQ((int)(strstr(names, "memdb") != NULL),
                  (int)mds_catalogue_backend_available(MDS_BACKEND_MEMDB));
        ASSERT_TRUE(strstr(names, "fdb") == NULL);
    }
    ASSERT_EQ(mds_catalogue_backend_available_names(NULL, 8), 0U);
    ASSERT_EQ(mds_catalogue_backend_available_names(names, 0), 0U);
}

/* Opening a backend the binary cannot construct fails with the same
 * status the old hard-coded factory used (MDS_ERR_INVAL), never
 * touches *out, and logs the backend name plus the available list. */
static void test_open_unavailable_backend(void)
{
    struct mds_config cfg;
    struct mds_catalogue *cat = (struct mds_catalogue *)&fake_feed;
    struct stderr_capture cap;
    char err[512];

    memset(&cfg, 0, sizeof(cfg));

    /* Known, never compiled in. */
    cfg.catalogue_backend = MDS_BACKEND_FDB;
    ASSERT_EQ(stderr_capture_begin(&cap), 0);
    ASSERT_EQ(mds_catalogue_open(&cfg, &cat), MDS_ERR_INVAL);
    stderr_capture_end(&cap, err, sizeof(err));
    ASSERT_TRUE(cat == (struct mds_catalogue *)&fake_feed);
    ASSERT_TRUE(strstr(err, "catalogue_backend fdb not compiled in") != NULL);
    ASSERT_TRUE(strstr(err, "available:") != NULL);

    /* No backend configured (the no-RonDB default). */
    cfg.catalogue_backend = MDS_BACKEND_NONE;
    ASSERT_EQ(stderr_capture_begin(&cap), 0);
    ASSERT_EQ(mds_catalogue_open(&cfg, &cat), MDS_ERR_INVAL);
    stderr_capture_end(&cap, err, sizeof(err));
    ASSERT_TRUE(strstr(err, "catalogue_backend not set") != NULL);
    ASSERT_TRUE(strstr(err, "available:") != NULL);

    /* Garbage id and NULL arguments. */
    cfg.catalogue_backend = (enum mds_catalogue_backend)77;
    ASSERT_EQ(stderr_capture_begin(&cap), 0);
    ASSERT_EQ(mds_catalogue_open(&cfg, &cat), MDS_ERR_INVAL);
    stderr_capture_end(&cap, err, sizeof(err));
    ASSERT_TRUE(strstr(err, "unknown catalogue_backend 77") != NULL);
    ASSERT_EQ(mds_catalogue_open(NULL, &cat), MDS_ERR_INVAL);
    ASSERT_EQ(mds_catalogue_open(&cfg, NULL), MDS_ERR_INVAL);

    /* memdb / rondb: refused by name when this build lacks them. */
    if (!mds_catalogue_backend_available(MDS_BACKEND_MEMDB)) {
        cfg.catalogue_backend = MDS_BACKEND_MEMDB;
        ASSERT_EQ(stderr_capture_begin(&cap), 0);
        ASSERT_EQ(mds_catalogue_open(&cfg, &cat), MDS_ERR_INVAL);
        stderr_capture_end(&cap, err, sizeof(err));
        ASSERT_TRUE(strstr(err, "catalogue_backend memdb not compiled in")
                    != NULL);
    }
    if (!mds_catalogue_backend_available(MDS_BACKEND_RONDB)) {
        cfg.catalogue_backend = MDS_BACKEND_RONDB;
        ASSERT_EQ(stderr_capture_begin(&cap), 0);
        ASSERT_EQ(mds_catalogue_open(&cfg, &cat), MDS_ERR_INVAL);
        stderr_capture_end(&cap, err, sizeof(err));
        ASSERT_TRUE(strstr(err, "catalogue_backend rondb not compiled in")
                    != NULL);
    }
    ASSERT_TRUE(cat == (struct mds_catalogue *)&fake_feed);
}

/* -------------------------------------------------------------------
 * Config parsing of catalogue_backend in both build flavours
 * ------------------------------------------------------------------- */

static int write_tmp_ini(const char *content, char path_out[128])
{
    FILE *f;

    (void)snprintf(path_out, 128, "/tmp/pnfs-cluster-extract-%d.conf",
                   (int)getpid());
    f = fopen(path_out, "w");
    if (f == NULL) {
        return -1;
    }
    (void)fputs(content, f);
    (void)fclose(f);
    return 0;
}

/* Parse @ini with stderr captured into @err_out (bounded). */
static enum mds_status load_capturing_stderr(const char *ini,
                                             struct mds_config *cfg,
                                             char *err_out, size_t err_cap)
{
    char cfg_path[128];
    struct stderr_capture cap;
    enum mds_status st;

    err_out[0] = '\0';
    if (write_tmp_ini(ini, cfg_path) != 0) {
        return MDS_ERR_IO;
    }
    if (stderr_capture_begin(&cap) != 0) {
        (void)unlink(cfg_path);
        return MDS_ERR_IO;
    }
    st = mds_config_load(cfg_path, cfg);
    stderr_capture_end(&cap, err_out, err_cap);
    (void)unlink(cfg_path);
    return st;
}

/* The parser knows names, not builds: every KNOWN name parses in every
 * build flavour (mds_catalogue_open refuses the unavailable ones, see
 * test_open_unavailable_backend); only an unknown name is a parse
 * error, and it lists the known names. */
static void test_config_backend_parsing(void)
{
    struct mds_config cfg;
    char err[512];
    enum mds_status st;

    /* Missing key: rondb when compiled in, otherwise no backend at all
     * (never a silent fallback to whatever else is built). */
    st = load_capturing_stderr("mds_id = 3\n", &cfg, err, sizeof(err));
    ASSERT_EQ(st, MDS_OK);
    ASSERT_EQ((int)cfg.catalogue_backend,
              rondb_compiled_in() ? (int)MDS_BACKEND_RONDB
                                  : (int)MDS_BACKEND_NONE);
    ASSERT_EQ(cfg.self.id, 3U);

    /* Known names parse regardless of availability. */
    st = load_capturing_stderr("catalogue_backend = fdb\n", &cfg,
                               err, sizeof(err));
    ASSERT_EQ(st, MDS_OK);
    ASSERT_EQ((int)cfg.catalogue_backend, (int)MDS_BACKEND_FDB);
    st = load_capturing_stderr("catalogue_backend = memdb\n", &cfg,
                               err, sizeof(err));
    ASSERT_EQ(st, MDS_OK);
    ASSERT_EQ((int)cfg.catalogue_backend, (int)MDS_BACKEND_MEMDB);
    st = load_capturing_stderr("catalogue_backend = rondb\n", &cfg,
                               err, sizeof(err));
    ASSERT_EQ(st, MDS_OK);
    ASSERT_EQ((int)cfg.catalogue_backend, (int)MDS_BACKEND_RONDB);
    /* Last key wins, like every other key. */
    st = load_capturing_stderr("catalogue_backend = fdb\n"
                               "catalogue_backend = memdb\n", &cfg,
                               err, sizeof(err));
    ASSERT_EQ(st, MDS_OK);
    ASSERT_EQ((int)cfg.catalogue_backend, (int)MDS_BACKEND_MEMDB);

    /* Unknown name: parse error naming it and listing the known names
     * (not the available ones -- the parser does not know the build). */
    st = load_capturing_stderr("catalogue_backend = bogus\n", &cfg,
                               err, sizeof(err));
    ASSERT_EQ(st, MDS_ERR_INVAL);
    ASSERT_TRUE(strstr(err, "unknown catalogue_backend 'bogus'") != NULL);
    ASSERT_TRUE(strstr(err, "known: rondb, memdb, fdb") != NULL);
    st = load_capturing_stderr("catalogue_backend = RonDB\n", &cfg,
                               err, sizeof(err));
    ASSERT_EQ(st, MDS_ERR_INVAL);
    ASSERT_TRUE(strstr(err, "unknown catalogue_backend 'RonDB'") != NULL);
}

/* -------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */

int main(void)
{
    fprintf(stdout, "test_cluster_extract\n");

    /* The factory reports refusals through MDS_LOG_ERROR, which drops
     * every record until the logger is initialised; route it to stderr
     * so the message assertions below can capture it. */
    mds_log_init(NULL);

    RUN_TEST(test_cluster_supported_predicates);
    RUN_TEST(test_cluster_dispatch_args_and_passthrough);
    RUN_TEST(test_subtree_init_loads_rows);
    RUN_TEST(test_subtree_init_list_failure_seeds_root);
    RUN_TEST(test_subtree_refresh_from_catalogue);
    RUN_TEST(test_subtree_seed_shards);
    RUN_TEST(test_membership_populate);
    RUN_TEST(test_watchdog_start_contract);
    RUN_TEST(test_image_feed_dispatch);
    RUN_TEST(test_backend_names);
    RUN_TEST(test_backend_registry);
    RUN_TEST(test_open_unavailable_backend);
    RUN_TEST(test_config_backend_parsing);

    fprintf(stdout, "\n  %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
