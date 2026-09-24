/*
 * SPDX-License-Identifier: MIT
 *
 * placement_modes.h -- the operator-facing placement modes (rr / fill /
 * smart): mode enum, INI key names, defaults and ranges.
 *
 * The values here mirror docs/placement-modes/contract-manifest.json in
 * XinnorLab/pNFS; a unit test pins the numbers so the two cannot drift
 * silently.  Design: docs/superpowers/specs/2026-09-23-placement-modes-
 * design.md (XinnorLab/pNFS), sections 4 and 5.
 */

#ifndef PLACEMENT_MODES_H
#define PLACEMENT_MODES_H

#include <stdint.h>

/*
 * PM_LEGACY: `placement_mode` absent -- the upstream placement_policy*
 * keys drive selection exactly as before.  The three explicit modes
 * take the dispatcher branch and the candidate gate (placement_gate.h).
 */
enum placement_mode {
    PM_LEGACY = 0,
    PM_RR     = 1,
    PM_FILL   = 2,
    PM_SMART  = 3,
};

enum placement_shrink {
    PM_SHRINK_ALLOW  = 0,   /* fewer eligible DS than stripes: place fewer stripes */
    PM_SHRINK_STRICT = 1,   /* ... refuse the layout instead */
};

#define PM_KEY_MODE                 "placement_mode"
#define PM_KEY_CAP_MAX_AGE_MS       "placement_capacity_max_age_ms"
#define PM_KEY_MIN_FREE_BYTES       "placement_min_free_bytes"
#define PM_KEY_DOMAIN_PREFIX        "ds_capacity_domain."
#define PM_KEY_DOMAIN_WEIGHT_PREFIX "placement_domain_weight."
#define PM_KEY_ALLOW_MANUAL         "placement_allow_manual_base_weights"
#define PM_KEY_SHRINK               "placement_stripe_shrink"

#define PM_DEFAULT_CAP_MAX_AGE_MS   120000u
#define PM_CAP_MAX_AGE_MS_MAX       86400000u
#define PM_DOMAIN_ID_MAX            128
#define PM_DOMAIN_WEIGHT_MIN        1u
#define PM_DOMAIN_WEIGHT_MAX        10000u
#define PM_WEIGHT_SCALE             65536u
#define PM_MAX_DOMAINS              256   /* == MDS_MAX_DS_NODES; asserted in placement_config.c */

/* Connector client (placement_mode = smart), design section 7. */
#define PM_KEY_CONN_ENABLED          "ds_connector_enabled"
#define PM_KEY_CONN_SOCKET           "ds_connector_socket"
#define PM_KEY_CONN_POLL_MS          "ds_connector_poll_ms"
#define PM_KEY_CONN_DEADLINE_MS      "ds_connector_request_deadline_ms"
#define PM_KEY_CONN_CONTRACT_MAJOR   "ds_connector_expected_contract_major"
#define PM_KEY_CONN_MAX_DS           "ds_connector_max_ds"
#define PM_KEY_CONN_ACCESS_SCOPE     "ds_connector_access_scope"
#define PM_KEY_CONN_PROFILE_DIGEST   "ds_connector_expected_profile_digest"
#define PM_KEY_CONN_CONFIG_DIGEST    "ds_connector_expected_config_digest"
#define PM_DEFAULT_CONN_SOCKET       "/run/lattice-ds-connector/connector.sock"
#define PM_DEFAULT_CONN_POLL_MS      1000u
#define PM_CONN_POLL_MS_MIN          200u
#define PM_CONN_POLL_MS_MAX          10000u
#define PM_DEFAULT_CONN_DEADLINE_MS  500u
#define PM_CONN_DEADLINE_MS_MIN      50u
#define PM_DEFAULT_CONN_CONTRACT_MAJOR 1u
#define PM_DEFAULT_CONN_MAX_DS       256u
#define PM_DEFAULT_CONN_ACCESS_SCOPE "cluster-default"
#define PM_CONN_SCOPE                "NEW_ALLOCATION"
#define PM_DIGEST_MAX                128
#define PM_SCOPE_MAX                 64

/* Bounded reason vocabulary of the gate (metrics labels, logs, config show). */
enum placement_reason {
    PR_NONE = 0,
    PR_DS_OFFLINE,
    PR_CAPACITY_UNKNOWN,
    PR_CAPACITY_STALE,
    PR_CAPACITY_FULL,
    PR_DOMAIN_MAP_CONTRADICTION,
    PR_SHARED_FS_ALIAS_UNMAPPED,
    PR_ASSESSMENT_UNKNOWN,
    PR_ASSESSMENT_STALE,
    PR_CONNECTOR_DENIED,
    PR_ZERO_MULTIPLIER,
    PR_NO_BINDING,
    PR_NO_ELIGIBLE_DS,
    PR_INSUFFICIENT_ELIGIBLE_DS,
    PR_MODE_NOT_READY,
    PR_WEIGHT_OVERFLOW,
    PR_COUNT
};

const char *placement_reason_name(enum placement_reason r);
const char *placement_mode_name(enum placement_mode m);     /* "legacy"|"rr"|"fill"|"smart" */
const char *placement_shrink_name(enum placement_shrink s); /* "allow"|"strict" */

#endif /* PLACEMENT_MODES_H */
