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
    ASSERT_EQ(apply(batch("rt-1", "e1", 5, "2026-09-24T10:00:06Z", "c", "COMPLETE", rec_ok(0)), &v, &rep), DC_REPLAY);
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
    const char *other = rec(1, 1, "s", "null", "192.168.64.71", "/mnt/data", 2049, "NEW_ALLOCATION",
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
    const char *unknown = rec(7, 1, "s", "null", "h", "/x", 2049, "NEW_ALLOCATION", "cluster-default",
                              "VALID", 15000, "sha256:p", "true", 1000000, "null");
    ASSERT_EQ(apply(batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "c", "COMPLETE", unknown), &v, &rep), DC_OK);
    ASSERT_EQ(rep.unknown_ds, 1u);
    char recs[8192]; snprintf(recs, sizeof(recs), "%s,%s", rec_ok(0), rec_ok(0));
    ASSERT_EQ(apply(batch("rt-1", "e1", 2, "2026-09-24T10:00:02Z", "c", "COMPLETE", recs), &v, &rep), DC_OK);
    ASSERT_EQ(v.rows[0].present, false);            /* duplicate: neither trusted */
    ASSERT_TRUE(rep.rejected_shape >= 1u);
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

int main(void)
{
    printf("test_ds_connector\n");
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
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
