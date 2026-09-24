/*
 * SPDX-License-Identifier: MIT
 *
 * test_ds_connector.c -- the connector batch parser: envelope, sequence
 * lines, binding pins, per-record shape (design section 7, LAT-06).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pnfs_mds.h"
#include "ds_connector.h"
#include "mds_metrics.h"
#include <stdatomic.h>

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

static struct ds_connector_registry REG;
static struct ds_connector_state ST;
static char BUF[65536];

static void reg_init(void)
{
    memset(&REG, 0, sizeof(REG));
    REG.count = 2;
    REG.ds[0].ds_id = 0;
    snprintf(REG.ds[0].host, sizeof(REG.ds[0].host), "192.168.64.51");
    snprintf(REG.ds[0].export_path, sizeof(REG.ds[0].export_path), "/mnt/data/pnfs-ds");
    REG.ds[0].tcp_port = 2049;
    REG.ds[1].ds_id = 1;
    snprintf(REG.ds[1].host, sizeof(REG.ds[1].host), "192.168.64.71");
    snprintf(REG.ds[1].export_path, sizeof(REG.ds[1].export_path), "/mnt/data/pnfs-ds");
    REG.ds[1].tcp_port = 2049;
}

static void st_init(const char *profile_pin, const char *config_pin)
{
    struct ds_connector_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.contract_major = 1;
    cfg.max_ds = 256;
    snprintf(cfg.access_scope, sizeof(cfg.access_scope), "cluster-default");
    if (profile_pin) snprintf(cfg.expected_profile_digest, sizeof(cfg.expected_profile_digest), "%s", profile_pin);
    if (config_pin) snprintf(cfg.expected_config_digest, sizeof(cfg.expected_config_digest), "%s", config_pin);
    ds_connector_state_init(&ST, &cfg);
}

/* One assessment record with overridable fields. */
static const char *rec(uint32_t ds, unsigned gen, const char *target, const char *inc,
                       const char *server, const char *path, unsigned port,
                       const char *scope, const char *access, const char *quality,
                       unsigned ttl, const char *profile, const char *allowed,
                       unsigned ppm, const char *domain_json)
{
    static char r[4][4096];
    static int slot;
    char *b = r[slot++ & 3];
    snprintf(b, 4096,
        "{\"ds_id\":%u,\"binding_generation\":%u,\"datastore_id\":\"ctrl-1\","
        "\"target_id\":\"%s\",\"target_incarnation\":%s,"
        "\"endpoint\":{\"server\":\"%s\",\"export_path\":\"%s\",\"protocol\":\"NFS\",\"transport\":\"TCP\",\"port\":%u},"
        "\"scope\":\"%s\",\"access_scope_id\":\"%s\",\"quality\":\"%s\","
        "\"observed_at\":\"2026-09-24T10:00:00Z\",\"evidence_age_ms\":1000,\"remaining_ttl_ms\":%u,"
        "\"profile\":{\"id\":\"xinas-mvp\",\"version\":\"1\",\"digest\":\"%s\"},"
        "\"placement\":{\"allowed\":%s,\"multiplier_ppm\":%u,\"reason_codes\":[\"NORMAL\",\"X\"]},"
        "\"resources\":{\"capacity_domain_id\":%s,\"shared_resource_ids\":[]},\"coverage\":[]}",
        ds, gen, target, inc, server, path, port, scope, access, quality, ttl, profile, allowed, ppm, domain_json);
    return b;
}

static const char *rec_ok(uint32_t ds)
{
    return rec(ds, 2, "mnt/data", "\"mnt/data:0:u\"", ds == 0 ? "192.168.64.51" : "192.168.64.71",
               "/mnt/data", 2049, "NEW_ALLOCATION", "cluster-default", "VALID", 15000,
               "sha256:p", "true", 1000000, "\"ctrl-1/fs-1/inc\"");
}

static const char *batch(const char *runtime_epoch, const char *inst_epoch, unsigned seq,
                         const char *generated_at, const char *config_digest,
                         const char *snapshot, const char *records)
{
    snprintf(BUF, sizeof(BUF),
        "{\"contract_version\":\"1.0\",\"runtime_epoch\":\"%s\",\"config_digest\":\"%s\","
        "\"generated_at\":\"%s\",\"instances\":[{\"connector_instance_id\":\"xi-01\","
        "\"module_type\":\"xinas\",\"epoch\":\"%s\",\"sequence\":%u,\"snapshot_status\":\"%s\","
        "\"assessments\":[%s]}]}",
        runtime_epoch, config_digest, generated_at, inst_epoch, seq, snapshot, records);
    return BUF;
}

static enum ds_connector_drop apply(const char *text, struct placement_assessment_view *v,
                                    struct ds_connector_report *rep)
{
    return ds_connector_apply_batch(&ST, text, strlen(text), &REG, 1000000, v, rep);
}

/* -----------------------------------------------------------------------
 * Tests
 * ----------------------------------------------------------------------- */

static void test_healthy_batch_is_accepted(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "cfg-d", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    ASSERT_EQ(v.batch_valid, true);
    ASSERT_EQ(v.count, 2u);
    ASSERT_EQ(v.rows[0].ds_id, 0u);
    ASSERT_EQ(v.rows[0].present, true);
    ASSERT_EQ(v.rows[0].valid, true);
    ASSERT_EQ(v.rows[0].allowed, true);
    ASSERT_EQ(v.rows[0].multiplier_ppm, 1000000u);
    ASSERT_EQ(v.rows[0].expires_mono_ms, 1015000u);
    ASSERT_EQ(v.rows[0].received_mono_ms, 1000000u);
    ASSERT_EQ(strcmp(v.rows[0].domain, "ctrl-1/fs-1/inc"), 0);
    ASSERT_EQ(v.rows[0].reason_count, 2u);
    ASSERT_EQ(strcmp(v.rows[0].reasons[0], "NORMAL"), 0);
    ASSERT_EQ(v.rows[1].present, false);            /* ds 1 has no record */
    ASSERT_EQ(strcmp(v.config_digest, "cfg-d"), 0);
    ASSERT_EQ(strcmp(v.profile_digest, "sha256:p"), 0);
    ASSERT_EQ(ST.pins[0].pinned, true);
    ASSERT_EQ(ST.pins[0].binding_generation, 2u);
    ASSERT_EQ(strcmp(ST.runtime_epoch, "rt-1"), 0);
    ASSERT_TRUE(ST.last_generated_at_ms > 0);
}

static void test_contract_major_mismatch(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    const char *b = batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "cfg", "COMPLETE", rec_ok(0));
    char *m = strdup(b); char *p = strstr(m, "\"1.0\""); p[1] = '2';
    ASSERT_EQ(apply(m, &v, &rep), DC_CONTRACT_MAJOR);
    ASSERT_EQ(v.batch_valid, false);
    ASSERT_EQ(ST.pins[0].pinned, false);           /* state untouched */
    free(m);
}

static void test_replay_drops_the_batch_and_keeps_state(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    ASSERT_EQ(apply(batch("rt-1", "e1", 5, "2026-09-24T10:00:05Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);
    /* the connector re-serves the same snapshot until its next collection:
     * an equal sequence is steady state, not a replay */
    ASSERT_EQ(apply(batch("rt-1", "e1", 5, "2026-09-24T10:00:06Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);
    ASSERT_EQ(v.batch_valid, true);
    ASSERT_EQ(apply(batch("rt-1", "e1", 4, "2026-09-24T10:00:07Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_REPLAY);
    ASSERT_EQ(v.batch_valid, false);
    ASSERT_EQ(ST.inst[0].sequence, 5u);
    ASSERT_EQ(apply(batch("rt-1", "e1", 6, "2026-09-24T10:00:08Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);
    /* a new instance epoch starts a fresh line */
    ASSERT_EQ(apply(batch("rt-1", "e2", 1, "2026-09-24T10:00:09Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);
    ASSERT_EQ(ST.inst[0].sequence, 1u);
}

static void test_runtime_epoch_change_resets_pins(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);
    /* same generation, different target: a mismatch while rt-1 lives */
    const char *changed = rec(0, 2, "other-share", "\"o:1\"", "192.168.64.51", "/mnt/data", 2049,
                              "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 2, "2026-09-24T10:00:02Z", "c", "COMPLETE", changed), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rejected_binding, 1u);
    ASSERT_EQ(v.rows[0].present, false);
    ASSERT_TRUE(strstr(rep.detail, "BINDING_MISMATCH") != NULL);
    /* after a connector restart (new runtime_epoch) the tuple is re-pinned; an
     * older generated_at is fine across the epoch boundary */
    ASSERT_EQ(apply(batch("rt-2", "e1", 1, "2026-09-24T09:00:00Z", "c", "COMPLETE", changed), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    ASSERT_EQ(strcmp(ST.pins[0].target_id, "other-share"), 0);
    ASSERT_EQ(v.rows[0].domain[0], '\0');          /* null domain */
}

static void test_old_generated_at_is_dropped(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:05Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);
    ASSERT_EQ(apply(batch("rt-1", "e1", 2, "2026-09-24T10:00:04.999Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_OLD_GENERATED_AT);
    ASSERT_EQ(apply(batch("rt-1", "e1", 2, "2026-09-24T10:00:05Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);   /* equal is fine */
}

static void test_config_digest_pin(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, "cfg-expected");
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "cfg-other", "COMPLETE", rec_ok(0)), &v, &rep), DC_CONFIG_DIGEST);
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "cfg-expected", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);
}

static void test_profile_digest_pin_and_consistency(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init("sha256:p", NULL);
    const char *other = rec(1, 1, "s", "\"i\"", "192.168.64.71", "/mnt/data", 2049, "NEW_ALLOCATION",
                            "cluster-default", "VALID", 15000, "sha256:q", "true", 1000000, "null");
    char recs[8192]; snprintf(recs, sizeof(recs), "%s,%s", rec_ok(0), other);
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "c", "COMPLETE", recs), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    ASSERT_EQ(rep.rejected_binding, 1u);
    ASSERT_EQ(v.rows[1].present, false);
    /* without a pin two digests inside one batch still reject the second */
    st_init(NULL, NULL);
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "c", "COMPLETE", recs), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    ASSERT_EQ(rep.rejected_binding, 1u);
    /* a connector profile reload (new digest on every record, same binding
     * tuple) is accepted without a connector restart: the digest is not part
     * of the pin */
    const char *reloaded = rec(0, 2, "mnt/data", "\"mnt/data:0:u\"", "192.168.64.51", "/mnt/data", 2049,
                               "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:q", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 2, "2026-09-24T10:00:02Z", "c", "COMPLETE", reloaded), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    ASSERT_EQ(rep.rejected_binding, 0u);
    ASSERT_EQ(strcmp(v.profile_digest, "sha256:q"), 0);
}

static void test_endpoint_rule(void)
{
    struct ds_connector_registry_ds ds;
    memset(&ds, 0, sizeof(ds));
    snprintf(ds.host, sizeof(ds.host), "192.168.64.51");
    snprintf(ds.export_path, sizeof(ds.export_path), "/mnt/data/pnfs-ds");
    ds.tcp_port = 2049;
    ASSERT_EQ(ds_connector_endpoint_matches(&ds, "192.168.64.51", "/mnt/data/pnfs-ds", 2049), true);   /* exact */
    ASSERT_EQ(ds_connector_endpoint_matches(&ds, "192.168.64.51", "/mnt/data", 2049), true);           /* under */
    ASSERT_EQ(ds_connector_endpoint_matches(&ds, "192.168.64.51", "/mnt/data/", 2049), true);          /* trailing slash */
    ASSERT_EQ(ds_connector_endpoint_matches(&ds, "192.168.64.51", "/mnt/data/pnfs-ds/", 2049), true);
    ASSERT_EQ(ds_connector_endpoint_matches(&ds, "192.168.64.51", "/", 2049), true);
    ASSERT_EQ(ds_connector_endpoint_matches(&ds, "192.168.64.51", "/mnt/dat", 2049), false);           /* prefix, not a component */
    ASSERT_EQ(ds_connector_endpoint_matches(&ds, "192.168.64.51", "/mnt/data/pnfs-ds/sub", 2049), false);
    ASSERT_EQ(ds_connector_endpoint_matches(&ds, "192.168.64.52", "/mnt/data", 2049), false);
    ASSERT_EQ(ds_connector_endpoint_matches(&ds, "192.168.64.51", "/mnt/data", 2050), false);
    ds.tcp_port = 0;
    ASSERT_EQ(ds_connector_endpoint_matches(&ds, "192.168.64.51", "/mnt/data", 2050), true);           /* unknown registry port */
    ASSERT_EQ(ds_connector_endpoint_matches(&ds, "192.168.64.51", "relative", 2049), false);
}

static void test_binding_rules_in_a_batch(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    const char *wrong_server = rec(0, 2, "mnt/data", "\"mnt/data:0:u\"", "10.0.0.9", "/mnt/data", 2049,
                                   "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "c", "COMPLETE", wrong_server), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rejected_binding, 1u);
    ASSERT_EQ(ST.pins[0].pinned, false);
    const char *wrong_scope = rec(0, 2, "mnt/data", "\"mnt/data:0:u\"", "192.168.64.51", "/mnt/data", 2049,
                                  "NEW_ALLOCATION", "other-scope", "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 2, "2026-09-24T10:00:02Z", "c", "COMPLETE", wrong_scope), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rejected_binding, 1u);
    const char *wrong_kind = rec(0, 2, "mnt/data", "\"mnt/data:0:u\"", "192.168.64.51", "/mnt/data", 2049,
                                 "SOMETHING", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 3, "2026-09-24T10:00:03Z", "c", "COMPLETE", wrong_kind), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rejected_shape, 1u);
    /* pin, then: same generation + changed incarnation -> mismatch; higher generation -> rebound */
    ASSERT_EQ(apply(batch("rt-1", "e1", 4, "2026-09-24T10:00:04Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    const char *new_inc = rec(0, 2, "mnt/data", "\"mnt/data:0:NEW\"", "192.168.64.51", "/mnt/data", 2049,
                              "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 5, "2026-09-24T10:00:05Z", "c", "COMPLETE", new_inc), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rejected_binding, 1u);
    ASSERT_EQ(v.rows[0].present, false);
    const char *rebound = rec(0, 3, "mnt/data", "\"mnt/data:0:NEW\"", "192.168.64.51", "/mnt/data", 2049,
                              "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 6, "2026-09-24T10:00:06Z", "c", "COMPLETE", rebound), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rebound, 1u);
    ASSERT_EQ(v.rows[0].present, true);
    ASSERT_EQ(v.rows[0].valid, false);              /* UNKNOWN until the next fresh record */
    ASSERT_EQ(ST.pins[0].binding_generation, 3u);
    ASSERT_EQ(apply(batch("rt-1", "e1", 7, "2026-09-24T10:00:07Z", "c", "COMPLETE", rebound), &v, &rep), DC_OK);
    ASSERT_EQ(v.rows[0].valid, true);
    /* lower generation after the re-pin is rejected */
    ASSERT_EQ(apply(batch("rt-1", "e1", 8, "2026-09-24T10:00:08Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rejected_binding, 1u);
}

static void test_unknown_duplicate_failed_and_shape(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    const char *unknown = rec(7, 1, "s", "\"i\"", "h", "/x", 2049, "NEW_ALLOCATION", "cluster-default",
                              "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "c", "COMPLETE", unknown), &v, &rep), DC_OK);
    ASSERT_EQ(rep.unknown_ds, 1u);
    char recs[8192]; snprintf(recs, sizeof(recs), "%s,%s", rec_ok(0), rec_ok(0));
    ASSERT_EQ(apply(batch("rt-1", "e1", 2, "2026-09-24T10:00:02Z", "c", "COMPLETE", recs), &v, &rep), DC_OK);
    ASSERT_EQ(v.rows[0].present, false);            /* duplicate: neither trusted */
    ASSERT_TRUE(rep.rejected_shape >= 1u);
    ASSERT_EQ(rep.accepted, 0u);
    ASSERT_EQ(ST.pins[0].pinned, false);            /* ... and nothing is pinned */
    ASSERT_EQ(apply(batch("rt-1", "e1", 3, "2026-09-24T10:00:03Z", "c", "FAILED", rec_ok(0)), &v, &rep), DC_OK);
    ASSERT_EQ(v.rows[0].present, true);
    ASSERT_EQ(v.rows[0].valid, false);              /* FAILED snapshot -> UNKNOWN */
    const char *bad_ttl = rec(0, 2, "mnt/data", "\"mnt/data:0:u\"", "192.168.64.51", "/mnt/data", 2049,
                              "NEW_ALLOCATION", "cluster-default", "VALID", 4000000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 4, "2026-09-24T10:00:04Z", "c", "COMPLETE", bad_ttl), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rejected_shape, 1u);
    const char *bad_ppm = rec(0, 2, "mnt/data", "\"mnt/data:0:u\"", "192.168.64.51", "/mnt/data", 2049,
                              "NEW_ALLOCATION", "cluster-default", "VALID", 1000, "sha256:p", "true", 1000001, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 5, "2026-09-24T10:00:05Z", "c", "COMPLETE", bad_ppm), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rejected_shape, 1u);
    const char *denied = rec(0, 2, "mnt/data", "\"mnt/data:0:u\"", "192.168.64.51", "/mnt/data", 2049,
                             "NEW_ALLOCATION", "cluster-default", "VALID", 1000, "sha256:p", "false", 0, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 6, "2026-09-24T10:00:06Z", "c", "COMPLETE", denied), &v, &rep), DC_OK);
    ASSERT_EQ(v.rows[0].allowed, false);
    ASSERT_EQ(v.rows[0].multiplier_ppm, 0u);
}

static void test_json_and_size_and_schema(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    ASSERT_EQ(apply("{not json", &v, &rep), DC_JSON);
    ASSERT_EQ(apply("[1,2]", &v, &rep), DC_SCHEMA);
    ASSERT_EQ(apply("{\"contract_version\":\"1.0\"}", &v, &rep), DC_SCHEMA);
    ASSERT_EQ(apply("{\"contract_version\":\"1.0\",\"runtime_epoch\":\"r\",\"config_digest\":\"c\",\"generated_at\":\"yesterday\",\"instances\":[]}", &v, &rep), DC_SCHEMA);
    ASSERT_EQ(apply("{\"contract_version\":\"1.0\",\"runtime_epoch\":\"r\",\"config_digest\":\"c\",\"generated_at\":\"2026-09-24T10:00:00Z\",\"instances\":[]}", &v, &rep), DC_OK);
    ASSERT_EQ(v.count, 2u);
    ASSERT_EQ(v.rows[0].present, false);
    ASSERT_EQ(ds_connector_apply_batch(&ST, "x", DC_BATCH_MAX + 1, &REG, 1, &v, &rep), DC_TOO_LARGE);
    ASSERT_EQ(strcmp(ds_connector_drop_name(DC_REPLAY), "REPLAY"), 0);
}

static void test_iso8601(void)
{
    ASSERT_EQ(ds_connector_iso8601_ms("2026-09-24T10:00:00Z", 20), 1790244000000ull);
    ASSERT_EQ(ds_connector_iso8601_ms("2026-09-24T10:00:00.5Z", 22), 1790244000500ull);
    ASSERT_EQ(ds_connector_iso8601_ms("2026-09-24T10:00:00.123456Z", 27), 1790244000123ull);
    ASSERT_EQ(ds_connector_iso8601_ms("2026-09-24 10:00:00Z", 20), 0u);
    ASSERT_EQ(ds_connector_iso8601_ms("2026-09-24T10:00:00", 19), 0u);
    ASSERT_EQ(ds_connector_iso8601_ms("2026-13-24T10:00:00Z", 20), 0u);
}

/* -----------------------------------------------------------------------
 * I/O half: a fake connector on a Unix socket
 * ----------------------------------------------------------------------- */

#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include "ds_cache.h"
#include "mds_catalogue.h"
#include "placement_gate.h"

struct mds_catalogue *catalogue_memdb_open(void);

struct fake_srv {
    char        dir[64];
    char        path[100];
    int         status;
    const char *body;
    long        content_length;   /* -1 = real length */
    int         delay_ms;
    bool        silent;           /* accept, never answer */
    bool        keep_dir;         /* stop leaves the directory for a re-serve */
    bool        no_content_length;
    bool        chunked;          /* advertise Transfer-Encoding: chunked */
    long        stream_bytes;     /* after the body: that many filler bytes */
    int         listen_fd;
    pthread_t   th;
};

static void *fake_srv_thread(void *a)
{
    struct fake_srv *s = a;
    struct pollfd pfd = { .fd = s->listen_fd, .events = POLLIN, .revents = 0 };
    int c;
    char req[2048];

    if (poll(&pfd, 1, 5000) <= 0) {
        return NULL;
    }
    c = accept(s->listen_fd, NULL, NULL);
    if (c < 0) {
        return NULL;
    }
    pfd.fd = c;
    if (poll(&pfd, 1, 1000) > 0) {
        (void)read(c, req, sizeof(req));
    }
    if (s->delay_ms > 0) {
        usleep((useconds_t)s->delay_ms * 1000);
    }
    if (!s->silent) {
        char hdr[256];
        size_t blen = s->body ? strlen(s->body) : 0;
        long cl = (s->content_length >= 0) ? s->content_length : (long)blen;
        int n;
        if (s->no_content_length) {
            n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n%s"
                         "Connection: close\r\n\r\n",
                         s->status, s->status == 200 ? "OK" : "Nope",
                         s->chunked ? "Transfer-Encoding: chunked\r\n" : "");
        } else {
            n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                         "Content-Length: %ld\r\nConnection: close\r\n\r\n",
                         s->status, s->status == 200 ? "OK" : "Nope", cl);
        }
        (void)write(c, hdr, (size_t)n);
        if (blen > 0) {
            (void)write(c, s->body, blen);
        }
        if (s->stream_bytes > 0) {
            static char filler[65536];
            long left = s->stream_bytes;
            memset(filler, 'x', sizeof(filler));
            while (left > 0) {
                size_t chunk = left > (long)sizeof(filler) ? sizeof(filler) : (size_t)left;
                ssize_t w = write(c, filler, chunk);
                if (w <= 0) {
                    break;
                }
                left -= w;
            }
        }
    } else {
        usleep(3000 * 1000);
    }
    close(c);
    return NULL;
}

static int fake_srv_start(struct fake_srv *s, int status, const char *body)
{
    struct sockaddr_un addr;

    if (s->path[0] == '\0') {
        snprintf(s->dir, sizeof(s->dir), "/tmp/pnfs-dc-XXXXXX");
        if (mkdtemp(s->dir) == NULL) {
            return -1;
        }
        snprintf(s->path, sizeof(s->path), "%s/c.sock", s->dir);
    } else {
        (void)unlink(s->path);   /* re-serve at a caller-chosen path */
    }
    s->status = status;
    s->body = body;
    if (s->content_length == 0) {
        s->content_length = -1;
    }
    s->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", s->path);
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s->listen_fd, 4) != 0) {
        return -1;
    }
    return pthread_create(&s->th, NULL, fake_srv_thread, s);
}

static void fake_srv_stop(struct fake_srv *s)
{
    (void)pthread_join(s->th, NULL);
    close(s->listen_fd);
    (void)unlink(s->path);
    if (!s->keep_dir) {
        (void)rmdir(s->dir);
    }
}

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void test_http_get_200_body(void)
{
    struct fake_srv s; memset(&s, 0, sizeof(s));
    char *body = NULL; size_t len = 0; int status = 0;
    ASSERT_EQ(fake_srv_start(&s, 200, "{\"hello\":1}"), 0);
    ASSERT_EQ(ds_connector_http_get(s.path, "/v1/assessments", 1000, &body, &len, &status), MDS_OK);
    ASSERT_EQ(status, 200);
    ASSERT_EQ(len, 11u);
    ASSERT_EQ(strcmp(body, "{\"hello\":1}"), 0);
    free(body);
    fake_srv_stop(&s);
}

static void test_http_get_503_and_other_statuses(void)
{
    struct fake_srv s; memset(&s, 0, sizeof(s));
    char *body = NULL; size_t len = 0; int status = 0;
    ASSERT_EQ(fake_srv_start(&s, 503, "{\"ready\":false}"), 0);
    ASSERT_EQ(ds_connector_http_get(s.path, "/v1/assessments", 1000, &body, &len, &status), MDS_ERR_DELAY);
    ASSERT_EQ(status, 503);
    ASSERT_TRUE(body == NULL);
    fake_srv_stop(&s);
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(fake_srv_start(&s, 404, "{}"), 0);
    ASSERT_EQ(ds_connector_http_get(s.path, "/v1/assessments", 1000, &body, &len, &status), MDS_ERR_INVAL);
    ASSERT_EQ(status, 404);
    fake_srv_stop(&s);
}

static void test_http_get_timeout_and_no_socket(void)
{
    struct fake_srv s; memset(&s, 0, sizeof(s));
    char *body = NULL; size_t len = 0; int status = 0;
    s.silent = true;
    ASSERT_EQ(fake_srv_start(&s, 200, "{}"), 0);
    uint64_t t0 = mono_ms();
    ASSERT_EQ(ds_connector_http_get(s.path, "/v1/assessments", 200, &body, &len, &status), MDS_ERR_IO);
    ASSERT_TRUE(mono_ms() - t0 < 1000);
    fake_srv_stop(&s);
    ASSERT_EQ(ds_connector_http_get("/tmp/pnfs-dc-no-such/c.sock", "/v1/assessments", 200, &body, &len, &status), MDS_ERR_NOTFOUND);
    ASSERT_EQ(ds_connector_http_get(NULL, "/x", 200, &body, &len, &status), MDS_ERR_INVAL);
}

static void test_http_get_oversize(void)
{
    struct fake_srv s; memset(&s, 0, sizeof(s));
    char *body = NULL; size_t len = 0; int status = 0;
    s.content_length = 5L * 1024 * 1024;
    ASSERT_EQ(fake_srv_start(&s, 200, "{}"), 0);
    ASSERT_EQ(ds_connector_http_get(s.path, "/v1/assessments", 1000, &body, &len, &status), MDS_ERR_IO);
    ASSERT_TRUE(body == NULL);
    fake_srv_stop(&s);
}

static struct ds_cache *cache_two_ds(struct mds_catalogue **cat_out)
{
    struct mds_catalogue *cat = catalogue_memdb_open();
    struct ds_cache *c = NULL;
    const char *hosts[2] = { "192.168.64.51", "192.168.64.71" };
    uint32_t i;

    if (cat == NULL) {
        return NULL;
    }
    for (i = 0; i < 2; i++) {
        struct mds_cat_txn *txn = NULL;
        struct mds_ds_info info;
        memset(&info, 0, sizeof(info));
        info.ds_id = i;
        info.state = DS_ONLINE;
        info.port = 2049;
        info.tcp_port = 2049;
        snprintf(info.host, sizeof(info.host), "%s", hosts[i]);
        snprintf(info.export_path, sizeof(info.export_path), "/mnt/data/pnfs-ds");
        if (mds_cat_txn_begin(cat, MDS_CAT_TXN_WRITE, &txn) != MDS_OK ||
            mds_cat_ds_put(cat, txn, &info) != MDS_OK || mds_cat_txn_commit(txn) != MDS_OK) {
            return NULL;
        }
    }
    if (ds_cache_create(cat, &c) != 0) {
        return NULL;
    }
    *cat_out = cat;
    return c;
}

static void test_poll_once_publishes_and_readiness(void)
{
    struct mds_catalogue *cat = NULL;
    struct ds_cache *cache = cache_two_ds(&cat);
    struct mds_config cfg;
    struct fake_srv s;
    struct placement_readiness r;
    char sock_dir[64];
    char sock[100];
    ASSERT_TRUE(cache != NULL);
    memset(&cfg, 0, sizeof(cfg));
    cfg.placement_mode = PM_SMART;
    cfg.placement_mode_set = true;
    cfg.placement_capacity_max_age_ms = 120000;
    cfg.ds_connector_poll_ms = 200;
    cfg.ds_connector_request_deadline_ms = 150;
    cfg.ds_connector_expected_contract_major = 1;
    cfg.ds_connector_max_ds = 256;
    snprintf(cfg.ds_connector_access_scope, sizeof(cfg.ds_connector_access_scope), "cluster-default");
    ASSERT_EQ(placement_gate_init(&cfg, cache), 0);
    placement_gate_readiness(&r);
    ASSERT_EQ(r.mode_active, true);
    ASSERT_EQ(r.connector_config_valid, false);        /* not configured yet */
    ASSERT_EQ(strcmp(r.coverage, "none"), 0);

    /* one socket path for the whole test: the connector is configured once
     * and every later server re-serves at that path */
    memset(&s, 0, sizeof(s));
    s.keep_dir = true;
    ASSERT_EQ(fake_srv_start(&s, 200, batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "cfg-d", "COMPLETE", rec_ok(0))), 0);
    snprintf(sock_dir, sizeof(sock_dir), "%s", s.dir);
    snprintf(sock, sizeof(sock), "%s", s.path);
    snprintf(cfg.ds_connector_socket, sizeof(cfg.ds_connector_socket), "%s", sock);
    ASSERT_EQ(ds_connector_configure(&cfg, cache), 0);
    ASSERT_EQ(ds_connector_poll_once(), MDS_OK);
    fake_srv_stop(&s);
    placement_gate_readiness(&r);
    ASSERT_EQ(r.connector_config_valid, true);
    ASSERT_EQ(r.connector_reachable, true);
    ASSERT_EQ(r.last_batch_valid, true);
    ASSERT_EQ(r.registered_ds, 2u);
    ASSERT_EQ(r.covered_ds, 1u);
    ASSERT_EQ(r.eligible_ds, 1u);
    ASSERT_EQ(strcmp(r.coverage, "partial"), 0);
    ASSERT_EQ(strcmp(r.config_digest, "cfg-d"), 0);
    ASSERT_EQ(strcmp(r.profile_digest, "sha256:p"), 0);
    ASSERT_EQ(atomic_load(&g_branch_metrics.connector_covered_ds), 1u);

#define RESERVE(status, body_) do { \
        memset(&s, 0, sizeof(s)); \
        s.keep_dir = true; \
        snprintf(s.dir, sizeof(s.dir), "%s", sock_dir); \
        snprintf(s.path, sizeof(s.path), "%s", sock); \
        ASSERT_EQ(fake_srv_start(&s, (status), (body_)), 0); \
    } while (0)

    /* the same snapshot re-served (equal sequence) is steady state */
    RESERVE(200, batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "cfg-d", "COMPLETE", rec_ok(0)));
    ASSERT_EQ(ds_connector_poll_once(), MDS_OK);
    fake_srv_stop(&s);
    placement_gate_readiness(&r);
    ASSERT_EQ(r.last_batch_valid, true);
    ASSERT_EQ(atomic_load(&g_branch_metrics.connector_batches_dropped_total[DC_REPLAY]), 0u);

    /* garbage: the batch is dropped, the previous view stays until it ages
     * out; reachable holds for 3 x poll after the last accepted batch */
    RESERVE(200, "{garbage");
    ASSERT_EQ(ds_connector_poll_once(), MDS_ERR_INVAL);
    fake_srv_stop(&s);
    placement_gate_readiness(&r);
    ASSERT_EQ(r.last_batch_valid, false);
    ASSERT_EQ(r.connector_reachable, true);
    ASSERT_EQ(r.covered_ds, 1u);
    ASSERT_EQ(atomic_load(&g_branch_metrics.connector_batches_dropped_total[DC_JSON]) >= 1u, true);

    /* 503 after the grace: the gauge and the fact both drop, on the same rule */
    usleep(700 * 1000);
    RESERVE(503, "{\"ready\":false}");
    ASSERT_EQ(ds_connector_poll_once(), MDS_ERR_DELAY);
    fake_srv_stop(&s);
    placement_gate_readiness(&r);
    ASSERT_EQ(r.connector_reachable, false);
    ASSERT_EQ(atomic_load(&g_branch_metrics.connector_reachable), 0u);
    ASSERT_EQ(atomic_load(&g_branch_metrics.connector_poll_errors_total[DCP_UNAVAILABLE]) >= 1u, true);

    /* socket gone: no fallback -- CONNECT is counted, the rows expire on
     * their own TTL */
    ASSERT_EQ(ds_connector_poll_once(), MDS_ERR_NOTFOUND);
    placement_gate_readiness(&r);
    ASSERT_EQ(r.connector_reachable, false);
    ASSERT_EQ(r.covered_ds, 1u);                        /* 15 s TTL not yet over */
    ASSERT_EQ(atomic_load(&g_branch_metrics.connector_poll_errors_total[DCP_CONNECT]) >= 1u, true);

    /* a good batch restores reachability */
    RESERVE(200, batch("rt-1", "e1", 2, "2026-09-24T10:00:02Z", "cfg-d", "COMPLETE", rec_ok(0)));
    ASSERT_EQ(ds_connector_poll_once(), MDS_OK);
    s.keep_dir = false;
    fake_srv_stop(&s);
    placement_gate_readiness(&r);
    ASSERT_EQ(r.connector_reachable, true);
    ASSERT_EQ(atomic_load(&g_branch_metrics.connector_reachable), 1u);
#undef RESERVE

    ds_connector_stop();
    placement_gate_destroy();
    ds_cache_destroy(cache);
    mds_catalogue_close(cat);
}

static enum ds_connector_drop apply_at(const char *text, uint64_t now,
                                       struct placement_assessment_view *v,
                                       struct ds_connector_report *rep)
{
    return ds_connector_apply_batch(&ST, text, strlen(text), &REG, now, v, rep);
}

static void test_steady_state_refreshes_the_ttl(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    const char *b = batch("rt-1", "e1", 7, "2026-09-24T10:00:07Z", "c", "COMPLETE", rec_ok(0));
    ASSERT_EQ(apply_at(b, 1000000, &v, &rep), DC_OK);
    ASSERT_TRUE(v.rows[0].expires_mono_ms == 1015000ull);
    /* ten polls later the connector still serves sequence 7: every poll is
     * accepted and the row is re-timed from the receive instant */
    for (int i = 1; i <= 10; i++) {
        ASSERT_EQ(apply_at(b, 1000000ull + (uint64_t)i * 1000, &v, &rep), DC_OK);
        ASSERT_EQ(rep.accepted, 1u);
        ASSERT_EQ(v.batch_valid, true);
    }
    ASSERT_TRUE(v.rows[0].expires_mono_ms == 1025000ull);
    ASSERT_TRUE(v.batch_received_mono_ms == 1010000ull);
    ASSERT_EQ(ST.inst[0].sequence, 7u);
}

/* Two connector instances in one batch, each covering one DS. */
static const char *batch2(unsigned seq1, unsigned seq2, const char *generated_at)
{
    snprintf(BUF, sizeof(BUF),
        "{\"contract_version\":\"1.0\",\"runtime_epoch\":\"rt-1\",\"config_digest\":\"c\","
        "\"generated_at\":\"%s\",\"instances\":["
        "{\"connector_instance_id\":\"xi-01\",\"module_type\":\"xinas\",\"epoch\":\"e1\",\"sequence\":%u,"
        "\"snapshot_status\":\"COMPLETE\",\"assessments\":[%s]},"
        "{\"connector_instance_id\":\"xi-02\",\"module_type\":\"xinas\",\"epoch\":\"e9\",\"sequence\":%u,"
        "\"snapshot_status\":\"COMPLETE\",\"assessments\":[%s]}]}",
        generated_at, seq1, rec_ok(0), seq2, rec_ok(1));
    return BUF;
}

static void test_multi_instance_batch(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    ASSERT_EQ(apply(batch2(3, 8, "2026-09-24T10:00:01Z"), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 2u);
    ASSERT_EQ(v.rows[0].present, true);
    ASSERT_EQ(v.rows[1].present, true);
    ASSERT_EQ(strcmp(ST.pins[0].instance, "xi-01"), 0);
    ASSERT_EQ(strcmp(ST.pins[1].instance, "xi-02"), 0);
    /* one instance moves on, the other re-serves: accepted */
    ASSERT_EQ(apply(batch2(4, 8, "2026-09-24T10:00:02Z"), &v, &rep), DC_OK);
    /* one instance goes backwards: the whole batch is a replay, lines keep */
    ASSERT_EQ(apply(batch2(4, 7, "2026-09-24T10:00:03Z"), &v, &rep), DC_REPLAY);
    ASSERT_TRUE(strstr(rep.detail, "xi-02") != NULL);
    int seq1 = -1, seq2 = -1;
    for (int k = 0; k < DC_INSTANCES_MAX; k++) {
        if (ST.inst[k].used && strcmp(ST.inst[k].id, "xi-01") == 0) seq1 = (int)ST.inst[k].sequence;
        if (ST.inst[k].used && strcmp(ST.inst[k].id, "xi-02") == 0) seq2 = (int)ST.inst[k].sequence;
    }
    ASSERT_EQ(seq1, 4);
    ASSERT_EQ(seq2, 8);
}

static void test_instance_lines_cannot_be_exhausted_silently(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    static char big[65536];
    size_t o;
    int i;
    reg_init(); st_init(NULL, NULL);
    /* DC_INSTANCES_MAX distinct ids fill every line */
    o = (size_t)snprintf(big, sizeof(big),
        "{\"contract_version\":\"1.0\",\"runtime_epoch\":\"rt-1\",\"config_digest\":\"c\","
        "\"generated_at\":\"2026-09-24T10:00:01Z\",\"instances\":[");
    for (i = 0; i < DC_INSTANCES_MAX; i++) {
        o += (size_t)snprintf(big + o, sizeof(big) - o,
            "%s{\"connector_instance_id\":\"inst-%03d\",\"module_type\":\"xinas\",\"epoch\":\"e\","
            "\"sequence\":1,\"snapshot_status\":\"COMPLETE\",\"assessments\":[]}", i ? "," : "", i);
    }
    o += (size_t)snprintf(big + o, sizeof(big) - o, "]}");
    ASSERT_EQ(apply(big, &v, &rep), DC_OK);
    /* a batch with one id nobody has a line for is refused, not run unprotected */
    const char *extra = "{\"contract_version\":\"1.0\",\"runtime_epoch\":\"rt-1\",\"config_digest\":\"c\","
        "\"generated_at\":\"2026-09-24T10:00:02Z\",\"instances\":["
        "{\"connector_instance_id\":\"inst-new\",\"module_type\":\"xinas\",\"epoch\":\"e\","
        "\"sequence\":1,\"snapshot_status\":\"COMPLETE\",\"assessments\":[]}]}";
    ASSERT_EQ(apply(extra, &v, &rep), DC_SCHEMA);
    ASSERT_TRUE(strstr(rep.detail, "sequence lines free") != NULL);
    /* a known id still goes through */
    const char *known = "{\"contract_version\":\"1.0\",\"runtime_epoch\":\"rt-1\",\"config_digest\":\"c\","
        "\"generated_at\":\"2026-09-24T10:00:03Z\",\"instances\":["
        "{\"connector_instance_id\":\"inst-005\",\"module_type\":\"xinas\",\"epoch\":\"e\","
        "\"sequence\":2,\"snapshot_status\":\"COMPLETE\",\"assessments\":[]}]}";
    ASSERT_EQ(apply(known, &v, &rep), DC_OK);
    /* a new runtime epoch frees every line */
    const char *fresh = "{\"contract_version\":\"1.0\",\"runtime_epoch\":\"rt-2\",\"config_digest\":\"c\","
        "\"generated_at\":\"2026-09-24T10:00:04Z\",\"instances\":["
        "{\"connector_instance_id\":\"inst-new\",\"module_type\":\"xinas\",\"epoch\":\"e\","
        "\"sequence\":1,\"snapshot_status\":\"COMPLETE\",\"assessments\":[]}]}";
    ASSERT_EQ(apply(fresh, &v, &rep), DC_OK);
}

static size_t nested_doc(char *doc, size_t depth, const char *filler)
{
    size_t o = (size_t)snprintf(doc, 512, "{\"contract_version\":\"1.0\",\"runtime_epoch\":\"rt-1\","
                                "\"config_digest\":\"c\",\"generated_at\":\"2026-09-24T10:00:01Z\",\"deep\":");
    memset(doc + o, '[', depth); o += depth;
    o += (size_t)snprintf(doc + o, 512, "%s", filler);
    memset(doc + o, ']', depth); o += depth;
    o += (size_t)snprintf(doc + o, 512, ",\"instances\":[]}");
    return o;
}

static void test_deep_nesting_is_bounded_and_skipped_without_recursion(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    char *doc = malloc(1024 * 1024);
    size_t o;
    ASSERT_TRUE(doc != NULL);
    reg_init(); st_init(NULL, NULL);
    /* an unknown envelope key holding a 63-deep array (the envelope itself is
     * level 1): skipped iteratively, the batch is fine */
    o = nested_doc(doc, 63, "\"]}[{\"");
    ASSERT_EQ(ds_connector_apply_batch(&ST, doc, o, &REG, 1000000, &v, &rep), DC_OK);
    ASSERT_EQ(v.count, 2u);
    /* one level more is refused before jsmn parses it */
    o = nested_doc(doc, 64, "");
    ASSERT_EQ(ds_connector_apply_batch(&ST, doc, o, &REG, 1000000, &v, &rep), DC_JSON);
    ASSERT_TRUE(strstr(rep.detail, "nesting") != NULL);
    /* 400 000 levels (jsmn is quadratic on depth: minutes) are refused in
     * one linear pass -- the poll thread never stalls */
    o = nested_doc(doc, 400000, "");
    uint64_t t0 = mono_ms();
    ASSERT_EQ(ds_connector_apply_batch(&ST, doc, o, &REG, 1000000, &v, &rep), DC_JSON);
    ASSERT_TRUE(mono_ms() - t0 < 1000);
    free(doc);
}

static void test_unicode_escapes_in_strings(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    /* json.dumps writes non-ASCII as \uXXXX: the Cyrillic share name and an
     * emoji (surrogate pair) both decode to UTF-8 */
    const char *cyr = rec(0, 2, "\\u0448\\u0430\\u0440\\u0430", "\"a:\\ud83d\\ude00\"", "192.168.64.51", "/mnt/data", 2049,
                          "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "c", "COMPLETE", cyr), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    ASSERT_EQ(strcmp(ST.pins[0].target_id, "\xd1\x88\xd0\xb0\xd1\x80\xd0\xb0"), 0);
    ASSERT_EQ(strcmp(ST.pins[0].target_incarnation, "a:\xf0\x9f\x98\x80"), 0);
    /* the same tuple spelled raw UTF-8 matches the pin */
    const char *raw = rec(0, 2, "\xd1\x88\xd0\xb0\xd1\x80\xd0\xb0", "\"a:\xf0\x9f\x98\x80\"", "192.168.64.51", "/mnt/data", 2049,
                          "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 2, "2026-09-24T10:00:02Z", "c", "COMPLETE", raw), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    ASSERT_EQ(rep.rejected_binding, 0u);
    /* malformed escapes never yield a partial string: jsmn (strict) rejects
     * a bad escape as invalid JSON, the decoder rejects a lone surrogate and
     * NUL as a record shape error */
    const char *bad_json[] = { "\\u12", "\\x41", "tail\\" };
    for (unsigned i = 0; i < sizeof(bad_json) / sizeof(bad_json[0]); i++) {
        const char *r = rec(1, 1, bad_json[i], "\"i\"", "192.168.64.71", "/mnt/data", 2049,
                            "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
        ASSERT_EQ(apply(batch("rt-1", "e1", 3 + i, "2026-09-24T10:00:03Z", "c", "COMPLETE", r), &v, &rep), DC_JSON);
        ASSERT_EQ(ST.pins[1].pinned, false);
    }
    const char *bad_shape[] = { "\\ud83d", "\\ude00", "\\u0000", "\\ud83d\\u0041" };
    for (unsigned i = 0; i < sizeof(bad_shape) / sizeof(bad_shape[0]); i++) {
        const char *r = rec(1, 1, bad_shape[i], "\"i\"", "192.168.64.71", "/mnt/data", 2049,
                            "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
        ASSERT_EQ(apply(batch("rt-1", "e1", 6 + i, "2026-09-24T10:00:03Z", "c", "COMPLETE", r), &v, &rep), DC_OK);
        ASSERT_EQ(rep.rejected_shape, 1u);
        ASSERT_EQ(ST.pins[1].pinned, false);
    }
    /* the ordinary escapes decode too */
    const char *esc = rec(1, 1, "a\\/b\\\"c\\\\d", "\"i\"", "192.168.64.71", "/mnt/data", 2049,
                          "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 20, "2026-09-24T10:00:20Z", "c", "COMPLETE", esc), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    ASSERT_EQ(strcmp(ST.pins[1].target_id, "a/b\"c\\d"), 0);
}

static void test_unobserved_incarnation_on_unknown_records(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    reg_init(); st_init(NULL, NULL);
    /* the source is down before the first VALID record: UNKNOWN + null
     * incarnation is accepted, the row is present and invalid, nothing pins */
    const char *down = rec(0, 2, "mnt/data", "null", "192.168.64.51", "/mnt/data", 2049,
                           "NEW_ALLOCATION", "cluster-default", "UNKNOWN", 15000, "sha256:p", "false", 0, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "c", "COMPLETE", down), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    ASSERT_EQ(rep.rejected_binding, 0u);
    ASSERT_EQ(v.rows[0].present, true);
    ASSERT_EQ(v.rows[0].valid, false);
    ASSERT_EQ(ST.pins[0].pinned, false);
    /* the source comes back: the VALID record pins */
    ASSERT_EQ(apply(batch("rt-1", "e1", 2, "2026-09-24T10:00:02Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_OK);
    ASSERT_EQ(ST.pins[0].pinned, true);
    ASSERT_EQ(strcmp(ST.pins[0].target_incarnation, "mnt/data:0:u"), 0);
    /* the source goes down again (the stand: xinas-agent stopped): the
     * unobserved record matches the pin on the rest of the tuple -- UNKNOWN,
     * not BINDING_MISMATCH, and the pin keeps its incarnation */
    ASSERT_EQ(apply(batch("rt-1", "e1", 3, "2026-09-24T10:00:03Z", "c", "COMPLETE", down), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    ASSERT_EQ(rep.rejected_binding, 0u);
    ASSERT_EQ(v.rows[0].present, true);
    ASSERT_EQ(v.rows[0].valid, false);
    ASSERT_EQ(strcmp(ST.pins[0].target_incarnation, "mnt/data:0:u"), 0);
    /* ... but the rest of the tuple is still checked */
    const char *other = rec(0, 2, "other-share", "null", "192.168.64.51", "/mnt/data", 2049,
                            "NEW_ALLOCATION", "cluster-default", "UNKNOWN", 15000, "sha256:p", "false", 0, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 4, "2026-09-24T10:00:04Z", "c", "COMPLETE", other), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rejected_binding, 1u);
    ASSERT_TRUE(strstr(rep.detail, "BINDING_MISMATCH") != NULL);
    /* a VALID record must carry the incarnation */
    const char *valid_null = rec(0, 2, "mnt/data", "null", "192.168.64.51", "/mnt/data", 2049,
                                 "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 5, "2026-09-24T10:00:05Z", "c", "COMPLETE", valid_null), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rejected_shape, 1u);
    ASSERT_EQ(v.rows[0].present, false);
    /* a rebind announced while the source is down clears the pin; the next
     * VALID record with the new generation pins */
    const char *rebind_down = rec(0, 3, "mnt/data", "null", "192.168.64.51", "/mnt/data", 2049,
                                  "NEW_ALLOCATION", "cluster-default", "UNKNOWN", 15000, "sha256:p", "false", 0, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 6, "2026-09-24T10:00:06Z", "c", "COMPLETE", rebind_down), &v, &rep), DC_OK);
    ASSERT_EQ(rep.rebound, 1u);
    ASSERT_EQ(ST.pins[0].pinned, false);
    const char *rebound_valid = rec(0, 3, "mnt/data", "\"mnt/data:0:NEW\"", "192.168.64.51", "/mnt/data", 2049,
                                    "NEW_ALLOCATION", "cluster-default", "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 7, "2026-09-24T10:00:07Z", "c", "COMPLETE", rebound_valid), &v, &rep), DC_OK);
    ASSERT_EQ(rep.accepted, 1u);
    ASSERT_EQ(ST.pins[0].binding_generation, 3u);
    ASSERT_EQ(strcmp(ST.pins[0].target_incarnation, "mnt/data:0:NEW"), 0);
    ASSERT_EQ(v.rows[0].valid, true);
}

static void test_contract_version_shapes(void)
{
    struct placement_assessment_view v; struct ds_connector_report rep;
    const char *bad[] = { "1x", "01.0", "1", "x1.0", ".0", "10.0", "" };
    const char *good[] = { "1.0", "1.7", "1.0.3" };
    reg_init(); st_init(NULL, NULL);
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char doc[512];
        snprintf(doc, sizeof(doc), "{\"contract_version\":\"%s\",\"runtime_epoch\":\"r\",\"config_digest\":\"c\","
                 "\"generated_at\":\"2026-09-24T10:00:00Z\",\"instances\":[]}", bad[i]);
        ASSERT_EQ(apply(doc, &v, &rep), DC_CONTRACT_MAJOR);
    }
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        char doc[512];
        snprintf(doc, sizeof(doc), "{\"contract_version\":\"%s\",\"runtime_epoch\":\"r\",\"config_digest\":\"c\","
                 "\"generated_at\":\"2026-09-24T10:00:00Z\",\"instances\":[]}", good[i]);
        ASSERT_EQ(apply(doc, &v, &rep), DC_OK);
    }
}

static void test_http_get_streamed_oversize_and_short_body(void)
{
    struct fake_srv s; memset(&s, 0, sizeof(s));
    char *body = NULL; size_t len = 0; int status = 0;
    /* no Content-Length, 5 MiB streamed: capped, never buffered whole */
    s.no_content_length = true;
    s.stream_bytes = 5L * 1024 * 1024;
    ASSERT_EQ(fake_srv_start(&s, 200, "{}"), 0);
    ASSERT_EQ(ds_connector_http_get(s.path, "/v1/assessments", 3000, &body, &len, &status), MDS_ERR_IO);
    ASSERT_TRUE(body == NULL);
    fake_srv_stop(&s);
    /* Content-Length larger than what arrives: bounded by the deadline */
    memset(&s, 0, sizeof(s));
    s.content_length = 100;
    s.silent = false;
    ASSERT_EQ(fake_srv_start(&s, 200, "{}"), 0);
    uint64_t t0 = mono_ms();
    ASSERT_EQ(ds_connector_http_get(s.path, "/v1/assessments", 200, &body, &len, &status), MDS_ERR_IO);
    ASSERT_TRUE(mono_ms() - t0 < 1500);
    ASSERT_TRUE(body == NULL);
    fake_srv_stop(&s);
    /* chunked framing is refused rather than mis-parsed */
    memset(&s, 0, sizeof(s));
    s.no_content_length = true;
    s.chunked = true;
    ASSERT_EQ(fake_srv_start(&s, 200, "2\r\n{}\r\n0\r\n\r\n"), 0);
    ASSERT_EQ(ds_connector_http_get(s.path, "/v1/assessments", 1000, &body, &len, &status), MDS_ERR_IO);
    ASSERT_TRUE(body == NULL);
    fake_srv_stop(&s);
}

int main(void)
{
    printf("test_ds_connector\n");
    RUN_TEST(test_http_get_200_body);
    RUN_TEST(test_http_get_503_and_other_statuses);
    RUN_TEST(test_http_get_timeout_and_no_socket);
    RUN_TEST(test_http_get_oversize);
    RUN_TEST(test_poll_once_publishes_and_readiness);
    RUN_TEST(test_iso8601);
    RUN_TEST(test_endpoint_rule);
    RUN_TEST(test_healthy_batch_is_accepted);
    RUN_TEST(test_contract_major_mismatch);
    RUN_TEST(test_replay_drops_the_batch_and_keeps_state);
    RUN_TEST(test_runtime_epoch_change_resets_pins);
    RUN_TEST(test_old_generated_at_is_dropped);
    RUN_TEST(test_config_digest_pin);
    RUN_TEST(test_profile_digest_pin_and_consistency);
    RUN_TEST(test_binding_rules_in_a_batch);
    RUN_TEST(test_unknown_duplicate_failed_and_shape);
    RUN_TEST(test_json_and_size_and_schema);
    RUN_TEST(test_steady_state_refreshes_the_ttl);
    RUN_TEST(test_multi_instance_batch);
    RUN_TEST(test_instance_lines_cannot_be_exhausted_silently);
    RUN_TEST(test_deep_nesting_is_bounded_and_skipped_without_recursion);
    RUN_TEST(test_unicode_escapes_in_strings);
    RUN_TEST(test_contract_version_shapes);
    RUN_TEST(test_unobserved_incarnation_on_unknown_records);
    RUN_TEST(test_http_get_streamed_oversize_and_short_body);
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
