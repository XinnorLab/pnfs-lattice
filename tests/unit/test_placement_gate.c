/*
 * SPDX-License-Identifier: MIT
 *
 * test_placement_gate.c -- the candidate gate and admit for rr / fill
 * over synthetic DS lists and capacity views (design sections 5, 6).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pnfs_mds.h"
#include "placement_gate.h"
#include "mds_catalogue.h"
#include "ds_capacity.h"
#include "wrr.h"
#include "mds_metrics.h"
#include "ds_prealloc.h"

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

/* -----------------------------------------------------------------------
 * Fixtures
 * ----------------------------------------------------------------------- */

static struct placement_capacity_view V;
static char DOM[MDS_MAX_DS_NODES][PM_DOMAIN_ID_MAX];
static _Atomic uint32_t RR;

static void mk_ds(struct mds_ds_info *ds, uint32_t id, uint32_t state, const char *host)
{
    memset(ds, 0, sizeof(*ds));
    ds->ds_id = id;
    ds->state = state;
    ds->mode = DS_MODE_GENERIC;
    ds->transport = DS_TRANSPORT_TCP;
    snprintf(ds->host, sizeof(ds->host), "%s", host);
}

static struct placement_ctx ctx_for(enum placement_mode m)
{
    struct placement_ctx c;
    memset(&c, 0, sizeof(c));
    c.mode = m;
    c.shrink = PM_SHRINK_ALLOW;
    c.now_mono_ms = 1000000;
    c.capacity_max_age_ms = 120000;
    c.min_free_bytes = 0;
    c.domain_of = (const char (*)[PM_DOMAIN_ID_MAX])DOM;
    c.cap = &V;
    c.rr_counter = &RR;
    return c;
}

static void add_row(uint32_t id, const char *host, uint64_t total, uint64_t avail,
                    uint64_t fsid, uint64_t t)
{
    struct ds_capacity_view_row *r = &V.rows[V.count++];
    memset(r, 0, sizeof(*r));
    r->ds_id = id;
    r->state = DS_ONLINE;
    snprintf(r->host, sizeof(r->host), "%s", host);
    r->obs.total_bytes = total;
    r->obs.avail_bytes = avail;
    r->obs.fsid = fsid;
    r->obs.observed_mono_ms = t;
}

static void reset(void)
{
    memset(&V, 0, sizeof(V));
    memset(DOM, 0, sizeof(DOM));
    RR = 0;
}

/* -----------------------------------------------------------------------
 * rr
 * ----------------------------------------------------------------------- */

static void test_rr_is_cyclic_over_the_gated_list(void)
{
    struct mds_ds_info ds[3];
    uint32_t seq[6];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "a");
    mk_ds(&ds[1], 1, DS_OFFLINE, "b");
    mk_ds(&ds[2], 2, DS_ONLINE, "c");
    struct placement_ctx c = ctx_for(PM_RR);
    for (int i = 0; i < 6; i++) {
        struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
        ASSERT_EQ(placement_admit(&c, ds, 3, &sc, 1, 0, &e, &why), MDS_OK);
        ASSERT_EQ(sc, 1u);
        seq[i] = e.ds_id;
    }
    for (int i = 0; i < 6; i++) {
        ASSERT_TRUE(seq[i] == 0 || seq[i] == 2);
    }
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(seq[i] != seq[i + 1]);   /* strict alternation */
    }
}

static void test_rr_ignores_capacity(void)
{
    struct mds_ds_info ds[1];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "a");
    struct placement_ctx c = ctx_for(PM_RR);
    c.cap = NULL;
    struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
    ASSERT_EQ(placement_admit(&c, ds, 1, &sc, 1, 0, &e, &why), MDS_OK);
    ASSERT_EQ(e.ds_id, 0u);
}

static void test_rr_mirrors_are_distinct_and_shrink(void)
{
    struct mds_ds_info ds[3];
    reset();
    for (uint32_t i = 0; i < 3; i++) mk_ds(&ds[i], i, DS_ONLINE, "h");
    struct placement_ctx c = ctx_for(PM_RR);
    struct mds_ds_map_entry e[8]; uint32_t sc = 4; enum placement_reason why;
    ASSERT_EQ(placement_admit(&c, ds, 3, &sc, 2, 0, e, &why), MDS_OK);
    ASSERT_EQ(sc, 1u);                         /* 3 DS / 2 mirrors -> 1 stripe */
    ASSERT_TRUE(e[0].ds_id != e[1].ds_id);
    c.shrink = PM_SHRINK_STRICT; sc = 4;
    ASSERT_EQ(placement_admit(&c, ds, 3, &sc, 2, 0, e, &why), MDS_ERR_NOSPC);
    ASSERT_EQ(why, PR_INSUFFICIENT_ELIGIBLE_DS);
    ASSERT_EQ(sc, 4u);
}

/* -----------------------------------------------------------------------
 * fill: capacity gate
 * ----------------------------------------------------------------------- */

static void test_fill_excludes_unknown_stale_and_full(void)
{
    struct mds_ds_info ds[4];
    reset();
    for (uint32_t i = 0; i < 4; i++) mk_ds(&ds[i], i, DS_ONLINE, "h");
    add_row(0, "h", 1000, 500, 1, 1000000);            /* fresh, half free */
    add_row(1, "h", 1000, 500, 2, 1000000 - 130000);   /* stale */
    add_row(2, "h", 1000, 0, 3, 1000000);              /* full */
    add_row(3, "h", 0, 0, 4, 0);                       /* never observed */
    struct placement_ctx c = ctx_for(PM_FILL);
    struct placement_candidate out[4]; struct placement_reject_counts why;
    ASSERT_EQ(placement_candidates(&c, ds, 4, out, &why), 1u);
    ASSERT_EQ(out[0].ds_id, 0u);
    ASSERT_EQ(why.by_reason[PR_CAPACITY_STALE], 1u);
    ASSERT_EQ(why.by_reason[PR_CAPACITY_FULL], 1u);
    ASSERT_EQ(why.by_reason[PR_CAPACITY_UNKNOWN], 1u);
}

static void test_fill_ds_missing_from_view_is_unknown(void)
{
    struct mds_ds_info ds[1];
    reset();
    mk_ds(&ds[0], 7, DS_ONLINE, "h");
    struct placement_ctx c = ctx_for(PM_FILL);
    struct placement_candidate out[1]; struct placement_reject_counts why;
    ASSERT_EQ(placement_candidates(&c, ds, 1, out, &why), 0u);
    ASSERT_EQ(why.by_reason[PR_CAPACITY_UNKNOWN], 1u);
}

static void test_fill_min_free_gate(void)
{
    struct mds_ds_info ds[1];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "h");
    add_row(0, "h", 1000, 100, 1, 1000000);
    struct placement_ctx c = ctx_for(PM_FILL);
    c.min_free_bytes = 100;   /* avail must be > 100 */
    struct placement_candidate out[1];
    ASSERT_EQ(placement_candidates(&c, ds, 1, out, NULL), 0u);
    c.min_free_bytes = 99;
    ASSERT_EQ(placement_candidates(&c, ds, 1, out, NULL), 1u);
}

static void test_fill_freshness_boundary(void)
{
    struct mds_ds_info ds[1];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "h");
    add_row(0, "h", 1000, 500, 1, 1000000 - 120000);   /* exactly max age: fresh */
    struct placement_ctx c = ctx_for(PM_FILL);
    struct placement_candidate out[1];
    ASSERT_EQ(placement_candidates(&c, ds, 1, out, NULL), 1u);
    V.rows[0].obs.observed_mono_ms = 1000000 - 120001;
    ASSERT_EQ(placement_candidates(&c, ds, 1, out, NULL), 0u);
    /* an observation from the future is corrupt: not fresh (fail closed) */
    V.rows[0].obs.observed_mono_ms = 1000000 + 1;
    struct placement_reject_counts why;
    ASSERT_EQ(placement_candidates(&c, ds, 1, out, &why), 0u);
    ASSERT_EQ(why.by_reason[PR_CAPACITY_STALE], 1u);
}

/* -----------------------------------------------------------------------
 * fill: weights and domains
 * ----------------------------------------------------------------------- */

static void test_fill_weight_is_fill_level_over_aliases(void)
{
    struct mds_ds_info ds[3];
    reset();
    for (uint32_t i = 0; i < 3; i++) mk_ds(&ds[i], i, DS_ONLINE, "xi");
    add_row(0, "xi", 1000, 800, 7, 1000000);   /* domain d: 80 % free, aliases 0 and 1 */
    add_row(1, "xi", 1000, 800, 7, 1000000);
    add_row(2, "xi", 1000, 200, 8, 1000000);   /* own domain: 20 % free */
    snprintf(DOM[0], PM_DOMAIN_ID_MAX, "d");
    snprintf(DOM[1], PM_DOMAIN_ID_MAX, "d");
    struct placement_ctx c = ctx_for(PM_FILL);
    struct placement_candidate out[3];
    ASSERT_EQ(placement_candidates(&c, ds, 3, out, NULL), 3u);
    ASSERT_TRUE(out[0].weight == placement_weight(80, 1000000, 2, NULL));
    ASSERT_TRUE(out[1].weight == out[0].weight);
    ASSERT_TRUE(out[2].weight == placement_weight(20, 1000000, 1, NULL));
    ASSERT_TRUE(out[0].weight * 2 == out[2].weight * 4);   /* domain totals 80 : 20 */
    ASSERT_EQ(strcmp(out[0].domain, "d"), 0);
    ASSERT_EQ(strcmp(out[2].domain, "ds:2"), 0);
}

static void test_alias_share_counts_offline_members(void)
{
    struct mds_ds_info ds[2];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "xi");
    mk_ds(&ds[1], 1, DS_OFFLINE, "xi");
    add_row(0, "xi", 1000, 800, 7, 1000000);
    add_row(1, "xi", 1000, 800, 7, 1000000);
    snprintf(DOM[0], PM_DOMAIN_ID_MAX, "d");
    snprintf(DOM[1], PM_DOMAIN_ID_MAX, "d");
    struct placement_ctx c = ctx_for(PM_FILL);
    struct placement_candidate out[2];
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, NULL), 1u);
    ASSERT_TRUE(out[0].weight == placement_weight(80, 1000000, 2, NULL));   /* still 1/2 */
}

static void test_domain_canonical_observation_is_lowest_id_and_conservative(void)
{
    struct mds_ds_info ds[2];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "xi");
    mk_ds(&ds[1], 1, DS_ONLINE, "xi");
    add_row(0, "xi", 1000, 800, 7, 1000000);
    add_row(1, "xi", 1000, 400, 7, 1000000);   /* > 1 % apart */
    snprintf(DOM[0], PM_DOMAIN_ID_MAX, "d");
    snprintf(DOM[1], PM_DOMAIN_ID_MAX, "d");
    struct placement_ctx c = ctx_for(PM_FILL);
    struct placement_candidate out[2];
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, NULL), 2u);
    ASSERT_TRUE(out[0].weight == placement_weight(40, 1000000, 2, NULL));   /* the smaller avail wins */
    /* within 1 %: the canonical (lowest id) value is used */
    V.rows[1].obs.avail_bytes = 795;
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, NULL), 2u);
    ASSERT_TRUE(out[0].weight == placement_weight(80, 1000000, 2, NULL));
    /* only the stale sibling observed: the fresh one is canonical */
    V.rows[0].obs.observed_mono_ms = 1000000 - 500000;
    V.rows[1].obs.avail_bytes = 400;
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, NULL), 2u);
    ASSERT_TRUE(out[0].weight == placement_weight(40, 1000000, 2, NULL));
}

static void test_alias_grades(void)
{
    struct mds_ds_info ds[2];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "xi");
    mk_ds(&ds[1], 1, DS_ONLINE, "xi");
    add_row(0, "xi", 1000, 800, 7, 1000000);
    add_row(1, "xi", 1000, 800, 7, 1000000);
    struct placement_ctx c = ctx_for(PM_FILL);
    struct placement_candidate out[2]; struct placement_reject_counts why;
    /* (b) same host + same fsid, no map -> proven alias, both excluded */
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, &why), 0u);
    ASSERT_EQ(why.by_reason[PR_SHARED_FS_ALIAS_UNMAPPED], 2u);
    /* declared -> fine */
    snprintf(DOM[0], PM_DOMAIN_ID_MAX, "d");
    snprintf(DOM[1], PM_DOMAIN_ID_MAX, "d");
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, &why), 2u);
    /* (a) declared one domain but fsid differs on the same host -> contradiction */
    V.rows[1].obs.fsid = 8;
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, &why), 0u);
    ASSERT_EQ(why.by_reason[PR_DOMAIN_MAP_CONTRADICTION], 2u);
    /* (c) same fsid behind different host strings, no map -> suspected only */
    memset(DOM, 0, sizeof(DOM));
    V.rows[1].obs.fsid = 7;
    snprintf(V.rows[1].host, MDS_DS_HOST_MAX, "xi-alt");
    snprintf(ds[1].host, MDS_DS_HOST_MAX, "xi-alt");
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, &why), 2u);
}

static void test_weight_bounds(void)
{
    bool ovf = true;
    ASSERT_EQ(placement_weight(1, 1, 256, &ovf), 256u);
    ASSERT_EQ(ovf, false);
    ASSERT_EQ(placement_weight(0, 1000000, 1, &ovf), 0u);
    ASSERT_EQ(placement_weight(100, 0, 1, &ovf), 0u);
    ASSERT_EQ(placement_weight(100, 1000000, 0, &ovf), 0u);
    uint64_t max = placement_weight(10000, 1000000, 1, &ovf);
    ASSERT_EQ(ovf, false);
    ASSERT_TRUE(max > 0);
    ASSERT_TRUE(max < (UINT64_C(1) << 62) / 256);   /* 256 such weights stay below 2^62 */
    ASSERT_EQ(placement_weight(10001, 1000000, 1, &ovf), 0u);   /* above the manual range */
    ASSERT_EQ(ovf, true);
    ASSERT_EQ(placement_weight(100000, 1000000, 1, &ovf), 0u);
    ASSERT_EQ(ovf, true);
}

/* -----------------------------------------------------------------------
 * fill: admit
 * ----------------------------------------------------------------------- */

static void test_fill_multi_stripe_distinct_and_shrink_vs_strict(void)
{
    struct mds_ds_info ds[3];
    reset();
    for (uint32_t i = 0; i < 3; i++) {
        mk_ds(&ds[i], i, DS_ONLINE, "h");
        add_row(i, "h", 1000, 500, 10 + i, 1000000);
    }
    struct placement_ctx c = ctx_for(PM_FILL);
    struct mds_ds_map_entry e[4]; uint32_t sc = 4; enum placement_reason why;
    ASSERT_EQ(placement_admit(&c, ds, 3, &sc, 1, 0, e, &why), MDS_OK);
    ASSERT_EQ(sc, 3u);
    ASSERT_TRUE(e[0].ds_id != e[1].ds_id && e[1].ds_id != e[2].ds_id && e[0].ds_id != e[2].ds_id);
    c.shrink = PM_SHRINK_STRICT; sc = 4;
    ASSERT_EQ(placement_admit(&c, ds, 3, &sc, 1, 0, e, &why), MDS_ERR_NOSPC);
    ASSERT_EQ(why, PR_INSUFFICIENT_ELIGIBLE_DS);
    ASSERT_EQ(sc, 4u);   /* unchanged on error */
}

static void test_fill_mirrors_are_distinct(void)
{
    struct mds_ds_info ds[4];
    reset();
    for (uint32_t i = 0; i < 4; i++) {
        mk_ds(&ds[i], i, DS_ONLINE, "h");
        add_row(i, "h", 1000, 500, 10 + i, 1000000);
    }
    struct placement_ctx c = ctx_for(PM_FILL);
    struct mds_ds_map_entry e[4]; uint32_t sc = 2; enum placement_reason why;
    mds_wrr_test_seed(3);
    for (int t = 0; t < 200; t++) {
        sc = 2;
        ASSERT_EQ(placement_admit(&c, ds, 4, &sc, 2, 0, e, &why), MDS_OK);
        ASSERT_EQ(sc, 2u);
        for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++) ASSERT_TRUE(e[i].ds_id != e[j].ds_id);
    }
}

static void test_no_candidate_is_nospc_never_ds0(void)
{
    struct mds_ds_info ds[2];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "h");
    mk_ds(&ds[1], 1, DS_ONLINE, "h");
    add_row(0, "h", 1000, 0, 1, 1000000);
    add_row(1, "h", 1000, 0, 2, 1000000);
    struct placement_ctx c = ctx_for(PM_FILL);
    struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
    e.ds_id = 77;
    ASSERT_EQ(placement_admit(&c, ds, 2, &sc, 1, 0, &e, &why), MDS_ERR_NOSPC);
    ASSERT_EQ(why, PR_NO_ELIGIBLE_DS);
    ASSERT_EQ(e.ds_id, 0u);   /* entries zeroed; the caller checks the status first */
}

static void test_smart_without_assessments_is_not_ready(void)
{
    struct mds_ds_info ds[1];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "h");
    add_row(0, "h", 1000, 500, 1, 1000000);
    struct placement_ctx c = ctx_for(PM_SMART);
    struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
    ASSERT_EQ(placement_admit(&c, ds, 1, &sc, 1, 0, &e, &why), MDS_ERR_NOSPC);
    ASSERT_EQ(why, PR_MODE_NOT_READY);
}

static void test_fill_fairness_4_to_1(void)
{
    struct mds_ds_info ds[2];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "a");
    mk_ds(&ds[1], 1, DS_ONLINE, "b");
    add_row(0, "a", 1000, 800, 1, 1000000);
    add_row(1, "b", 1000, 200, 2, 1000000);
    struct placement_ctx c = ctx_for(PM_FILL);
    mds_wrr_test_seed(4242);
    uint32_t hits0 = 0;
    for (int i = 0; i < 100000; i++) {
        struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
        ASSERT_EQ(placement_admit(&c, ds, 2, &sc, 1, 0, &e, &why), MDS_OK);
        if (e.ds_id == 0) hits0++;
    }
    ASSERT_TRUE(hits0 > 78000 && hits0 < 82000);
}

static void test_fill_equal_fill_is_even(void)
{
    struct mds_ds_info ds[2];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "a");
    mk_ds(&ds[1], 1, DS_ONLINE, "b");
    add_row(0, "a", 40000, 39000, 1, 1000000);   /* 97.5 % free, bigger */
    add_row(1, "b", 30000, 29250, 2, 1000000);   /* 97.5 % free, smaller */
    struct placement_ctx c = ctx_for(PM_FILL);
    mds_wrr_test_seed(99);
    uint32_t hits0 = 0;
    for (int i = 0; i < 100000; i++) {
        struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
        ASSERT_EQ(placement_admit(&c, ds, 2, &sc, 1, 0, &e, &why), MDS_OK);
        if (e.ds_id == 0) hits0++;
    }
    ASSERT_TRUE(hits0 > 48000 && hits0 < 52000);
}

static void test_alias_domain_total_equals_single_ds_domain(void)
{
    /* two DS on one domain vs one DS on another, equal fill: the two
     * domains receive the same number of files */
    struct mds_ds_info ds[3];
    reset();
    for (uint32_t i = 0; i < 3; i++) mk_ds(&ds[i], i, DS_ONLINE, "xi");
    add_row(0, "xi", 1000, 500, 7, 1000000);
    add_row(1, "xi", 1000, 500, 7, 1000000);
    add_row(2, "xi", 1000, 500, 8, 1000000);
    snprintf(DOM[0], PM_DOMAIN_ID_MAX, "d");
    snprintf(DOM[1], PM_DOMAIN_ID_MAX, "d");
    struct placement_ctx c = ctx_for(PM_FILL);
    mds_wrr_test_seed(17);
    uint32_t dom_d = 0;
    for (int i = 0; i < 100000; i++) {
        struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
        ASSERT_EQ(placement_admit(&c, ds, 3, &sc, 1, 0, &e, &why), MDS_OK);
        if (e.ds_id != 2) dom_d++;
    }
    ASSERT_TRUE(dom_d > 48000 && dom_d < 52000);
}

static void test_sizes_64_65_256(void)
{
    static struct mds_ds_info ds[256];
    uint32_t sizes[3] = {64, 65, 256};
    for (int s = 0; s < 3; s++) {
        reset();
        for (uint32_t i = 0; i < sizes[s]; i++) {
            char h[16];
            snprintf(h, sizeof(h), "h%u", i);
            mk_ds(&ds[i], i, DS_ONLINE, h);
            add_row(i, h, 1000, 500, 100 + i, 1000000);
        }
        struct placement_ctx c = ctx_for(PM_FILL);
        struct mds_ds_map_entry e[8]; uint32_t sc = 8; enum placement_reason why;
        ASSERT_EQ(placement_admit(&c, ds, sizes[s], &sc, 1, 0, e, &why), MDS_OK);
        ASSERT_EQ(sc, 8u);
        for (int i = 0; i < 8; i++) for (int j = i + 1; j < 8; j++) ASSERT_TRUE(e[i].ds_id != e[j].ds_id);
    }
}

static void test_ds_admitted_single(void)
{
    struct mds_ds_info ds[2];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "a");
    mk_ds(&ds[1], 1, DS_ONLINE, "b");
    add_row(0, "a", 1000, 800, 1, 1000000);
    add_row(1, "b", 1000, 0, 2, 1000000);
    struct placement_ctx c = ctx_for(PM_FILL);
    enum placement_reason why;
    ASSERT_EQ(placement_ds_admitted(&c, ds, 2, 0, &why), true);
    ASSERT_EQ(why, PR_NONE);
    ASSERT_EQ(placement_ds_admitted(&c, ds, 2, 1, &why), false);
    ASSERT_EQ(why, PR_CAPACITY_FULL);
    ASSERT_EQ(placement_ds_admitted(&c, ds, 2, 9, &why), false);
    ASSERT_EQ(why, PR_DS_OFFLINE);
    c.mode = PM_RR;
    ASSERT_EQ(placement_ds_admitted(&c, ds, 2, 1, &why), true);
    ds[1].state = DS_OFFLINE;
    ASSERT_EQ(placement_ds_admitted(&c, ds, 2, 1, &why), false);
    ASSERT_EQ(why, PR_DS_OFFLINE);
}

static void test_registry_view_drives_n_and_aliases_with_a_filtered_list(void)
{
    /* Scenario A (review finding 1): ds1 of domain d is OFFLINE and the
     * caller's list no longer contains it; N must still be 2. */
    struct mds_ds_info listed[2];
    reset();
    mk_ds(&listed[0], 0, DS_ONLINE, "xi");
    mk_ds(&listed[1], 2, DS_ONLINE, "other");
    add_row(0, "xi", 1000, 500, 7, 1000000);
    add_row(1, "xi", 1000, 500, 7, 1000000);
    V.rows[1].state = DS_OFFLINE;
    add_row(2, "other", 1000, 500, 8, 1000000);
    snprintf(DOM[0], PM_DOMAIN_ID_MAX, "d");
    snprintf(DOM[1], PM_DOMAIN_ID_MAX, "d");
    struct placement_ctx c = ctx_for(PM_FILL);
    struct placement_candidate out[2];
    ASSERT_EQ(placement_candidates(&c, listed, 2, out, NULL), 2u);
    ASSERT_TRUE(out[0].weight == placement_weight(50, 1000000, 2, NULL));
    ASSERT_TRUE(out[1].weight == placement_weight(50, 1000000, 1, NULL));
    /* Scenario B: the filtered-out sibling is an undeclared proven alias. */
    reset();
    add_row(0, "xi", 1000, 500, 7, 1000000);
    add_row(1, "xi", 1000, 500, 7, 1000000);
    struct placement_reject_counts why;
    ASSERT_EQ(placement_candidates(&c, listed, 1, out, &why), 0u);
    ASSERT_EQ(why.by_reason[PR_SHARED_FS_ALIAS_UNMAPPED], 1u);
    /* and the create boundary agrees with selection */
    enum placement_reason r;
    ASSERT_EQ(placement_ds_admitted(&c, listed, 1, 0, &r), false);
    ASSERT_EQ(r, PR_SHARED_FS_ALIAS_UNMAPPED);
}

static void test_ds_admitted_reason_uses_domain_level_verdict(void)
{
    /* ds0 (avail 50) and ds1 (avail 800) share a domain; the conservative
     * rule makes the domain full for min_free 100, ds1 included. */
    struct mds_ds_info ds[2];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "xi");
    mk_ds(&ds[1], 1, DS_ONLINE, "xi");
    add_row(0, "xi", 1000, 50, 7, 1000000);
    add_row(1, "xi", 1000, 800, 7, 1000000);
    snprintf(DOM[0], PM_DOMAIN_ID_MAX, "d");
    snprintf(DOM[1], PM_DOMAIN_ID_MAX, "d");
    struct placement_ctx c = ctx_for(PM_FILL);
    c.min_free_bytes = 100;
    struct placement_candidate out[2];
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, NULL), 0u);
    enum placement_reason r;
    ASSERT_EQ(placement_ds_admitted(&c, ds, 2, 1, &r), false);
    ASSERT_EQ(r, PR_CAPACITY_FULL);
    /* a total of 0 with a time stamp is UNKNOWN, not STALE */
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "h");
    add_row(0, "h", 0, 0, 1, 1000000);
    struct placement_reject_counts why;
    ASSERT_EQ(placement_candidates(&c, ds, 1, out, &why), 0u);
    ASSERT_EQ(why.by_reason[PR_CAPACITY_UNKNOWN], 1u);
}

static void test_admit_counts_per_ds_reasons(void)
{
    struct mds_ds_info ds[3];
    reset();
    for (uint32_t i = 0; i < 3; i++) mk_ds(&ds[i], i, DS_ONLINE, "h");
    add_row(0, "h", 1000, 500, 1, 1000000);
    add_row(1, "h", 1000, 0, 2, 1000000);           /* full */
    add_row(2, "h", 1000, 500, 3, 1000000 - 500000); /* stale */
    struct placement_ctx c = ctx_for(PM_FILL);
    uint64_t full_before = atomic_load(&g_branch_metrics.placement_rejections_total[PR_CAPACITY_FULL]);
    uint64_t stale_before = atomic_load(&g_branch_metrics.placement_rejections_total[PR_CAPACITY_STALE]);
    struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
    ASSERT_EQ(placement_admit(&c, ds, 3, &sc, 1, 0, &e, &why), MDS_OK);
    ASSERT_EQ(e.ds_id, 0u);
    ASSERT_EQ(atomic_load(&g_branch_metrics.placement_rejections_total[PR_CAPACITY_FULL]), full_before + 1);
    ASSERT_EQ(atomic_load(&g_branch_metrics.placement_rejections_total[PR_CAPACITY_STALE]), stale_before + 1);
}

/* -----------------------------------------------------------------------
 * smart (Stage B): assessment view, connector domains, manual weights
 * ----------------------------------------------------------------------- */

static struct placement_assessment_view A;
static char DWID[PM_MAX_DOMAINS][PM_DOMAIN_ID_MAX];
static uint32_t DW[PM_MAX_DOMAINS];

static void add_assess(uint32_t id, bool valid, bool allowed, uint32_t ppm, uint64_t ttl_ms,
                       const char *domain)
{
    struct placement_assessment_row *r = &A.rows[A.count++];
    memset(r, 0, sizeof(*r));
    r->ds_id = id;
    r->present = true;
    r->valid = valid;
    r->allowed = allowed;
    r->multiplier_ppm = ppm;
    r->received_mono_ms = 1000000 - 1000;
    r->expires_mono_ms = 1000000 - 1000 + ttl_ms;
    if (domain != NULL) snprintf(r->domain, PM_DOMAIN_ID_MAX, "%s", domain);
    A.batch_valid = true;
}

static struct placement_ctx smart_ctx(void)
{
    struct placement_ctx c = ctx_for(PM_SMART);
    c.assess = &A;
    return c;
}

static void reset_smart(void)
{
    reset();
    memset(&A, 0, sizeof(A));
    memset(DWID, 0, sizeof(DWID));
    memset(DW, 0, sizeof(DW));
}

static void test_smart_healthy_row_is_a_candidate(void)
{
    struct mds_ds_info ds[1];
    reset_smart();
    mk_ds(&ds[0], 0, DS_ONLINE, "h");
    add_row(0, "h", 1000, 500, 1, 1000000);
    add_assess(0, true, true, 1000000, 15000, NULL);
    struct placement_ctx c = smart_ctx();
    struct placement_candidate out[1];
    ASSERT_EQ(placement_candidates(&c, ds, 1, out, NULL), 1u);
    ASSERT_TRUE(out[0].weight == placement_weight(50, 1000000, 1, NULL));
    A.rows[0].multiplier_ppm = 250000;
    ASSERT_EQ(placement_candidates(&c, ds, 1, out, NULL), 1u);
    ASSERT_TRUE(out[0].weight == placement_weight(50, 250000, 1, NULL));
}

static void test_smart_reasons(void)
{
    struct mds_ds_info ds[5];
    reset_smart();
    for (uint32_t i = 0; i < 5; i++) { mk_ds(&ds[i], i, DS_ONLINE, "h"); add_row(i, "h", 1000, 500, 10 + i, 1000000); }
    /* ds0: no record; ds1: UNKNOWN; ds2: expired; ds3: denied; ds4: ppm 0 */
    add_assess(1, false, true, 1000000, 15000, NULL);
    add_assess(2, true, true, 1000000, 500, NULL);      /* received at -1000, ttl 500 -> expired */
    add_assess(3, true, false, 1000000, 15000, NULL);
    add_assess(4, true, true, 0, 15000, NULL);
    struct placement_ctx c = smart_ctx();
    struct placement_candidate out[5]; struct placement_reject_counts why;
    ASSERT_EQ(placement_candidates(&c, ds, 5, out, &why), 0u);
    ASSERT_EQ(why.by_reason[PR_NO_BINDING], 1u);
    ASSERT_EQ(why.by_reason[PR_ASSESSMENT_UNKNOWN], 1u);
    ASSERT_EQ(why.by_reason[PR_ASSESSMENT_STALE], 1u);
    ASSERT_EQ(why.by_reason[PR_CONNECTOR_DENIED], 1u);
    ASSERT_EQ(why.by_reason[PR_ZERO_MULTIPLIER], 1u);
    /* capacity comes first: a full domain is CAPACITY_FULL even with a fine assessment */
    reset_smart();
    mk_ds(&ds[0], 0, DS_ONLINE, "h");
    add_row(0, "h", 1000, 0, 1, 1000000);
    add_assess(0, true, true, 1000000, 15000, NULL);
    ASSERT_EQ(placement_candidates(&c, ds, 1, out, &why), 0u);
    ASSERT_EQ(why.by_reason[PR_CAPACITY_FULL], 1u);
    /* per-DS verdict agrees */
    enum placement_reason r;
    ASSERT_EQ(placement_ds_admitted(&c, ds, 1, 0, &r), false);
    ASSERT_EQ(r, PR_CAPACITY_FULL);
}

static void test_smart_without_view_is_not_ready(void)
{
    struct mds_ds_info ds[1];
    reset_smart();
    mk_ds(&ds[0], 0, DS_ONLINE, "h");
    add_row(0, "h", 1000, 500, 1, 1000000);
    struct placement_ctx c = smart_ctx();
    c.assess = NULL;
    struct placement_candidate out[1]; struct placement_reject_counts why;
    ASSERT_EQ(placement_candidates(&c, ds, 1, out, &why), 0u);
    ASSERT_EQ(why.by_reason[PR_MODE_NOT_READY], 1u);
}

static void test_smart_degraded_ppm_is_picked_less(void)
{
    struct mds_ds_info ds[2];
    reset_smart();
    mk_ds(&ds[0], 0, DS_ONLINE, "a"); mk_ds(&ds[1], 1, DS_ONLINE, "b");
    add_row(0, "a", 1000, 500, 1, 1000000); add_row(1, "b", 1000, 500, 2, 1000000);
    add_assess(0, true, true, 1000000, 15000, NULL);
    add_assess(1, true, true, 250000, 15000, NULL);
    struct placement_ctx c = smart_ctx();
    mds_wrr_test_seed(777);
    uint32_t hits1 = 0;
    for (int i = 0; i < 100000; i++) {
        struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
        ASSERT_EQ(placement_admit(&c, ds, 2, &sc, 1, 0, &e, &why), MDS_OK);
        if (e.ds_id == 1) hits1++;
    }
    ASSERT_TRUE(hits1 > 18000 && hits1 < 22000);   /* 1 : 4 */
}

static void test_smart_connector_domain_and_map_mismatch(void)
{
    struct mds_ds_info ds[3];
    reset_smart();
    for (uint32_t i = 0; i < 3; i++) { mk_ds(&ds[i], i, DS_ONLINE, "xi"); add_row(i, "xi", 1000, 800, 7 + (i == 2), 1000000); }
    /* the connector says ds0 and ds1 share filesystem fs-1 */
    add_assess(0, true, true, 1000000, 15000, "ctrl/fs-1");
    add_assess(1, true, true, 1000000, 15000, "ctrl/fs-1");
    add_assess(2, true, true, 1000000, 15000, "ctrl/fs-2");
    struct placement_ctx c = smart_ctx();
    struct placement_candidate out[3]; struct placement_reject_counts why;
    ASSERT_EQ(placement_candidates(&c, ds, 3, out, &why), 3u);
    ASSERT_TRUE(out[0].weight == placement_weight(80, 1000000, 2, NULL));
    ASSERT_TRUE(out[2].weight == placement_weight(80, 1000000, 1, NULL));
    ASSERT_EQ(strcmp(out[0].domain, "ctrl/fs-1"), 0);
    /* an operator map that agrees is fine; one that disagrees excludes the DS */
    snprintf(DOM[0], PM_DOMAIN_ID_MAX, "ctrl/fs-1");
    snprintf(DOM[1], PM_DOMAIN_ID_MAX, "ctrl/fs-1");
    ASSERT_EQ(placement_candidates(&c, ds, 3, out, &why), 3u);
    snprintf(DOM[1], PM_DOMAIN_ID_MAX, "somewhere-else");
    ASSERT_EQ(placement_candidates(&c, ds, 3, out, &why), 2u);
    ASSERT_EQ(why.by_reason[PR_DOMAIN_MAP_MISMATCH], 1u);
    ASSERT_TRUE(out[0].weight == placement_weight(80, 1000000, 2, NULL));   /* N still counts the alias */
}

static void test_smart_alias_with_an_unbound_sibling(void)
{
    struct mds_ds_info ds[2];
    reset_smart();
    /* ds0 and ds1 export the same filesystem from one host; only ds0 has a
     * connector record (domain ctrl/fs-1), ds1 is not bound yet */
    mk_ds(&ds[0], 0, DS_ONLINE, "xi");
    mk_ds(&ds[1], 1, DS_ONLINE, "xi");
    add_row(0, "xi", 1000, 800, 7, 1000000);
    add_row(1, "xi", 1000, 800, 7, 1000000);
    add_assess(0, true, true, 1000000, 15000, "ctrl/fs-1");
    struct placement_ctx c = smart_ctx();
    struct placement_candidate out[2]; struct placement_reject_counts why;
    /* the declared side stays a candidate; the undeclared alias is excluded */
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, &why), 1u);
    ASSERT_EQ(out[0].ds_id, 0u);
    ASSERT_EQ(why.by_reason[PR_SHARED_FS_ALIAS_UNMAPPED], 1u);
    ASSERT_TRUE(out[0].weight == placement_weight(80, 1000000, 1, NULL));   /* N = 1: the alias is not a member */
    /* an operator map for the sibling that agrees makes both members */
    snprintf(DOM[1], PM_DOMAIN_ID_MAX, "ctrl/fs-1");
    add_assess(1, true, true, 1000000, 15000, "ctrl/fs-1");
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, &why), 2u);
    ASSERT_TRUE(out[0].weight == placement_weight(80, 1000000, 2, NULL));
    /* two DIFFERENT declared domains on one filesystem contradict the map */
    A.count = 1;
    snprintf(DOM[1], PM_DOMAIN_ID_MAX, "ctrl/fs-9");
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, &why), 0u);
    ASSERT_EQ(why.by_reason[PR_DOMAIN_MAP_CONTRADICTION], 2u);
}

static void test_smart_manual_base_weight(void)
{
    struct mds_ds_info ds[2];
    reset_smart();
    mk_ds(&ds[0], 0, DS_ONLINE, "a"); mk_ds(&ds[1], 1, DS_ONLINE, "b");
    add_row(0, "a", 1000, 500, 1, 1000000); add_row(1, "b", 1000, 500, 2, 1000000);
    add_assess(0, true, true, 1000000, 15000, "d-a");
    add_assess(1, true, true, 1000000, 15000, "d-b");
    snprintf(DWID[0], PM_DOMAIN_ID_MAX, "d-a"); DW[0] = 300;
    struct placement_ctx c = smart_ctx();
    struct placement_candidate out[2];
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, NULL), 2u);
    ASSERT_TRUE(out[0].weight == placement_weight(50, 1000000, 1, NULL));   /* no manual weights in the ctx */
    c.domain_weight_id = (const char (*)[PM_DOMAIN_ID_MAX])DWID;
    c.domain_weight = DW;
    c.domain_weight_count = 1;
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, NULL), 2u);
    ASSERT_TRUE(out[0].weight == placement_weight(300, 1000000, 1, NULL));
    ASSERT_TRUE(out[1].weight == placement_weight(50, 1000000, 1, NULL));
    /* fill ignores manual weights (config forbids them anyway) */
    c.mode = PM_FILL;
    ASSERT_EQ(placement_candidates(&c, ds, 2, out, NULL), 2u);
    ASSERT_TRUE(out[0].weight == placement_weight(50, 1000000, 1, NULL));
}

static void test_reason_names_are_bounded(void)
{
    for (int r = 0; r < PR_COUNT; r++) {
        ASSERT_TRUE(strcmp(placement_reason_name((enum placement_reason)r), "UNKNOWN_REASON") != 0);
    }
    ASSERT_EQ(strcmp(placement_reason_name((enum placement_reason)PR_COUNT), "UNKNOWN_REASON"), 0);
    ASSERT_EQ(strcmp(placement_reason_name(PR_CAPACITY_FULL), "CAPACITY_FULL"), 0);
}

static void test_admit_argument_checks(void)
{
    struct mds_ds_info ds[1];
    reset();
    mk_ds(&ds[0], 0, DS_ONLINE, "a");
    struct placement_ctx c = ctx_for(PM_RR);
    struct mds_ds_map_entry e; uint32_t sc = 1; uint32_t zero = 0; enum placement_reason why;
    ASSERT_EQ(placement_admit(NULL, ds, 1, &sc, 1, 0, &e, &why), MDS_ERR_INVAL);
    ASSERT_EQ(placement_admit(&c, ds, 1, &zero, 1, 0, &e, &why), MDS_ERR_INVAL);
    ASSERT_EQ(placement_admit(&c, ds, 1, &sc, 0, 0, &e, &why), MDS_ERR_INVAL);
    ASSERT_EQ(placement_admit(&c, ds, 0, &sc, 1, 0, &e, &why), MDS_ERR_INVAL);
}

/* -----------------------------------------------------------------------
 * Singleton (Task 5): published capacity view, init rules, site helper
 * ----------------------------------------------------------------------- */

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

static void add_cache_ds(struct mds_catalogue *cat, struct ds_cache *c, uint32_t ds_id, const char *host)
{
    struct mds_cat_txn *txn = NULL;
    struct mds_ds_info info;
    memset(&info, 0, sizeof(info));
    info.ds_id = ds_id;
    info.state = DS_ONLINE;
    info.port = 2049;
    snprintf(info.host, sizeof(info.host), "%s", host);
    (void)mds_cat_txn_begin(cat, MDS_CAT_TXN_WRITE, &txn);
    (void)mds_cat_ds_put(cat, txn, &info);
    (void)mds_cat_txn_commit(txn);
    (void)ds_cache_invalidate(c, cat);
}

static struct mds_config fill_cfg(void)
{
    struct mds_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.placement_mode = PM_FILL;
    cfg.placement_mode_set = true;
    cfg.placement_capacity_max_age_ms = 120000;
    cfg.placement_stripe_shrink = PM_SHRINK_ALLOW;
    snprintf(cfg.placement_config_generation, sizeof(cfg.placement_config_generation), "%064x", 1);
    return cfg;
}

static void test_singleton_legacy_when_not_initialised(void)
{
    struct placement_ctx c;
    ASSERT_EQ(placement_gate_mode(), PM_LEGACY);
    ASSERT_EQ(strcmp(placement_gate_generation(), ""), 0);
    placement_gate_ctx(&c, 5);
    ASSERT_EQ(c.mode, PM_LEGACY);
    ASSERT_TRUE(c.cap == NULL);
    placement_gate_ctx_release(&c);
    placement_gate_publish_capacity();   /* no-op, no crash */
    placement_gate_destroy();            /* no-op */
}

static void test_singleton_init_requires_mode_set(void)
{
    struct mds_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(placement_gate_init(&cfg, NULL), -1);
    ASSERT_EQ(placement_gate_init(NULL, NULL), -1);
    ASSERT_EQ(placement_gate_mode(), PM_LEGACY);
}

static void test_singleton_fill_needs_cache_and_kernel(void)
{
    struct mds_config cfg = fill_cfg();
    ASSERT_EQ(placement_gate_init(&cfg, NULL), -1);   /* fill without a DS cache */
    struct mds_catalogue *cat = NULL;
    struct ds_cache *cache = cache_with_ds(&cat, 0);
    ASSERT_TRUE(cache != NULL);
    int rc = placement_gate_init(&cfg, cache);
    ASSERT_EQ(rc, mds_wrr_kernel_id() == 0 ? -1 : 0);
    placement_gate_destroy();
    ASSERT_EQ(placement_gate_mode(), PM_LEGACY);
    ds_cache_destroy(cache);
    mds_catalogue_close(cat);
}

static void test_singleton_rr_needs_no_cache(void)
{
    struct mds_config cfg = fill_cfg();
    cfg.placement_mode = PM_RR;
    ASSERT_EQ(placement_gate_init(&cfg, NULL), 0);
    ASSERT_EQ(placement_gate_mode(), PM_RR);
    ASSERT_EQ(strlen(placement_gate_generation()), 64u);
    struct placement_ctx c;
    placement_gate_ctx(&c, 1);
    ASSERT_EQ(c.mode, PM_RR);
    ASSERT_TRUE(c.cap == NULL);
    ASSERT_TRUE(c.rr_counter != NULL);
    placement_gate_ctx_release(&c);
    placement_gate_destroy();
}

static void test_singleton_publishes_capacity_from_cache(void)
{
    struct mds_config cfg = fill_cfg();
    struct mds_catalogue *cat = NULL;
    struct ds_cache *cache = cache_with_ds(&cat, 0);
    ASSERT_TRUE(cache != NULL);
    ASSERT_EQ(placement_gate_init(&cfg, cache), 0);
    ASSERT_EQ(placement_gate_mode(), PM_FILL);
    struct placement_ctx c;
    placement_gate_ctx(&c, ds_cache_mono_ms());
    ASSERT_TRUE(c.cap != NULL);
    ASSERT_EQ(c.cap->count, 1u);
    ASSERT_EQ(c.cap->rows[0].obs.observed_mono_ms, 0u);
    ASSERT_EQ(c.capacity_max_age_ms, 120000u);
    placement_gate_ctx_release(&c);
    ASSERT_TRUE(c.cap == NULL);
    ASSERT_EQ(ds_capacity_probe_once(cache, "/tmp", CAP_WEIGHT_OFF), 1);   /* probe_once publishes */
    placement_gate_ctx(&c, ds_cache_mono_ms());
    ASSERT_TRUE(c.cap->rows[0].obs.observed_mono_ms != 0);
    /* a ctx keeps its snapshot alive across a republish */
    struct ds_capacity_obs o = { 1000, 10, 1, ds_cache_mono_ms(), 0 };
    ASSERT_EQ(ds_cache_set_capacity_obs(cache, 0, &o), 0);
    placement_gate_publish_capacity();
    ASSERT_TRUE(c.cap->rows[0].obs.total_bytes != 1000);
    placement_gate_ctx_release(&c);
    placement_gate_ctx(&c, ds_cache_mono_ms());
    ASSERT_EQ(c.cap->rows[0].obs.total_bytes, 1000u);
    placement_gate_ctx_release(&c);
    placement_gate_destroy();
    ds_cache_destroy(cache);
    mds_catalogue_close(cat);
}

static void test_singleton_smart_init_and_readiness(void)
{
    struct mds_config cfg = fill_cfg();
    cfg.placement_mode = PM_SMART;
    cfg.ds_connector_poll_ms = 1000;
    ASSERT_EQ(placement_gate_init(&cfg, NULL), -1);          /* smart needs the cache */
    struct mds_catalogue *cat = NULL;
    struct ds_cache *cache = cache_with_ds(&cat, 0);
    ASSERT_TRUE(cache != NULL);
    ASSERT_EQ(placement_gate_init(&cfg, cache), 0);
    ASSERT_EQ(placement_gate_mode(), PM_SMART);
    struct placement_readiness r;
    placement_gate_readiness(&r);
    ASSERT_EQ(r.mode_active, true);
    ASSERT_EQ(r.connector_config_valid, false);
    ASSERT_EQ(r.connector_reachable, false);
    ASSERT_EQ(r.registered_ds, 1u);
    ASSERT_EQ(r.covered_ds, 0u);
    ASSERT_EQ(strcmp(r.coverage, "none"), 0);
    /* no assessments published: every candidate check is NOT_READY */
    struct mds_ds_info ds[1]; mk_ds(&ds[0], 0, DS_ONLINE, "ds-host");
    struct ds_capacity_obs half = { 1000, 500, 1, ds_cache_mono_ms(), 0 };
    ASSERT_EQ(ds_cache_set_capacity_obs(cache, 0, &half), 0);
    placement_gate_publish_capacity();
    struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
    ASSERT_EQ(placement_select_gated(true, PLACEMENT_WEIGHTED_RR, ds, 1, &sc, 1, 65536, 0, &e, &why), MDS_ERR_NOSPC);
    ASSERT_EQ(why, PR_MODE_NOT_READY);
    /* a published view with a fresh VALID row makes the DS eligible */
    struct placement_assessment_view v; memset(&v, 0, sizeof(v));
    v.count = 1; v.batch_valid = true;
    v.rows[0].ds_id = 0; v.rows[0].present = true; v.rows[0].valid = true; v.rows[0].allowed = true;
    v.rows[0].multiplier_ppm = 1000000; v.rows[0].received_mono_ms = ds_cache_mono_ms();
    v.rows[0].expires_mono_ms = ds_cache_mono_ms() + 15000;
    placement_gate_publish_assessments(&v);
    placement_gate_readiness(&r);
    ASSERT_EQ(r.covered_ds, 1u);
    ASSERT_EQ(r.eligible_ds, 1u);
    ASSERT_EQ(strcmp(r.coverage, "full"), 0);
    ASSERT_EQ(placement_select_gated(true, PLACEMENT_WEIGHTED_RR, ds, 1, &sc, 1, 65536, 0, &e, &why), MDS_OK);
    ASSERT_EQ(e.ds_id, 0u);
    struct placement_ds_status st;
    ASSERT_EQ(placement_gate_ds_status(0, &st), true);
    ASSERT_EQ(st.assessed, true);
    ASSERT_EQ(st.assessment_valid, true);
    ASSERT_EQ(st.assessment_ppm, 1000000u);
    ASSERT_TRUE(st.assessment_ttl_ms > 10000);
    placement_gate_destroy();
    ds_cache_destroy(cache);
    mds_catalogue_close(cat);
}

static void test_admit_histogram_counts_every_selection(void)
{
    uint64_t c0 = atomic_load(&g_branch_metrics.placement_admit_hist.count);
    struct mds_ds_info ds[1]; mk_ds(&ds[0], 0, DS_ONLINE, "ds-host");
    struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
    /* legacy (gate not initialised) is observed too: the perf row compares
     * like with like */
    ASSERT_EQ(placement_gate_mode(), PM_LEGACY);
    ASSERT_EQ(placement_select_gated(false, PLACEMENT_RR, ds, 1, &sc, 1, 65536, 0, &e, &why), MDS_OK);
    ASSERT_TRUE(atomic_load(&g_branch_metrics.placement_admit_hist.count) == c0 + 1);
    struct mds_config cfg = fill_cfg();
    struct mds_catalogue *cat = NULL;
    struct ds_cache *cache = cache_with_ds(&cat, 0);
    ASSERT_TRUE(cache != NULL);
    ASSERT_EQ(placement_gate_init(&cfg, cache), 0);
    struct ds_capacity_obs half = { 1000, 500, 1, ds_cache_mono_ms(), 0 };
    ASSERT_EQ(ds_cache_set_capacity_obs(cache, 0, &half), 0);
    placement_gate_publish_capacity();
    sc = 1;
    ASSERT_EQ(placement_select_gated(true, PLACEMENT_WEIGHTED_RR, ds, 1, &sc, 1, 65536, 0, &e, &why), MDS_OK);
    struct placement_token tok;
    ASSERT_EQ(placement_gate_admit_create(0, PP_NEW_OBJECT, &tok, &why), MDS_OK);
    ASSERT_TRUE(atomic_load(&g_branch_metrics.placement_admit_hist.count) == c0 + 3);
    placement_gate_destroy();
    ds_cache_destroy(cache);
    mds_catalogue_close(cat);
}

static void test_select_gated_legacy_path(void)
{
    struct mds_ds_info ds[2];
    mk_ds(&ds[0], 0, DS_ONLINE, "a");
    mk_ds(&ds[1], 1, DS_OFFLINE, "b");
    ASSERT_EQ(placement_gate_mode(), PM_LEGACY);
    struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why = PR_CAPACITY_FULL;
    ASSERT_EQ(placement_select_gated(false, PLACEMENT_RR, ds, 2, &sc, 1, 65536, 0, &e, &why), MDS_OK);
    ASSERT_EQ(e.ds_id, 0u);
    ASSERT_EQ(why, PR_NONE);
    ASSERT_EQ(placement_select_gated(true, PLACEMENT_WEIGHTED_RR, ds, 2, &sc, 1, 65536, 0, &e, &why), MDS_OK);
    ASSERT_EQ(e.ds_id, 0u);
    sc = 1;
    ASSERT_EQ(placement_select_gated(false, PLACEMENT_RR, ds, 2, &sc, 1, 65536, 12345, &e, &why), MDS_OK);
    ASSERT_EQ(e.ds_id, 0u);
}

static void test_select_gated_legacy_rr_at2_branch(void)
{
    struct mds_ds_info ds[2];
    mk_ds(&ds[0], 10, DS_ONLINE, "a");
    mk_ds(&ds[1], 11, DS_ONLINE, "b");
    ASSERT_EQ(placement_gate_mode(), PM_LEGACY);
    struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
    ASSERT_EQ(placement_select_gated(false, PLACEMENT_RR, ds, 2, &sc, 1, 65536, 1, &e, &why), MDS_OK);
    ASSERT_EQ(e.ds_id, 11u);   /* start = 1 % 2 */
    ASSERT_EQ(placement_select_gated(false, PLACEMENT_RR, ds, 2, &sc, 1, 65536, 2, &e, &why), MDS_OK);
    ASSERT_EQ(e.ds_id, 10u);   /* start = 2 % 2 */
}

static void test_select_gated_fill_path_uses_the_gate(void)
{
    struct mds_config cfg = fill_cfg();
    struct mds_catalogue *cat = NULL;
    struct ds_cache *cache = cache_with_ds(&cat, 0);
    ASSERT_TRUE(cache != NULL);
    add_cache_ds(cat, cache, 1, "other");
    ASSERT_EQ(placement_gate_init(&cfg, cache), 0);
    struct ds_capacity_obs full = { 1000, 0, 1, ds_cache_mono_ms(), 0 };
    struct ds_capacity_obs half = { 1000, 500, 2, ds_cache_mono_ms(), 0 };
    ASSERT_EQ(ds_cache_set_capacity_obs(cache, 0, &full), 0);
    ASSERT_EQ(ds_cache_set_capacity_obs(cache, 1, &half), 0);
    placement_gate_publish_capacity();
    struct mds_ds_info ds[2];
    mk_ds(&ds[0], 0, DS_ONLINE, "ds-host");
    mk_ds(&ds[1], 1, DS_ONLINE, "other");
    for (int i = 0; i < 50; i++) {
        struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
        ASSERT_EQ(placement_select_gated(true, PLACEMENT_WEIGHTED_RR, ds, 2, &sc, 1, 65536, 0, &e, &why), MDS_OK);
        ASSERT_EQ(e.ds_id, 1u);
    }
    uint64_t before = atomic_load(&g_branch_metrics.placement_rejections_total[PR_NO_ELIGIBLE_DS]);
    ASSERT_EQ(ds_cache_set_capacity_obs(cache, 1, &full), 0);
    placement_gate_publish_capacity();
    struct mds_ds_map_entry e; uint32_t sc = 1; enum placement_reason why;
    ASSERT_EQ(placement_select_gated(true, PLACEMENT_WEIGHTED_RR, ds, 2, &sc, 1, 65536, 0, &e, &why), MDS_ERR_NOSPC);
    ASSERT_EQ(why, PR_NO_ELIGIBLE_DS);
    ASSERT_EQ(atomic_load(&g_branch_metrics.placement_rejections_total[PR_NO_ELIGIBLE_DS]), before + 1);
    ASSERT_EQ(atomic_load(&g_branch_metrics.placement_mode_gauge), (uint64_t)PM_FILL);
    placement_gate_destroy();
    ASSERT_EQ(atomic_load(&g_branch_metrics.placement_mode_gauge), 0u);
    ds_cache_destroy(cache);
    mds_catalogue_close(cat);
}

static void test_prealloc_stub_pop_is_gated(void)
{
    struct mds_config cfg = fill_cfg();
    struct mds_catalogue *cat = NULL;
    struct ds_cache *cache = cache_with_ds(&cat, 0);
    ASSERT_TRUE(cache != NULL);
    add_cache_ds(cat, cache, 1, "other");
    ASSERT_EQ(placement_gate_init(&cfg, cache), 0);
    struct ds_capacity_obs full = { 1000, 0, 1, ds_cache_mono_ms(), 0 };
    struct ds_capacity_obs half = { 1000, 500, 2, ds_cache_mono_ms(), 0 };
    ASSERT_EQ(ds_cache_set_capacity_obs(cache, 0, &full), 0);
    ASSERT_EQ(ds_cache_set_capacity_obs(cache, 1, &half), 0);
    placement_gate_publish_capacity();
    struct ds_prealloc_ctx *pa = NULL;
    ASSERT_EQ(ds_prealloc_init_ex(cat, NULL, PLACEMENT_WEIGHTED_RR, 1, &pa), 0);
    ASSERT_TRUE(pa != NULL);
    ds_prealloc_set_ds_cache(pa, cache);
    for (int i = 0; i < 50; i++) {
        struct mds_ds_map_entry e; uint32_t su = 0; uint64_t fid = 0;
        ASSERT_EQ(ds_prealloc_pop(pa, &e, &su, &fid), 0);
        ASSERT_EQ(e.ds_id, 1u);
        ASSERT_TRUE(fid != 0);
    }
    ASSERT_EQ(ds_cache_set_capacity_obs(cache, 1, &full), 0);
    placement_gate_publish_capacity();
    {
        struct mds_ds_map_entry e; uint32_t su = 0; uint64_t fid = 0;
        ASSERT_EQ(ds_prealloc_pop(pa, &e, &su, &fid), -1);
        ASSERT_EQ(ds_prealloc_peek(pa, &e, &su), -1);
        ASSERT_EQ(ds_prealloc_select_any_online(pa, &e, &su), MDS_ERR_NOSPC);
    }
    ds_prealloc_destroy(pa);
    placement_gate_destroy();
    ds_cache_destroy(cache);
    mds_catalogue_close(cat);
}

int main(void)
{
    printf("test_placement_gate\n");
    RUN_TEST(test_prealloc_stub_pop_is_gated);
    RUN_TEST(test_singleton_legacy_when_not_initialised);
    RUN_TEST(test_singleton_init_requires_mode_set);
    RUN_TEST(test_singleton_fill_needs_cache_and_kernel);
    RUN_TEST(test_singleton_rr_needs_no_cache);
    RUN_TEST(test_singleton_publishes_capacity_from_cache);
    RUN_TEST(test_singleton_smart_init_and_readiness);
    RUN_TEST(test_select_gated_legacy_path);
    RUN_TEST(test_admit_histogram_counts_every_selection);
    RUN_TEST(test_select_gated_legacy_rr_at2_branch);
    RUN_TEST(test_registry_view_drives_n_and_aliases_with_a_filtered_list);
    RUN_TEST(test_ds_admitted_reason_uses_domain_level_verdict);
    RUN_TEST(test_admit_counts_per_ds_reasons);
    RUN_TEST(test_smart_healthy_row_is_a_candidate);
    RUN_TEST(test_smart_reasons);
    RUN_TEST(test_smart_without_view_is_not_ready);
    RUN_TEST(test_smart_degraded_ppm_is_picked_less);
    RUN_TEST(test_smart_connector_domain_and_map_mismatch);
    RUN_TEST(test_smart_manual_base_weight);
    RUN_TEST(test_smart_alias_with_an_unbound_sibling);
    RUN_TEST(test_select_gated_fill_path_uses_the_gate);
    RUN_TEST(test_rr_is_cyclic_over_the_gated_list);
    RUN_TEST(test_rr_ignores_capacity);
    RUN_TEST(test_rr_mirrors_are_distinct_and_shrink);
    RUN_TEST(test_fill_excludes_unknown_stale_and_full);
    RUN_TEST(test_fill_ds_missing_from_view_is_unknown);
    RUN_TEST(test_fill_min_free_gate);
    RUN_TEST(test_fill_freshness_boundary);
    RUN_TEST(test_fill_weight_is_fill_level_over_aliases);
    RUN_TEST(test_alias_share_counts_offline_members);
    RUN_TEST(test_domain_canonical_observation_is_lowest_id_and_conservative);
    RUN_TEST(test_alias_grades);
    RUN_TEST(test_weight_bounds);
    RUN_TEST(test_fill_multi_stripe_distinct_and_shrink_vs_strict);
    RUN_TEST(test_fill_mirrors_are_distinct);
    RUN_TEST(test_no_candidate_is_nospc_never_ds0);
    RUN_TEST(test_smart_without_assessments_is_not_ready);
    RUN_TEST(test_fill_fairness_4_to_1);
    RUN_TEST(test_fill_equal_fill_is_even);
    RUN_TEST(test_alias_domain_total_equals_single_ds_domain);
    RUN_TEST(test_sizes_64_65_256);
    RUN_TEST(test_ds_admitted_single);
    RUN_TEST(test_reason_names_are_bounded);
    RUN_TEST(test_admit_argument_checks);
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
