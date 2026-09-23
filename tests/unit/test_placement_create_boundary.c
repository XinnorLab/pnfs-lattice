/*
 * SPDX-License-Identifier: MIT
 *
 * test_placement_create_boundary.c -- the create-if-absent boundary
 * (design section 5a): lookup never creates, create needs a token, the
 * ensure helpers refuse a new object on a DS the gate rejects while an
 * existing object stays reachable, and legacy mode creates freely.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#include "pnfs_mds.h"
#include "placement_gate.h"
#include "proxy_io.h"
#include "mds_catalogue.h"
#include "ds_cache.h"

struct mds_catalogue *catalogue_memdb_open(void);

static int tests_run;
static int tests_passed;
static int test_failed;

#define ASSERT_EQ(a, b) do { \
    if ((long long)(a) != (long long)(b)) { \
        fprintf(stderr, "  FAIL %s:%d: %s (%lld) != %s (%lld)\n", \
                __FILE__, __LINE__, #a, (long long)(a), #b, (long long)(b)); \
        test_failed = 1; \
        return; \
    } \
} while (0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL %s:%d: !(%s)\n", __FILE__, __LINE__, #cond); \
        test_failed = 1; \
        return; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    tests_run++; \
    test_failed = 0; \
    fprintf(stdout, "  %-58s", #fn); \
    fflush(stdout); \
    fn(); \
    if (test_failed == 0) { tests_passed++; fprintf(stdout, "PASS\n"); } \
    else { fprintf(stdout, "FAILED\n"); } \
} while (0)

static char *make_ds_dir(void)
{
    char *tpl = strdup("/tmp/test_pmcb_XXXXXX");
    if (tpl == NULL || mkdtemp(tpl) == NULL) {
        free(tpl);
        return NULL;
    }
    return tpl;
}

static void rm_ds_dir(char *path)
{
    char cmd[4200];
    if (path == NULL) {
        return;
    }
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
    free(path);
}

static struct ds_cache *cache_with_ds(struct mds_catalogue **cat_out, uint32_t ds_id)
{
    struct mds_catalogue *cat = catalogue_memdb_open();
    struct mds_cat_txn *txn = NULL;
    struct mds_ds_info info;
    struct ds_cache *c = NULL;

    if (cat == NULL) {
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    info.ds_id = ds_id;
    info.state = DS_ONLINE;
    info.port = 2049;
    snprintf(info.host, sizeof(info.host), "ds-host");
    if (mds_cat_txn_begin(cat, MDS_CAT_TXN_WRITE, &txn) != MDS_OK ||
        mds_cat_ds_put(cat, txn, &info) != MDS_OK ||
        mds_cat_txn_commit(txn) != MDS_OK) {
        return NULL;
    }
    if (ds_cache_create(cat, &c) != 0) {
        return NULL;
    }
    *cat_out = cat;
    return c;
}

static int file_exists(const char *dir, uint64_t fileid, uint32_t s, uint32_t m)
{
    char p[4200];
    struct stat st;
    snprintf(p, sizeof(p), "%s/data/%llu_%u_%u", dir, (unsigned long long)fileid, s, m);
    return stat(p, &st) == 0;
}

static void test_legacy_mode_creates_freely(void)
{
    char *dir = make_ds_dir();
    struct mds_proxy_ctx *proxy = NULL;
    ASSERT_TRUE(dir != NULL);
    ASSERT_EQ(mds_proxy_ctx_create(&proxy), MDS_OK);
    ASSERT_EQ(mds_proxy_mount_set(proxy, 1, dir), MDS_OK);
    ASSERT_EQ(placement_gate_mode(), PM_LEGACY);
    ASSERT_EQ(mds_proxy_ensure_ds_file(proxy, 1, 7, 0, 0), MDS_OK);
    ASSERT_TRUE(file_exists(dir, 7, 0, 0));
    uint8_t fh[MDS_NFS_FH_MAX]; uint32_t fl = sizeof(fh);
    /* name_to_handle_at on tmpfs may or may not work; the handle path
     * itself is upstream's; what matters here is that the file exists. */
    (void)mds_proxy_ensure_ds_file_fh(proxy, 1, 8, 0, 0, fh, &fl);
    ASSERT_TRUE(file_exists(dir, 8, 0, 0));
    mds_proxy_ctx_destroy(proxy);
    rm_ds_dir(dir);
}

static void test_token_is_bound_to_ds_and_age(void)
{
    struct placement_token t = { 1, PP_NEW_OBJECT, 1000, 0 };
    ASSERT_EQ(placement_token_valid(&t, 1, 1000), true);
    ASSERT_EQ(placement_token_valid(&t, 1, 3000), true);    /* exactly max age */
    ASSERT_EQ(placement_token_valid(&t, 1, 3001), false);
    ASSERT_EQ(placement_token_valid(&t, 2, 1500), false);
    ASSERT_EQ(placement_token_valid(&t, 1, 999), false);    /* minted in the future */
    ASSERT_EQ(placement_token_valid(NULL, 1, 1500), false);
    t.purpose = 9;
    ASSERT_EQ(placement_token_valid(&t, 1, 1500), false);
}

static void test_create_refuses_without_token(void)
{
    char *dir = make_ds_dir();
    struct mds_proxy_ctx *proxy = NULL;
    ASSERT_TRUE(dir != NULL);
    ASSERT_EQ(mds_proxy_ctx_create(&proxy), MDS_OK);
    ASSERT_EQ(mds_proxy_mount_set(proxy, 1, dir), MDS_OK);
    ASSERT_EQ(mds_proxy_create_ds_file(proxy, 1, 5, 0, 0, NULL), MDS_ERR_INVAL);
    struct placement_token wrong = { 2, PP_NEW_OBJECT, ds_cache_mono_ms(), 0 };
    ASSERT_EQ(mds_proxy_create_ds_file(proxy, 1, 5, 0, 0, &wrong), MDS_ERR_INVAL);
    uint8_t fh[MDS_NFS_FH_MAX]; uint32_t fl = sizeof(fh);
    ASSERT_EQ(mds_proxy_create_ds_file_fh(proxy, 1, 5, 0, 0, &wrong, fh, &fl), MDS_ERR_INVAL);
    ASSERT_TRUE(!file_exists(dir, 5, 0, 0));
    struct placement_token good = { 1, PP_NEW_OBJECT, ds_cache_mono_ms(), 0 };
    ASSERT_EQ(mds_proxy_create_ds_file(proxy, 1, 5, 0, 0, &good), MDS_OK);
    ASSERT_TRUE(file_exists(dir, 5, 0, 0));
    mds_proxy_ctx_destroy(proxy);
    rm_ds_dir(dir);
}

static void test_admit_create_legacy_and_rr(void)
{
    struct placement_token tok;
    enum placement_reason why;
    ASSERT_EQ(placement_gate_admit_create(1, PP_NEW_OBJECT, NULL, &why), MDS_ERR_INVAL);
    ASSERT_EQ(placement_gate_admit_create(1, PP_NEW_OBJECT, &tok, &why), MDS_OK);   /* legacy */
    ASSERT_EQ(tok.ds_id, 1u);
    ASSERT_EQ(placement_token_valid(&tok, 1, ds_cache_mono_ms()), true);

    struct mds_catalogue *cat = NULL;
    struct ds_cache *cache = cache_with_ds(&cat, 1);
    ASSERT_TRUE(cache != NULL);
    struct mds_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.placement_mode = PM_RR;
    cfg.placement_mode_set = true;
    ASSERT_EQ(placement_gate_init(&cfg, cache), 0);
    ASSERT_EQ(placement_gate_admit_create(1, PP_RECREATE_MISSING, &tok, &why), MDS_OK);
    ASSERT_EQ(placement_gate_admit_create(2, PP_RECREATE_MISSING, &tok, &why), MDS_ERR_NOSPC);  /* unregistered */
    ASSERT_EQ(why, PR_DS_OFFLINE);
    placement_gate_destroy();
    ds_cache_destroy(cache);
    mds_catalogue_close(cat);
}

static void test_ensure_creates_in_rr_and_refuses_full_ds_in_fill(void)
{
    struct mds_catalogue *cat = NULL;
    struct ds_cache *cache = cache_with_ds(&cat, 1);
    char *dir = make_ds_dir();
    struct mds_proxy_ctx *proxy = NULL;
    struct mds_config cfg;
    ASSERT_TRUE(cache != NULL && dir != NULL);
    ASSERT_EQ(mds_proxy_ctx_create(&proxy), MDS_OK);
    ASSERT_EQ(mds_proxy_mount_set(proxy, 1, dir), MDS_OK);

    memset(&cfg, 0, sizeof(cfg));
    cfg.placement_mode = PM_RR;
    cfg.placement_mode_set = true;
    ASSERT_EQ(placement_gate_init(&cfg, cache), 0);
    ASSERT_EQ(mds_proxy_ensure_ds_file(proxy, 1, 100, 0, 0), MDS_OK);      /* rr: created */
    ASSERT_TRUE(file_exists(dir, 100, 0, 0));
    placement_gate_destroy();

    cfg.placement_mode = PM_FILL;
    cfg.placement_capacity_max_age_ms = 120000;
    ASSERT_EQ(placement_gate_init(&cfg, cache), 0);
    {
        struct ds_capacity_obs full = { 1000, 0, 1, ds_cache_mono_ms(), 0 };
        ASSERT_EQ(ds_cache_set_capacity_obs(cache, 1, &full), 0);
    }
    placement_gate_publish_capacity();
    ASSERT_EQ(mds_proxy_ensure_ds_file(proxy, 1, 100, 0, 0), MDS_OK);      /* exists: not gated */
    ASSERT_EQ(mds_proxy_ensure_ds_file(proxy, 1, 101, 0, 0), MDS_ERR_NOSPC); /* absent + full */
    ASSERT_TRUE(!file_exists(dir, 101, 0, 0));
    {
        uint8_t fh[MDS_NFS_FH_MAX]; uint32_t fl = sizeof(fh);
        ASSERT_EQ(mds_proxy_ensure_ds_file_fh(proxy, 1, 101, 0, 0, fh, &fl), MDS_ERR_NOSPC);
        ASSERT_TRUE(!file_exists(dir, 101, 0, 0));
        fl = sizeof(fh);
        ASSERT_EQ(mds_proxy_lookup_ds_file_fh(proxy, 1, 101, 0, 0, fh, &fl), MDS_ERR_NOTFOUND);
        ASSERT_TRUE(!file_exists(dir, 101, 0, 0));
    }
    {
        struct ds_capacity_obs half = { 1000, 500, 1, ds_cache_mono_ms(), 0 };
        ASSERT_EQ(ds_cache_set_capacity_obs(cache, 1, &half), 0);
    }
    placement_gate_publish_capacity();
    ASSERT_EQ(mds_proxy_ensure_ds_file(proxy, 1, 101, 0, 0), MDS_OK);
    ASSERT_TRUE(file_exists(dir, 101, 0, 0));
    /* stale observation: refused again for a new object, existing one fine.
     * CLOCK_MONOTONIC may be small on a fresh CI runner, so the staleness
     * comes from a tiny max age, not from a large subtraction. */
    placement_gate_destroy();
    cfg.placement_capacity_max_age_ms = 1;
    ASSERT_EQ(placement_gate_init(&cfg, cache), 0);
    {
        uint64_t now = ds_cache_mono_ms();
        struct ds_capacity_obs stale = { 1000, 500, 1, (now > 50) ? now - 50 : 1, 0 };
        ASSERT_EQ(ds_cache_set_capacity_obs(cache, 1, &stale), 0);
    }
    placement_gate_publish_capacity();
    ASSERT_EQ(mds_proxy_ensure_ds_file(proxy, 1, 101, 0, 0), MDS_OK);
    ASSERT_EQ(mds_proxy_ensure_ds_file(proxy, 1, 102, 0, 0), MDS_ERR_NOSPC);
    ASSERT_TRUE(!file_exists(dir, 102, 0, 0));

    placement_gate_destroy();
    mds_proxy_ctx_destroy(proxy);
    rm_ds_dir(dir);
    ds_cache_destroy(cache);
    mds_catalogue_close(cat);
}

static void test_write_direct_does_not_create_on_a_refused_ds(void)
{
    struct mds_catalogue *cat = NULL;
    struct ds_cache *cache = cache_with_ds(&cat, 1);
    char *dir = make_ds_dir();
    struct mds_proxy_ctx *proxy = NULL;
    struct mds_config cfg;
    ASSERT_TRUE(cache != NULL && dir != NULL);
    ASSERT_EQ(mds_proxy_ctx_create(&proxy), MDS_OK);
    ASSERT_EQ(mds_proxy_mount_set(proxy, 1, dir), MDS_OK);
    memset(&cfg, 0, sizeof(cfg));
    cfg.placement_mode = PM_FILL;
    cfg.placement_mode_set = true;
    cfg.placement_capacity_max_age_ms = 120000;
    ASSERT_EQ(placement_gate_init(&cfg, cache), 0);
    {
        struct ds_capacity_obs full = { 1000, 0, 1, ds_cache_mono_ms(), 0 };
        ASSERT_EQ(ds_cache_set_capacity_obs(cache, 1, &full), 0);
    }
    placement_gate_publish_capacity();
    {
        const char payload[8] = "abcdefg";
        enum mds_status st = mds_proxy_write_direct(proxy, 1, 300, 0, 0, 0,
                                                    payload, sizeof(payload));
        ASSERT_TRUE(st != MDS_OK);
        ASSERT_TRUE(!file_exists(dir, 300, 0, 0));
    }
    placement_gate_destroy();
    mds_proxy_ctx_destroy(proxy);
    rm_ds_dir(dir);
    ds_cache_destroy(cache);
    mds_catalogue_close(cat);
}

int main(void)
{
    printf("test_placement_create_boundary\n");
    RUN_TEST(test_legacy_mode_creates_freely);
    RUN_TEST(test_token_is_bound_to_ds_and_age);
    RUN_TEST(test_create_refuses_without_token);
    RUN_TEST(test_admit_create_legacy_and_rr);
    RUN_TEST(test_ensure_creates_in_rr_and_refuses_full_ds_in_fill);
    RUN_TEST(test_write_direct_does_not_create_on_a_refused_ds);
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
