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
        int n = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                         "Content-Length: %ld\r\nConnection: close\r\n\r\n",
                         s->status, s->status == 200 ? "OK" : "Nope", cl);
        (void)write(c, hdr, (size_t)n);
        if (blen > 0) {
            (void)write(c, s->body, blen);
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

    snprintf(s->dir, sizeof(s->dir), "/tmp/pnfs-dc-XXXXXX");
    if (mkdtemp(s->dir) == NULL) {
        return -1;
    }
    snprintf(s->path, sizeof(s->path), "%s/c.sock", s->dir);
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
    (void)rmdir(s->dir);
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
    ASSERT_EQ(ds_connector_http_get("/tmp/pnfs-dc-no-such/c.sock", "/v1/assessments", 200, &body, &len, &status), MDS_ERR_IO);
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

    memset(&s, 0, sizeof(s));
    ASSERT_EQ(fake_srv_start(&s, 200, batch("rt-1", "e1", 1, "2026-09-24T10:00:01Z", "cfg-d", "COMPLETE", rec_ok(0))), 0);
    snprintf(cfg.ds_connector_socket, sizeof(cfg.ds_connector_socket), "%s", s.path);
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

    /* garbage: the batch is dropped, the previous view stays until it ages out */
    memset(&s, 0, sizeof(s));
    ASSERT_EQ(fake_srv_start(&s, 200, "{garbage"), 0);
    snprintf(cfg.ds_connector_socket, sizeof(cfg.ds_connector_socket), "%s", s.path);
    ASSERT_EQ(ds_connector_configure(&cfg, cache), 0);   /* new socket path; state reset is fine here */
    ASSERT_EQ(ds_connector_poll_once(), MDS_ERR_INVAL);
    fake_srv_stop(&s);
    placement_gate_readiness(&r);
    ASSERT_EQ(r.last_batch_valid, false);
    ASSERT_EQ(r.covered_ds, 1u);
    ASSERT_EQ(atomic_load(&g_branch_metrics.connector_batches_dropped_total[DC_JSON]) >= 1u, true);

    /* socket gone: no fallback -- reachability lapses after 3 x poll and the
     * rows expire on their own TTL */
    ASSERT_EQ(ds_connector_poll_once(), MDS_ERR_IO);
    usleep(700 * 1000);
    ASSERT_EQ(ds_connector_poll_once(), MDS_ERR_IO);
    placement_gate_readiness(&r);
    ASSERT_EQ(r.connector_reachable, false);
    ASSERT_EQ(r.covered_ds, 1u);                        /* 15 s TTL not yet over */
    ASSERT_EQ(atomic_load(&g_branch_metrics.connector_reachable), 0u);

    ds_connector_stop();
    placement_gate_destroy();
    ds_cache_destroy(cache);
    mds_catalogue_close(cat);
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
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
