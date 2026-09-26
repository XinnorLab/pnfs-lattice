/*
 * SPDX-License-Identifier: MIT
 *
 * placement_config.c -- validation and the config generation for the
 * placement modes (placement_modes.h).  Pure functions over struct
 * mds_config so the rules are unit-tested without the INI parser.
 *
 * Rules (design section 4):
 *   - an explicit placement_mode rejects every legacy placement key and a
 *     workload_profile that sets a placement policy (PLACEMENT_MODE_CONFLICT);
 *   - ds_weight.<id> is a conflict in fill; placement_domain_weight.* needs
 *     smart + placement_allow_manual_base_weights (DOMAIN_WEIGHT_FORBIDDEN);
 *   - fill/smart need ds_capacity_poll_ms > 0 and a larger max age (RANGE);
 *   - smart needs default_mirror_count == 1 and a build with
 *     ENABLE_DS_CONNECTOR (MIRROR_COUNT_UNSUPPORTED / PLACEMENT_MODE_UNSUPPORTED_BUILD).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include <openssl/sha.h>

#include "pnfs_mds.h"
#include "placement_modes.h"

_Static_assert(PM_MAX_DOMAINS == MDS_MAX_DS_NODES,
               "PM_MAX_DOMAINS must equal MDS_MAX_DS_NODES");

const char *placement_mode_name(enum placement_mode m)
{
    switch (m) {
    case PM_RR:    return "rr";
    case PM_FILL:  return "fill";
    case PM_SMART: return "smart";
    case PM_LEGACY:
    default:       return "legacy";
    }
}

static const char *const reason_names[PR_COUNT] = {
    [PR_NONE]                     = "NONE",
    [PR_DS_OFFLINE]               = "DS_OFFLINE",
    [PR_CAPACITY_UNKNOWN]         = "CAPACITY_UNKNOWN",
    [PR_CAPACITY_STALE]           = "CAPACITY_STALE",
    [PR_CAPACITY_FULL]            = "CAPACITY_FULL",
    [PR_DOMAIN_MAP_CONTRADICTION] = "DOMAIN_MAP_CONTRADICTION",
    [PR_SHARED_FS_ALIAS_UNMAPPED] = "SHARED_FS_ALIAS_UNMAPPED",
    [PR_ASSESSMENT_UNKNOWN]       = "ASSESSMENT_UNKNOWN",
    [PR_ASSESSMENT_STALE]         = "ASSESSMENT_STALE",
    [PR_CONNECTOR_DENIED]         = "CONNECTOR_DENIED",
    [PR_ZERO_MULTIPLIER]          = "ZERO_MULTIPLIER",
    [PR_NO_BINDING]               = "NO_BINDING",
    [PR_NO_ELIGIBLE_DS]           = "NO_ELIGIBLE_DS",
    [PR_INSUFFICIENT_ELIGIBLE_DS] = "INSUFFICIENT_ELIGIBLE_DS",
    [PR_MODE_NOT_READY]           = "MODE_NOT_READY",
    [PR_WEIGHT_OVERFLOW]          = "WEIGHT_OVERFLOW",
    [PR_DOMAIN_MAP_MISMATCH]      = "DOMAIN_MAP_MISMATCH",
};

const char *placement_reason_name(enum placement_reason r)
{
    if ((unsigned)r >= PR_COUNT || reason_names[r] == NULL) {
        return "UNKNOWN_REASON";
    }
    return reason_names[r];
}

static const char *const drop_names[DC_COUNT] = {
    [DC_OK] = "OK",
    [DC_JSON] = "JSON",
    [DC_SCHEMA] = "SCHEMA",
    [DC_CONTRACT_MAJOR] = "CONTRACT_MAJOR",
    [DC_REPLAY] = "REPLAY",
    [DC_OLD_GENERATED_AT] = "OLD_GENERATED_AT",
    [DC_CONFIG_DIGEST] = "CONFIG_DIGEST",
    [DC_TOO_LARGE] = "TOO_LARGE",
    [DC_PROFILE_INCONSISTENT] = "PROFILE_INCONSISTENT",
    [DC_PROFILE_LIMIT] = "PROFILE_LIMIT",
};

const char *ds_connector_drop_name(enum ds_connector_drop d)
{
    if ((unsigned)d >= DC_COUNT || drop_names[d] == NULL) {
        return "UNKNOWN";
    }
    return drop_names[d];
}

static const char *const poll_error_names[DCP_COUNT] = {
    [DCP_NONE] = "NONE",
    [DCP_CONNECT] = "CONNECT",
    [DCP_TIMEOUT] = "TIMEOUT",
    [DCP_HTTP] = "HTTP",
    [DCP_UNAVAILABLE] = "UNAVAILABLE",
    [DCP_DROP] = "DROP",
};

const char *ds_connector_poll_error_name(enum ds_connector_poll_error e)
{
    if ((unsigned)e >= DCP_COUNT || poll_error_names[e] == NULL) {
        return "UNKNOWN";
    }
    return poll_error_names[e];
}

const char *placement_shrink_name(enum placement_shrink s)
{
    return (s == PM_SHRINK_STRICT) ? "strict" : "allow";
}

enum placement_mode placement_config_effective_mode(const struct mds_config *cfg)
{
    if (cfg == NULL || !cfg->placement_mode_set) {
        return PM_LEGACY;
    }
    return cfg->placement_mode;
}

static enum mds_status fail(char *err, size_t cap, const char *msg)
{
    if (err != NULL && cap > 0) {
        (void)snprintf(err, cap, "%s", msg);
    }
    return MDS_ERR_INVAL;
}

enum mds_status placement_config_validate(const struct mds_config *cfg,
                                          char *err, size_t cap)
{
    if (cfg == NULL) {
        return fail(err, cap, "RANGE: null config");
    }
    if (!cfg->placement_mode_set) {
        if (cfg->ds_connector_enabled_set && cfg->ds_connector_enabled) {
            return fail(err, cap,
                "PLACEMENT_MODE_CONFLICT: ds_connector_enabled = true needs placement_mode = smart");
        }
        return MDS_OK;
    }
    if (cfg->placement_policy_set || cfg->placement_policy_enabled_set ||
        cfg->placement_capacity_weighting_set) {
        return fail(err, cap,
            "PLACEMENT_MODE_CONFLICT: placement_policy, "
            "placement_policy_enabled and placement_capacity_weighting "
            "cannot be combined with placement_mode (remove the legacy keys)");
    }
    if (cfg->workload_profile != MDS_PROFILE_DEFAULT &&
        (cfg->tuning_set & MDS_CFG_SET_PLACEMENT_POLICY)) {
        return fail(err, cap,
            "PLACEMENT_MODE_CONFLICT: the selected workload_profile sets a "
            "placement policy; use a profile without one or drop placement_mode");
    }
    if (cfg->placement_mode == PM_FILL && cfg->ds_weight_set) {
        return fail(err, cap,
            "PLACEMENT_MODE_CONFLICT: ds_weight.<id> is not allowed in fill "
            "(weights come from the fill level)");
    }
    if (cfg->placement_domain_weight_count > 0 &&
        (cfg->placement_mode != PM_SMART ||
         !cfg->placement_allow_manual_base_weights)) {
        return fail(err, cap,
            "DOMAIN_WEIGHT_FORBIDDEN: placement_domain_weight.* needs "
            "placement_mode = smart and placement_allow_manual_base_weights = true");
    }
    if (cfg->placement_mode == PM_FILL || cfg->placement_mode == PM_SMART) {
        if (cfg->ds_capacity_poll_ms == 0) {
            return fail(err, cap,
                "RANGE: ds_capacity_poll_ms must be > 0 in fill/smart");
        }
        if (cfg->placement_capacity_max_age_ms <= cfg->ds_capacity_poll_ms) {
            return fail(err, cap,
                "RANGE: placement_capacity_max_age_ms must exceed ds_capacity_poll_ms");
        }
    }
    /* ds_connector_enabled is derived from the mode; an explicit value that
     * contradicts it is a mistake, not a setting (design section 4). */
    if (cfg->ds_connector_enabled_set) {
        if (cfg->placement_mode == PM_SMART && !cfg->ds_connector_enabled) {
            return fail(err, cap,
                "PLACEMENT_MODE_CONFLICT: smart needs the connector; "
                "ds_connector_enabled = false contradicts placement_mode");
        }
        if (cfg->placement_mode != PM_SMART && cfg->ds_connector_enabled) {
            return fail(err, cap,
                "PLACEMENT_MODE_CONFLICT: ds_connector_enabled = true is only "
                "meaningful with placement_mode = smart");
        }
    }
    if (cfg->placement_mode == PM_SMART) {
        if (cfg->default_mirror_count > 1) {
            return fail(err, cap,
                "MIRROR_COUNT_UNSUPPORTED: smart requires default_mirror_count = 1");
        }
        if (cfg->ds_connector_poll_ms < PM_CONN_POLL_MS_MIN ||
            cfg->ds_connector_poll_ms > PM_CONN_POLL_MS_MAX) {
            return fail(err, cap,
                "RANGE: ds_connector_poll_ms must be 200..10000");
        }
        if (cfg->ds_connector_request_deadline_ms < PM_CONN_DEADLINE_MS_MIN ||
            cfg->ds_connector_request_deadline_ms > cfg->ds_connector_poll_ms) {
            return fail(err, cap,
                "RANGE: ds_connector_request_deadline_ms must be 50..ds_connector_poll_ms");
        }
        if (cfg->ds_connector_max_ds == 0 || cfg->ds_connector_max_ds > MDS_MAX_DS_NODES) {
            return fail(err, cap, "RANGE: ds_connector_max_ds must be 1..256");
        }
        if (cfg->ds_connector_expected_contract_major == 0) {
            return fail(err, cap, "RANGE: ds_connector_expected_contract_major must be >= 1");
        }
        if (cfg->ds_connector_socket[0] != '/') {
            return fail(err, cap, "RANGE: ds_connector_socket must be an absolute path");
        }
        if (strlen(cfg->ds_connector_socket) >= 108) {
            return fail(err, cap, "RANGE: ds_connector_socket must be shorter than 108 bytes (sun_path)");
        }
#ifndef ENABLE_DS_CONNECTOR
        return fail(err, cap,
            "PLACEMENT_MODE_UNSUPPORTED_BUILD: smart needs a binary built "
            "with ENABLE_DS_CONNECTOR=ON");
#endif
    }
    return MDS_OK;
}

/* Canonical text of the managed keys; order and format are the contract. */
struct dw_pair {
    const char *id;
    uint32_t    w;
};

static int dw_cmp(const void *a, const void *b)
{
    return strcmp(((const struct dw_pair *)a)->id,
                  ((const struct dw_pair *)b)->id);
}

void placement_config_generation(const struct mds_config *cfg, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    char *buf;
    size_t cap = 64 * 1024;
    size_t off = 0;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    struct dw_pair pairs[PM_MAX_DOMAINS];
    uint32_t i;

    out[0] = '\0';
    if (cfg == NULL) {
        return;
    }
    buf = malloc(cap);
    if (buf == NULL) {
        return;
    }
#define APPEND(...) do { \
        int n_ = snprintf(buf + off, cap - off, __VA_ARGS__); \
        if (n_ < 0 || (size_t)n_ >= cap - off) { free(buf); return; } \
        off += (size_t)n_; \
    } while (0)

    APPEND("mode=%s\n", placement_mode_name(cfg->placement_mode));
    APPEND("poll=%u\n", (unsigned)cfg->ds_capacity_poll_ms);
    APPEND("max_age=%u\n", (unsigned)cfg->placement_capacity_max_age_ms);
    APPEND("min_free=%llu\n", (unsigned long long)cfg->placement_min_free_bytes);
    APPEND("shrink=%s\n", placement_shrink_name(cfg->placement_stripe_shrink));
    APPEND("allow_manual=%d\n", cfg->placement_allow_manual_base_weights ? 1 : 0);
    for (i = 0; i < MDS_MAX_DS_NODES; i++) {
        if (cfg->ds_capacity_domain[i][0] != '\0') {
            APPEND("domain.%u=%s\n", (unsigned)i, cfg->ds_capacity_domain[i]);
        }
    }
    for (i = 0; i < cfg->placement_domain_weight_count && i < PM_MAX_DOMAINS; i++) {
        pairs[i].id = cfg->placement_domain_weight_id[i];
        pairs[i].w = cfg->placement_domain_weight[i];
    }
    if (i > 0) {
        qsort(pairs, i, sizeof(pairs[0]), dw_cmp);
    }
    for (uint32_t j = 0; j < i; j++) {
        APPEND("domain_weight.%s=%u\n", pairs[j].id, (unsigned)pairs[j].w);
    }
    APPEND("stripe=%u\n", (unsigned)cfg->default_stripe_count);
    APPEND("mirror=%u\n", (unsigned)cfg->default_mirror_count);
    if (cfg->placement_mode == PM_SMART) {
        APPEND("conn_socket=%s\n", cfg->ds_connector_socket);
        APPEND("conn_poll=%u\n", (unsigned)cfg->ds_connector_poll_ms);
        APPEND("conn_deadline=%u\n", (unsigned)cfg->ds_connector_request_deadline_ms);
        APPEND("conn_major=%u\n", (unsigned)cfg->ds_connector_expected_contract_major);
        APPEND("conn_max_ds=%u\n", (unsigned)cfg->ds_connector_max_ds);
        APPEND("conn_scope=%s\n", cfg->ds_connector_access_scope);
        {
            char pins[PM_PROFILES_MAX * (PM_PROFILE_ID_MAX + PM_DIGEST_MAX + 1)];

            if (pm_format_profile_pins(cfg->ds_connector_expected_profiles,
                                       cfg->ds_connector_expected_profile_count,
                                       pins, sizeof(pins)) < 0) {
                free(buf);
                return;
            }
            APPEND("conn_profiles=%s\n", pins);
        }
        APPEND("conn_config=%s\n", cfg->ds_connector_expected_config_digest);
    }
#undef APPEND

    (void)SHA256((const unsigned char *)buf, off, digest);
    free(buf);
    for (i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = '\0';
}
bool pm_profile_id_valid(const char *s, size_t len)
{
    size_t i;

    if (s == NULL || len == 0 || len >= PM_PROFILE_ID_MAX) {
        return false;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static int pin_cmp(const void *a, const void *b)
{
    return strcmp(((const struct pm_profile_pin *)a)->id,
                  ((const struct pm_profile_pin *)b)->id);
}

static void trim(const char **s, size_t *len)
{
    while (*len > 0 && isspace((unsigned char)**s)) {
        (*s)++;
        (*len)--;
    }
    while (*len > 0 && isspace((unsigned char)(*s)[*len - 1])) {
        (*len)--;
    }
}

int pm_parse_profile_pins(const char *val, struct pm_profile_pin out[PM_PROFILES_MAX],
                          uint32_t *count, char *err, size_t errcap)
{
    const char *p = val;
    uint32_t n = 0;
    uint32_t i;

    *count = 0;
    if (val == NULL || val[0] == '\0') {
        (void)snprintf(err, errcap, "empty value");
        return -1;
    }
    for (;;) {
        const char *end = strchr(p, ',');
        size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        const char *id = p;
        const char *dg;
        size_t idl;
        size_t dgl;

        if (eq == NULL) {
            (void)snprintf(err, errcap, "item '%.*s' is not id=digest", (int)len, p);
            return -1;
        }
        idl = (size_t)(eq - p);
        dg = eq + 1;
        dgl = len - idl - 1;
        trim(&id, &idl);
        trim(&dg, &dgl);
        if (!pm_profile_id_valid(id, idl)) {
            (void)snprintf(err, errcap, "profile id '%.*s' must match [A-Za-z0-9._-]{1,63}",
                           (int)idl, id);
            return -1;
        }
        if (dgl == 0 || dgl >= PM_DIGEST_MAX) {
            (void)snprintf(err, errcap, "profile %.*s: digest must be 1..%d bytes",
                           (int)idl, id, PM_DIGEST_MAX - 1);
            return -1;
        }
        if (n == PM_PROFILES_MAX) {
            (void)snprintf(err, errcap, "more than %d profiles", PM_PROFILES_MAX);
            return -1;
        }
        (void)snprintf(out[n].id, sizeof(out[n].id), "%.*s", (int)idl, id);
        (void)snprintf(out[n].digest, sizeof(out[n].digest), "%.*s", (int)dgl, dg);
        for (i = 0; i < n; i++) {
            if (strcmp(out[i].id, out[n].id) == 0) {
                (void)snprintf(err, errcap, "profile %s pinned twice", out[n].id);
                return -1;
            }
        }
        n++;
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    qsort(out, n, sizeof(out[0]), pin_cmp);
    *count = n;
    return 0;
}

int pm_format_profile_pins(const struct pm_profile_pin *p, uint32_t n,
                           char *buf, size_t cap)
{
    size_t off = 0;
    uint32_t i;
    int w;

    if (cap == 0) {
        return -1;
    }
    if (n == 0) {
        w = snprintf(buf, cap, "-");
        return (w < 0 || (size_t)w >= cap) ? -1 : w;
    }
    for (i = 0; i < n; i++) {
        w = snprintf(buf + off, cap - off, "%s%s=%s", i == 0 ? "" : ",", p[i].id, p[i].digest);
        if (w < 0 || (size_t)w >= cap - off) {
            buf[0] = '\0';
            return -1;
        }
        off += (size_t)w;
    }
    return (int)off;
}
