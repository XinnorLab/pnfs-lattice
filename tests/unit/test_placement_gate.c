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

int main(void)
{
    printf("test_placement_gate\n");
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
