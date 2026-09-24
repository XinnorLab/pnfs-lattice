/*
 * SPDX-License-Identifier: MIT
 *
 * placement_gate.h -- the candidate gate in front of every DS selector
 * and the admission check at the create-if-absent boundary (XinnorLab
 * placement modes; design sections 5, 5a, 6).
 *
 * Two layers:
 *   - pure functions over explicit views (placement_candidates,
 *     placement_admit, placement_ds_admitted, placement_weight) that unit
 *     tests drive with synthetic data;
 *   - a process singleton (placement_gate_init & co.) that owns the
 *     effective mode, publishes the capacity view built from the DS
 *     cache, and mints admission tokens for proxy_io.
 *
 * Legacy (placement_mode absent): the singleton is never initialised,
 * placement_gate_mode() is PM_LEGACY and the selection sites take the
 * upstream path via placement_select_gated().
 */

#ifndef PLACEMENT_GATE_H
#define PLACEMENT_GATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "pnfs_mds.h"
#include "placement_modes.h"
#include "ds_cache.h"

/* enum placement_reason and placement_reason_name() live in placement_modes.h. */

/* Immutable capacity input for one decision (built from the DS cache). */
struct placement_capacity_view {
    uint32_t count;
    struct ds_capacity_view_row rows[MDS_MAX_DS_NODES];
};

/* Connector assessments (placement_mode = smart), design section 7.
 * Built by ds_connector.c from a validated batch; published into the gate
 * with placement_gate_publish_assessments(); one row per registered DS. */
#define PA_REASONS_MAX  4
#define PA_REASON_LEN   32
struct placement_assessment_row {
    uint32_t ds_id;
    bool     present;            /* an accepted record exists for this ds */
    bool     valid;              /* quality == VALID and the instance snapshot was not FAILED */
    bool     allowed;
    uint32_t multiplier_ppm;
    uint64_t expires_mono_ms;    /* received + remaining_ttl_ms (MDS clock) */
    uint64_t received_mono_ms;
    char     domain[PM_DOMAIN_ID_MAX];   /* resources.capacity_domain_id, "" when null */
    char     reasons[PA_REASONS_MAX][PA_REASON_LEN];
    uint32_t reason_count;
};
struct placement_assessment_view {
    uint32_t count;
    struct placement_assessment_row rows[MDS_MAX_DS_NODES];
    bool     batch_valid;
    uint64_t batch_received_mono_ms;
    char     config_digest[PM_DIGEST_MAX];
    char     profile_digest[PM_DIGEST_MAX];
};

struct placement_ctx {
    enum placement_mode   mode;
    enum placement_shrink shrink;
    uint64_t              now_mono_ms;
    uint32_t              capacity_max_age_ms;
    uint64_t              min_free_bytes;
    /* cfg->ds_capacity_domain (indexed by ds_id) or NULL. */
    const char          (*domain_of)[PM_DOMAIN_ID_MAX];
    const struct placement_capacity_view   *cap;      /* NULL in rr/legacy */
    const struct placement_assessment_view *assess;   /* NULL until Stage B */
    _Atomic uint32_t     *rr_counter;                 /* shared rr cursor; NULL = rr_key only */
    /* Manual base weights (smart, placement_allow_manual_base_weights). */
    const char          (*domain_weight_id)[PM_DOMAIN_ID_MAX];
    const uint32_t       *domain_weight;
    uint32_t              domain_weight_count;
    /* Singleton bookkeeping: the view references held by this ctx. */
    void                 *view_ref;
    void                 *assess_ref;
};

struct placement_candidate {
    uint32_t idx;                      /* index into the caller's ds_list */
    uint32_t ds_id;
    uint64_t weight;                   /* > 0; 1 in rr */
    char     domain[PM_DOMAIN_ID_MAX]; /* "" in rr */
};

struct placement_reject_counts {
    uint32_t by_reason[PR_COUNT];
};

/*
 * Fixed-point weight = domain_weight * ppm * PM_WEIGHT_SCALE / n_aliases,
 * computed in unsigned __int128.  Returns 0 (and sets *overflow when
 * given) if any input is 0, domain_weight exceeds PM_DOMAIN_WEIGHT_MAX,
 * the result is 0, or MDS_MAX_DS_NODES such weights would reach 2^62.
 */
uint64_t placement_weight(uint32_t domain_weight, uint32_t ppm,
                          uint32_t n_aliases, bool *overflow);

/*
 * Filter ds_list (already ONLINE/profile/io-limit filtered by the caller
 * for the native rules; the gate re-checks DS_ONLINE) by mode.  Writes
 * up to n candidates to out[] and per-reason rejection counts to why
 * (may be NULL).  Returns the candidate count.
 */
uint32_t placement_candidates(const struct placement_ctx *ctx,
                              const struct mds_ds_info *ds_list, uint32_t n,
                              struct placement_candidate *out,
                              struct placement_reject_counts *why);

/*
 * The one entry point for a new backing object's DS selection.
 * stripe_count is in/out: on MDS_OK it holds the effective count (as
 * placement_select2); unchanged on error.  MDS_ERR_NOSPC + *reason on
 * refusal; MDS_ERR_INVAL on bad arguments; MDS_ERR_NOMEM.
 * entries must hold *stripe_count x mirror_count slots.
 */
enum mds_status placement_admit(const struct placement_ctx *ctx,
                                const struct mds_ds_info *ds_list, uint32_t n,
                                uint32_t *stripe_count, uint32_t mirror_count,
                                uint64_t rr_key,
                                struct mds_ds_map_entry *entries,
                                enum placement_reason *reason);

/* Per-DS verdict under the same rules (used at the create boundary). */
bool placement_ds_admitted(const struct placement_ctx *ctx,
                           const struct mds_ds_info *ds_list, uint32_t n,
                           uint32_t ds_id, enum placement_reason *reason);

/* Diagnostics hook for a suspected (unprovable) alias: counter + WARN. */
void placement_gate_note_alias_suspected(uint32_t a, uint32_t b);
/* Per-DS rejection counts into the metrics (+ ERROR for the alias grades). */
void placement_gate_note_rejections(const struct placement_reject_counts *why);

/* -----------------------------------------------------------------------
 * Process singleton (Task 5) -- declared here, implemented alongside.
 * ----------------------------------------------------------------------- */

/* Connector-side facts the poll thread reports (smart). */
struct placement_connector_facts {
    bool     config_valid;
    bool     reachable;              /* a successful poll within 3 x poll_ms */
    bool     last_batch_valid;
    uint64_t last_success_mono_ms;   /* 0 = never */
    enum ds_connector_poll_error last_error;
    enum ds_connector_drop last_drop;
    char     last_detail[160];
};
void placement_gate_publish_assessments(const struct placement_assessment_view *view);
void placement_gate_set_connector_facts(const struct placement_connector_facts *f);

/* The four readiness facts + coverage (design section 7, review finding 4). */
struct placement_readiness {
    bool     mode_active;              /* effective mode is smart */
    bool     connector_config_valid;
    bool     connector_reachable;
    bool     last_batch_valid;
    uint32_t registered_ds;
    uint32_t covered_ds;               /* fresh valid records */
    uint32_t eligible_ds;              /* covered and allowed with ppm > 0 */
    char     coverage[8];              /* "full" | "partial" | "none" | "n/a" */
    uint64_t last_success_mono_ms;
    char     config_digest[PM_DIGEST_MAX];
    char     profile_digest[PM_DIGEST_MAX];
    char     last_detail[160];
};
void placement_gate_readiness(struct placement_readiness *out);

int  placement_gate_init(const struct mds_config *cfg, struct ds_cache *cache);
void placement_gate_destroy(void);
enum placement_mode placement_gate_mode(void);        /* PM_LEGACY when not initialised */
const char *placement_gate_generation(void);          /* "" when not initialised */
void placement_gate_publish_capacity(void);           /* rebuild the view from the DS cache */
void placement_gate_ctx(struct placement_ctx *out, uint64_t now_mono_ms);
void placement_gate_ctx_release(struct placement_ctx *ctx);

/*
 * Site helper: gated selection when a mode is set, the upstream selector
 * otherwise (legacy_policy_enabled ? placement_select_ex2 : placement_select2,
 * or placement_select_rr_at2 when rr_key != 0).
 */
enum mds_status placement_select_gated(bool legacy_policy_enabled,
                                       enum mds_placement_policy legacy_policy,
                                       const struct mds_ds_info *ds_list, uint32_t n,
                                       uint32_t *stripe_count, uint32_t mirror_count,
                                       uint32_t stripe_unit, uint64_t rr_key,
                                       struct mds_ds_map_entry *entries,
                                       enum placement_reason *reason);

/* Per-DS view for `config show` (design section 9). */
struct placement_ds_status {
    bool     registered;
    uint32_t state;
    char     domain[PM_DOMAIN_ID_MAX];
    uint64_t capacity_age_ms;   /* UINT64_MAX when never observed */
    uint64_t avail_bytes;
    uint64_t total_bytes;
    uint64_t weight;            /* 0 when not a candidate */
    enum placement_reason reason;
};

/* DS ids in the published view (0 in rr/legacy or before the first publish). */
uint32_t placement_gate_ds_ids(uint32_t *ids, uint32_t cap);
/* The gate's current verdict for one DS; false when unknown to the gate. */
bool placement_gate_ds_status(uint32_t ds_id, struct placement_ds_status *out);

/* -----------------------------------------------------------------------
 * Create-boundary admission (Task 7).
 * ----------------------------------------------------------------------- */

enum placement_purpose {
    PP_NEW_OBJECT       = 1,
    PP_RECREATE_MISSING = 2,
};

struct placement_token {
    uint32_t ds_id;
    uint32_t purpose;
    uint64_t minted_mono_ms;
    uint32_t snapshot_gen;
};

#define PLACEMENT_TOKEN_MAX_AGE_MS 2000u

enum mds_status placement_gate_admit_create(uint32_t ds_id, enum placement_purpose p,
                                            struct placement_token *tok,
                                            enum placement_reason *reason);
bool placement_token_valid(const struct placement_token *tok, uint32_t ds_id,
                           uint64_t now_mono_ms);

#endif /* PLACEMENT_GATE_H */
