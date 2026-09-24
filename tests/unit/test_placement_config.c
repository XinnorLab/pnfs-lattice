/*
 * SPDX-License-Identifier: MIT
 *
 * test_placement_config.c -- placement_mode parsing, conflict rules,
 * ranges and the config generation (design section 4).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pnfs_mds.h"
#include "placement_modes.h"

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
    fprintf(stdout, "  %-52s", #fn); \
    fflush(stdout); \
    fn(); \
    if (test_failed == 0) { tests_passed++; fprintf(stdout, "PASS\n"); } \
    else { fprintf(stdout, "FAILED\n"); } \
} while (0)

static int write_tmp_ini(const char *content, char path_out[128])
{
    (void)snprintf(path_out, 128, "/tmp/pnfs-pm-cfg-%d.conf", (int)getpid());
    FILE *fp = fopen(path_out, "w");
    if (fp == NULL) {
        return -1;
    }
    (void)fputs(content, fp);
    (void)fclose(fp);
    return 0;
}

static void test_absent_key_is_legacy(void)
{
    struct mds_config cfg; char path[128];
    ASSERT_EQ(write_tmp_ini("placement_policy_enabled = true\nplacement_policy = wrr\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);
    ASSERT_EQ(cfg.placement_mode, PM_LEGACY);
    ASSERT_EQ(cfg.placement_mode_set, false);
    ASSERT_EQ(placement_config_effective_mode(&cfg), PM_LEGACY);
    ASSERT_EQ(cfg.placement_policy_enabled, true);
    ASSERT_EQ(cfg.placement_policy, PLACEMENT_WEIGHTED_RR);
    ASSERT_EQ(cfg.placement_config_generation[0], '\0');
}

static void test_each_mode_parses_with_defaults(void)
{
    const char *modes[] = {"rr", "fill"};
    for (int i = 0; i < 2; i++) {
        struct mds_config cfg; char path[128], ini[128];
        (void)snprintf(ini, sizeof(ini), "placement_mode = %s\n", modes[i]);
        ASSERT_EQ(write_tmp_ini(ini, path), 0);
        ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);
        ASSERT_EQ(cfg.placement_mode, i == 0 ? PM_RR : PM_FILL);
        ASSERT_EQ(cfg.placement_mode_set, true);
        ASSERT_EQ(cfg.placement_capacity_max_age_ms, 120000u);
        ASSERT_EQ(cfg.placement_min_free_bytes, 0u);
        ASSERT_EQ(cfg.placement_stripe_shrink, PM_SHRINK_ALLOW);
        ASSERT_EQ(cfg.placement_allow_manual_base_weights, false);
        ASSERT_EQ(strlen(cfg.placement_config_generation), 64u);
        ASSERT_EQ(cfg.placement_policy_enabled, true);
        ASSERT_EQ(cfg.placement_policy, i == 0 ? PLACEMENT_RR : PLACEMENT_WEIGHTED_RR);
    }
}

static void test_smart_parses_with_connector_defaults(void)
{
    struct mds_config cfg; char path[128];
    ASSERT_EQ(write_tmp_ini("placement_mode = smart\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);   /* ENABLE_DS_CONNECTOR build */
    ASSERT_EQ(cfg.placement_mode, PM_SMART);
    ASSERT_EQ(cfg.ds_connector_enabled, true);
    ASSERT_EQ(cfg.ds_connector_enabled_set, false);
    ASSERT_EQ(strcmp(cfg.ds_connector_socket, "/run/lattice-ds-connector/connector.sock"), 0);
    ASSERT_EQ(cfg.ds_connector_poll_ms, 1000u);
    ASSERT_EQ(cfg.ds_connector_request_deadline_ms, 500u);
    ASSERT_EQ(cfg.ds_connector_expected_contract_major, 1u);
    ASSERT_EQ(cfg.ds_connector_max_ds, 256u);
    ASSERT_EQ(strcmp(cfg.ds_connector_access_scope, "cluster-default"), 0);
    ASSERT_EQ(cfg.ds_connector_expected_profile_digest[0], '\0');
    ASSERT_EQ(cfg.placement_policy, PLACEMENT_WEIGHTED_RR);
    ASSERT_EQ(strlen(cfg.placement_config_generation), 64u);
    /* explicit, consistent values */
    ASSERT_EQ(write_tmp_ini("placement_mode = smart\nds_connector_enabled = true\nds_connector_socket = /tmp/c.sock\nds_connector_poll_ms = 5000\nds_connector_request_deadline_ms = 4000\nds_connector_max_ds = 8\nds_connector_access_scope = lab\nds_connector_expected_profile_digest = sha256:abc\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);
    ASSERT_EQ(strcmp(cfg.ds_connector_socket, "/tmp/c.sock"), 0);
    ASSERT_EQ(cfg.ds_connector_poll_ms, 5000u);
    ASSERT_EQ(cfg.ds_connector_request_deadline_ms, 4000u);
    ASSERT_EQ(cfg.ds_connector_max_ds, 8u);
    ASSERT_EQ(strcmp(cfg.ds_connector_access_scope, "lab"), 0);
    ASSERT_EQ(strcmp(cfg.ds_connector_expected_profile_digest, "sha256:abc"), 0);
}

static void test_smart_connector_conflicts_and_ranges(void)
{
    const char *bad[] = {
        "placement_mode = smart\nds_connector_enabled = false\n",
        "placement_mode = rr\nds_connector_enabled = true\n",
        "placement_mode = fill\nds_connector_enabled = true\n",
        "placement_mode = smart\ndefault_mirror_count = 2\n",
        "placement_mode = smart\nds_capacity_poll_ms = 0\n",
        "placement_mode = smart\nds_connector_poll_ms = 100\n",
        "placement_mode = smart\nds_connector_poll_ms = 20000\n",
        "placement_mode = smart\nds_connector_request_deadline_ms = 2000\n",
        "placement_mode = smart\nds_connector_request_deadline_ms = 10\n",
        "placement_mode = smart\nds_connector_max_ds = 0\n",
        "placement_mode = smart\nds_connector_max_ds = 257\n",
        "placement_mode = smart\nds_connector_expected_contract_major = 0\n",
        "placement_mode = smart\nds_connector_socket = relative.sock\n",
        "placement_mode = smart\nds_connector_socket = /run/lattice-ds-connector/"
            "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789.sock\n",
        "ds_connector_enabled = true\n",                            /* the switch alone names no mode */
        "placement_policy_enabled = true\nds_connector_enabled = true\n",
        "placement_mode = smart\nds_connector_poll_ms = 1s\n",
        "placement_mode = smart\nds_connector_access_scope = \n",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct mds_config cfg; char path[128];
        ASSERT_EQ(write_tmp_ini(bad[i], path), 0);
        ASSERT_EQ(mds_config_load(path, &cfg), MDS_ERR_INVAL);
    }
    /* rr/fill ignore the connector keys other than the enabled switch */
    struct mds_config cfg; char path[128];
    /* the longest socket path that still fits sun_path (107 bytes) */
    ASSERT_EQ(write_tmp_ini("placement_mode = smart\nds_connector_socket = /run/lattice-ds-connector/"
        "0123456789012345678901234567890123456789012345678901234567890123456789012345.sock\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);
    ASSERT_EQ(strlen(cfg.ds_connector_socket), 107u);
    /* without a mode the connector switch may only be off */
    ASSERT_EQ(write_tmp_ini("placement_policy_enabled = true\nds_connector_enabled = false\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);
    ASSERT_EQ(cfg.placement_mode, PM_LEGACY);
    ASSERT_EQ(cfg.ds_connector_enabled, false);
    ASSERT_EQ(write_tmp_ini("placement_mode = fill\nds_connector_socket = /tmp/x.sock\nds_connector_enabled = false\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);
    ASSERT_EQ(cfg.ds_connector_enabled, false);
}

static void test_generation_covers_connector_keys(void)
{
    struct mds_config a, b; char path[128];
    ASSERT_EQ(write_tmp_ini("placement_mode = smart\nds_connector_socket = /run/a.sock\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &a), MDS_OK);
    ASSERT_EQ(write_tmp_ini("placement_mode = smart\nds_connector_socket = /run/b.sock\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &b), MDS_OK);
    ASSERT_TRUE(strcmp(a.placement_config_generation, b.placement_config_generation) != 0);
    /* in fill the connector keys are not part of the managed set */
    ASSERT_EQ(write_tmp_ini("placement_mode = fill\nds_connector_socket = /run/a.sock\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &a), MDS_OK);
    ASSERT_EQ(write_tmp_ini("placement_mode = fill\nds_connector_socket = /run/b.sock\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &b), MDS_OK);
    ASSERT_EQ(strcmp(a.placement_config_generation, b.placement_config_generation), 0);
}

static void test_legacy_keys_conflict_with_mode(void)
{
    const char *bad[] = {
        "placement_mode = rr\nplacement_policy = rr\n",
        "placement_mode = rr\nplacement_policy_enabled = true\n",
        "placement_mode = rr\nplacement_policy_enabled = false\n",
        "placement_mode = fill\nplacement_capacity_weighting = proportional\n",
        "placement_mode = fill\nplacement_capacity_weighting = off\n",
        "placement_mode = fill\nds_weight.0 = 3\n",
        "placement_mode = fill\nworkload_profile = hpc\n",
        "placement_mode = rr\nworkload_profile = genomics\n",
        "placement_mode = fill\nplacement_domain_weight.d1 = 5\n",
        "placement_mode = rr\nplacement_domain_weight.d1 = 5\nplacement_allow_manual_base_weights = true\n",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct mds_config cfg; char path[128];
        ASSERT_EQ(write_tmp_ini(bad[i], path), 0);
        ASSERT_EQ(mds_config_load(path, &cfg), MDS_ERR_INVAL);
    }
    /* rr may carry ds_weight (ignored by rr; forbidden only in fill). */
    struct mds_config cfg; char path[128];
    ASSERT_EQ(write_tmp_ini("placement_mode = rr\nds_weight.0 = 3\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);
    /* the default profile sets nothing and is compatible */
    ASSERT_EQ(write_tmp_ini("placement_mode = fill\nworkload_profile = default\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);
}

static void test_ranges(void)
{
    const char *bad[] = {
        "placement_mode = fill\nds_capacity_poll_ms = 0\n",
        "placement_mode = fill\nds_capacity_poll_ms = 60000\nplacement_capacity_max_age_ms = 60000\n",
        "placement_mode = fill\nplacement_capacity_max_age_ms = 86400001\n",
        "placement_mode = fill\nplacement_capacity_max_age_ms = 0\n",
        "placement_mode = fill\nplacement_stripe_shrink = maybe\n",
        "placement_mode = fill\nds_capacity_domain.3 = \n",
        "placement_mode = fill\nds_capacity_domain.999 = d\n",
        "placement_mode = fill\nds_capacity_domain.x = d\n",
        "placement_mode = sideways\n",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct mds_config cfg; char path[128];
        ASSERT_EQ(write_tmp_ini(bad[i], path), 0);
        ASSERT_EQ(mds_config_load(path, &cfg), MDS_ERR_INVAL);
    }
    struct mds_config cfg; char path[128];
    ASSERT_EQ(write_tmp_ini(
        "placement_mode = fill\n"
        "ds_capacity_poll_ms = 30000\n"
        "placement_capacity_max_age_ms = 90000\n"
        "placement_min_free_bytes = 1073741824\n"
        "placement_stripe_shrink = strict\n"
        "ds_capacity_domain.0 = xi/fs-1\n"
        "ds_capacity_domain.1 = xi/fs-1\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);
    ASSERT_EQ(cfg.placement_capacity_max_age_ms, 90000u);
    ASSERT_TRUE(cfg.placement_min_free_bytes == 1073741824ull);
    ASSERT_EQ(cfg.placement_stripe_shrink, PM_SHRINK_STRICT);
    ASSERT_EQ(strcmp(cfg.ds_capacity_domain[0], "xi/fs-1"), 0);
    ASSERT_EQ(strcmp(cfg.ds_capacity_domain[1], "xi/fs-1"), 0);
    ASSERT_EQ(cfg.ds_capacity_domain[2][0], '\0');
}

static void test_numbers_are_parsed_strictly(void)
{
    const char *bad[] = {
        "placement_mode = fill\nplacement_min_free_bytes = -1\n",
        "placement_mode = fill\nplacement_min_free_bytes = 1G\n",
        "placement_mode = fill\nplacement_capacity_max_age_ms = 12abc\n",
        "placement_mode = fill\nplacement_capacity_max_age_ms = +5\n",
        "placement_mode = fill\nplacement_min_free_bytes = 99999999999999999999999\n",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct mds_config cfg; char path[128];
        ASSERT_EQ(write_tmp_ini(bad[i], path), 0);
        ASSERT_EQ(mds_config_load(path, &cfg), MDS_ERR_INVAL);
    }
}

static void test_generation_covers_the_poll_interval(void)
{
    struct mds_config a, b; char path[128];
    ASSERT_EQ(write_tmp_ini("placement_mode = fill\nds_capacity_poll_ms = 30000\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &a), MDS_OK);
    ASSERT_EQ(write_tmp_ini("placement_mode = fill\nds_capacity_poll_ms = 20000\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &b), MDS_OK);
    ASSERT_TRUE(strcmp(a.placement_config_generation, b.placement_config_generation) != 0);
}

static void test_rr_does_not_need_the_probe(void)
{
    struct mds_config cfg; char path[128];
    ASSERT_EQ(write_tmp_ini("placement_mode = rr\nds_capacity_poll_ms = 0\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);
}

static void test_generation_is_stable_and_sensitive(void)
{
    struct mds_config a, b; char path[128];
    ASSERT_EQ(write_tmp_ini("placement_mode = fill\nds_capacity_domain.1 = d\nds_capacity_domain.0 = d\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &a), MDS_OK);
    ASSERT_EQ(write_tmp_ini("ds_capacity_domain.0 = d\nplacement_mode = fill\nds_capacity_domain.1 = d\n# comment\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &b), MDS_OK);
    ASSERT_EQ(strcmp(a.placement_config_generation, b.placement_config_generation), 0);
    ASSERT_EQ(write_tmp_ini("placement_mode = fill\nds_capacity_domain.0 = d\nds_capacity_domain.1 = e\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &b), MDS_OK);
    ASSERT_TRUE(strcmp(a.placement_config_generation, b.placement_config_generation) != 0);
    ASSERT_EQ(write_tmp_ini("placement_mode = rr\nds_capacity_domain.0 = d\nds_capacity_domain.1 = d\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &b), MDS_OK);
    ASSERT_TRUE(strcmp(a.placement_config_generation, b.placement_config_generation) != 0);
}

static void test_rr_keeps_geometry(void)
{
    struct mds_config cfg; char path[128];
    ASSERT_EQ(write_tmp_ini("placement_mode = rr\ndefault_stripe_count = 4\n", path), 0);
    ASSERT_EQ(mds_config_load(path, &cfg), MDS_OK);
    ASSERT_EQ(cfg.default_stripe_count, 4u);
    ASSERT_EQ(cfg.placement_policy_enabled, true);
    ASSERT_EQ(cfg.placement_policy, PLACEMENT_RR);
}

static void test_validate_is_pure(void)
{
    struct mds_config cfg; char err[256];
    memset(&cfg, 0, sizeof(cfg));
    ASSERT_EQ(placement_config_validate(&cfg, err, sizeof(err)), MDS_OK);   /* legacy: nothing to check */
    cfg.placement_mode_set = true; cfg.placement_mode = PM_FILL;
    cfg.ds_capacity_poll_ms = 1000; cfg.placement_capacity_max_age_ms = 5000;
    ASSERT_EQ(placement_config_validate(&cfg, err, sizeof(err)), MDS_OK);
    cfg.placement_capacity_max_age_ms = 1000;
    ASSERT_EQ(placement_config_validate(&cfg, err, sizeof(err)), MDS_ERR_INVAL);
    ASSERT_EQ(strncmp(err, "RANGE:", 6), 0);
    ASSERT_EQ(placement_config_validate(NULL, err, sizeof(err)), MDS_ERR_INVAL);
}

static void test_manifest_constants(void)
{
    /* Pinned to docs/placement-modes/contract-manifest.json in XinnorLab/pNFS. */
    ASSERT_EQ(PM_WEIGHT_SCALE, 65536u);
    ASSERT_EQ(PM_DOMAIN_WEIGHT_MIN, 1u);
    ASSERT_EQ(PM_DOMAIN_WEIGHT_MAX, 10000u);
    ASSERT_EQ(PM_DEFAULT_CAP_MAX_AGE_MS, 120000u);
    ASSERT_EQ(PM_CAP_MAX_AGE_MS_MAX, 86400000u);
    ASSERT_EQ(PM_DOMAIN_ID_MAX, 128);
    ASSERT_EQ(PM_MAX_DOMAINS, MDS_MAX_DS_NODES);
    ASSERT_EQ(strcmp(placement_mode_name(PM_FILL), "fill"), 0);
    ASSERT_EQ(strcmp(placement_mode_name(PM_LEGACY), "legacy"), 0);
}

int main(void)
{
    printf("test_placement_config\n");
    RUN_TEST(test_absent_key_is_legacy);
    RUN_TEST(test_each_mode_parses_with_defaults);
    RUN_TEST(test_smart_parses_with_connector_defaults);
    RUN_TEST(test_smart_connector_conflicts_and_ranges);
    RUN_TEST(test_generation_covers_connector_keys);
    RUN_TEST(test_legacy_keys_conflict_with_mode);
    RUN_TEST(test_ranges);
    RUN_TEST(test_rr_does_not_need_the_probe);
    RUN_TEST(test_numbers_are_parsed_strictly);
    RUN_TEST(test_generation_covers_the_poll_interval);
    RUN_TEST(test_generation_is_stable_and_sensitive);
    RUN_TEST(test_rr_keeps_geometry);
    RUN_TEST(test_validate_is_pure);
    RUN_TEST(test_manifest_constants);
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
