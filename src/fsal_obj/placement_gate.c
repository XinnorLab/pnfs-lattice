/*
 * SPDX-License-Identifier: MIT
 *
 * placement_gate.c -- candidate gate and admission for the placement
 * modes.  See placement_gate.h and the design (XinnorLab/pNFS
 * docs/superpowers/specs/2026-09-23-placement-modes-design.md).
 *
 * Pure part (this half of the file): everything takes a placement_ctx
 * with explicit views and does no I/O.  The singleton lives in the
 * second half.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#include "placement_gate.h"
#include "placement.h"
#include "mds_metrics.h"
#include "mds_log.h"
#include "wrr.h"

_Static_assert(PR_COUNT <= 16, "placement_rejections_total[16] must hold every reason");

/* -----------------------------------------------------------------------
 * Weight
 * ----------------------------------------------------------------------- */

/* GCC's 128-bit type, allowed under -pedantic via __extension__. */
__extension__ typedef unsigned __int128 pm_u128;

uint64_t placement_weight(uint32_t domain_weight, uint32_t ppm,
                          uint32_t n_aliases, bool *overflow)
{
    pm_u128 w;
    const pm_u128 bound = (pm_u128)1 << 62;

    if (overflow != NULL) {
        *overflow = false;
    }
    if (domain_weight == 0 || ppm == 0 || n_aliases == 0) {
        return 0;
    }
    if (domain_weight > PM_DOMAIN_WEIGHT_MAX) {
        if (overflow != NULL) {
            *overflow = true;
        }
        return 0;
    }
    w = (pm_u128)domain_weight * (pm_u128)ppm *
        (pm_u128)PM_WEIGHT_SCALE / (pm_u128)n_aliases;
    if (w == 0 || w * (pm_u128)MDS_MAX_DS_NODES >= bound) {
        if (overflow != NULL) {
            *overflow = true;
        }
        return 0;
    }
    return (uint64_t)w;
}

/* -----------------------------------------------------------------------
 * Candidates
 * ----------------------------------------------------------------------- */

static void count_reason(struct placement_reject_counts *why,
                         enum placement_reason r)
{
    if (why != NULL && (unsigned)r < PR_COUNT) {
        why->by_reason[r]++;
    }
}

static void domain_for(const struct placement_ctx *ctx, uint32_t ds_id,
                       char out[PM_DOMAIN_ID_MAX])
{
    if (ctx->domain_of != NULL && ds_id < MDS_MAX_DS_NODES &&
        ctx->domain_of[ds_id][0] != '\0') {
        (void)snprintf(out, PM_DOMAIN_ID_MAX, "%s", ctx->domain_of[ds_id]);
    } else {
        (void)snprintf(out, PM_DOMAIN_ID_MAX, "ds:%u", (unsigned)ds_id);
    }
}

static bool declared_domain(const struct placement_ctx *ctx, uint32_t ds_id)
{
    return ctx->domain_of != NULL && ds_id < MDS_MAX_DS_NODES &&
           ctx->domain_of[ds_id][0] != '\0';
}

static bool row_fresh(const struct placement_ctx *ctx,
                      const struct ds_capacity_view_row *r)
{
    if (r == NULL || r->obs.observed_mono_ms == 0 || r->obs.total_bytes == 0) {
        return false;
    }
    if (ctx->now_mono_ms < r->obs.observed_mono_ms) {
        return true; /* clock skew inside one process: treat as fresh */
    }
    return (ctx->now_mono_ms - r->obs.observed_mono_ms) <=
           (uint64_t)ctx->capacity_max_age_ms;
}

/* Per-DS scratch for one candidates() call. */
struct pc_scratch {
    const struct ds_capacity_view_row *row;   /* NULL when absent from the view */
    char     domain[PM_DOMAIN_ID_MAX];
    uint32_t n_aliases;
    bool     alias_contradiction;
    bool     alias_unmapped;
};

/* Weighted-mode candidate list (fill and, with an assessment view, smart). */
static uint32_t candidates_weighted(const struct placement_ctx *ctx,
                                    const struct mds_ds_info *ds_list, uint32_t n,
                                    struct placement_candidate *out,
                                    struct placement_reject_counts *why)
{
    struct pc_scratch *sc;
    uint32_t i, j;
    uint32_t n_out = 0;

    if (ctx->mode == PM_SMART && ctx->assess == NULL) {
        /* Stage A: smart has no assessment source yet. */
        for (i = 0; i < n; i++) {
            count_reason(why, PR_MODE_NOT_READY);
        }
        return 0;
    }
    if (ctx->cap == NULL) {
        for (i = 0; i < n; i++) {
            count_reason(why, PR_CAPACITY_UNKNOWN);
        }
        return 0;
    }

    sc = calloc(n, sizeof(*sc));
    if (sc == NULL) {
        return 0;
    }

    /* 1. Domain + row per registered DS (any state; N counts all). */
    for (i = 0; i < n; i++) {
        domain_for(ctx, ds_list[i].ds_id, sc[i].domain);
        sc[i].row = NULL;
        for (j = 0; j < ctx->cap->count; j++) {
            if (ctx->cap->rows[j].ds_id == ds_list[i].ds_id) {
                sc[i].row = &ctx->cap->rows[j];
                break;
            }
        }
    }
    for (i = 0; i < n; i++) {
        uint32_t cnt = 0;
        for (j = 0; j < n; j++) {
            if (strcmp(sc[i].domain, sc[j].domain) == 0) {
                cnt++;
            }
        }
        sc[i].n_aliases = cnt;
    }

    /* 2. Alias grades over every pair with an observed fsid. */
    for (i = 0; i < n; i++) {
        const struct ds_capacity_view_row *ri = sc[i].row;
        if (ri == NULL || ri->obs.observed_mono_ms == 0) {
            continue;
        }
        for (j = i + 1; j < n; j++) {
            const struct ds_capacity_view_row *rj = sc[j].row;
            bool same_host;
            bool same_domain;

            if (rj == NULL || rj->obs.observed_mono_ms == 0) {
                continue;
            }
            same_host = (strncmp(ri->host, rj->host, MDS_DS_HOST_MAX) == 0);
            same_domain = declared_domain(ctx, ds_list[i].ds_id) &&
                          declared_domain(ctx, ds_list[j].ds_id) &&
                          strcmp(sc[i].domain, sc[j].domain) == 0;
            if (same_domain) {
                /* (a) one declared domain, one host, two filesystems */
                if (same_host && ri->obs.fsid != rj->obs.fsid) {
                    sc[i].alias_contradiction = true;
                    sc[j].alias_contradiction = true;
                }
            } else if (ri->obs.fsid == rj->obs.fsid) {
                if (same_host) {
                    /* (b) proven alias without a shared declaration */
                    sc[i].alias_unmapped = true;
                    sc[j].alias_unmapped = true;
                } else {
                    /* (c) cannot be proven from NFS: diagnostics only */
                    placement_gate_note_alias_suspected(ds_list[i].ds_id,
                                                        ds_list[j].ds_id);
                }
            }
        }
    }

    /* 3. Per DS: online -> alias marks -> domain observation -> gate. */
    for (i = 0; i < n; i++) {
        const struct ds_capacity_view_row *canon = NULL;
        uint64_t min_avail = UINT64_MAX;
        bool any_observed = false;
        uint64_t avail;
        uint64_t total;
        uint32_t domain_weight;
        uint64_t weight;
        bool ovf = false;

        if (ds_list[i].state != DS_ONLINE) {
            count_reason(why, PR_DS_OFFLINE);
            continue;
        }
        if (sc[i].alias_contradiction) {
            count_reason(why, PR_DOMAIN_MAP_CONTRADICTION);
            continue;
        }
        if (sc[i].alias_unmapped) {
            count_reason(why, PR_SHARED_FS_ALIAS_UNMAPPED);
            continue;
        }
        /* Canonical observation of the domain: the fresh row of the
         * lowest ds_id; a fresh sibling that disagrees by more than 1 %
         * of total pulls avail down to the smaller value. */
        for (j = 0; j < n; j++) {
            const struct ds_capacity_view_row *rj = sc[j].row;
            if (strcmp(sc[i].domain, sc[j].domain) != 0) {
                continue;
            }
            if (rj != NULL && rj->obs.observed_mono_ms != 0) {
                any_observed = true;
            }
            if (!row_fresh(ctx, rj)) {
                continue;
            }
            if (canon == NULL || rj->ds_id < canon->ds_id) {
                canon = rj;
            }
            if (rj->obs.avail_bytes < min_avail) {
                min_avail = rj->obs.avail_bytes;
            }
        }
        if (canon == NULL) {
            count_reason(why, any_observed ? PR_CAPACITY_STALE
                                           : PR_CAPACITY_UNKNOWN);
            continue;
        }
        total = canon->obs.total_bytes;
        avail = canon->obs.avail_bytes;
        if (avail > min_avail && (avail - min_avail) > total / 100) {
            avail = min_avail;
        }
        if (avail > total) {
            avail = total;
        }
        if (total == 0) {
            count_reason(why, PR_CAPACITY_UNKNOWN);
            continue;
        }
        if (avail <= ctx->min_free_bytes) {
            count_reason(why, PR_CAPACITY_FULL);
            continue;
        }
        domain_weight = (uint32_t)((avail * 100ULL) / total);
        if (domain_weight == 0) {
            domain_weight = 1;
        }
        weight = placement_weight(domain_weight, 1000000u, sc[i].n_aliases, &ovf);
        if (weight == 0 || ovf) {
            count_reason(why, PR_WEIGHT_OVERFLOW);
            continue;
        }
        out[n_out].idx = i;
        out[n_out].ds_id = ds_list[i].ds_id;
        out[n_out].weight = weight;
        memcpy(out[n_out].domain, sc[i].domain, PM_DOMAIN_ID_MAX);
        n_out++;
    }

    free(sc);
    return n_out;
}

uint32_t placement_candidates(const struct placement_ctx *ctx,
                              const struct mds_ds_info *ds_list, uint32_t n,
                              struct placement_candidate *out,
                              struct placement_reject_counts *why)
{
    uint32_t i;
    uint32_t n_out = 0;

    if (why != NULL) {
        memset(why, 0, sizeof(*why));
    }
    if (ctx == NULL || ds_list == NULL || out == NULL || n == 0) {
        return 0;
    }
    if (ctx->mode == PM_FILL || ctx->mode == PM_SMART) {
        return candidates_weighted(ctx, ds_list, n, out, why);
    }
    /* rr / legacy: every ONLINE DS, weight 1, registry order. */
    for (i = 0; i < n; i++) {
        if (ds_list[i].state != DS_ONLINE) {
            count_reason(why, PR_DS_OFFLINE);
            continue;
        }
        out[n_out].idx = i;
        out[n_out].ds_id = ds_list[i].ds_id;
        out[n_out].weight = 1;
        out[n_out].domain[0] = '\0';
        n_out++;
    }
    return n_out;
}

/* -----------------------------------------------------------------------
 * Admit
 * ----------------------------------------------------------------------- */

static void set_reason(enum placement_reason *reason, enum placement_reason r)
{
    if (reason != NULL) {
        *reason = r;
    }
}

enum mds_status placement_admit(const struct placement_ctx *ctx,
                                const struct mds_ds_info *ds_list, uint32_t n,
                                uint32_t *stripe_count, uint32_t mirror_count,
                                uint64_t rr_key,
                                struct mds_ds_map_entry *entries,
                                enum placement_reason *reason)
{
    struct placement_candidate *cands;
    bool taken[MDS_MAX_DS_NODES];
    uint64_t masked[MDS_MAX_DS_NODES];
    uint32_t n_c;
    uint32_t sc;
    uint32_t need;
    uint32_t s, m;

    set_reason(reason, PR_NONE);
    if (ctx == NULL || ds_list == NULL || entries == NULL ||
        stripe_count == NULL || *stripe_count == 0 || mirror_count == 0 ||
        n == 0 || n > MDS_MAX_DS_NODES) {
        return MDS_ERR_INVAL;
    }
    sc = *stripe_count;
    memset(entries, 0, (size_t)sc * mirror_count * sizeof(*entries));

    cands = calloc(n, sizeof(*cands));
    if (cands == NULL) {
        return MDS_ERR_NOMEM;
    }
    n_c = placement_candidates(ctx, ds_list, n, cands, NULL);
    atomic_store_explicit(&g_branch_metrics.placement_eligible_ds, n_c,
                          memory_order_relaxed);
    if (n_c == 0) {
        free(cands);
        set_reason(reason, (ctx->mode == PM_SMART && ctx->assess == NULL)
                           ? PR_MODE_NOT_READY : PR_NO_ELIGIBLE_DS);
        return MDS_ERR_NOSPC;
    }
    if (n_c < mirror_count) {
        free(cands);
        set_reason(reason, PR_INSUFFICIENT_ELIGIBLE_DS);
        return MDS_ERR_NOSPC;
    }
    need = sc * mirror_count;
    if (need > n_c) {
        if (ctx->shrink == PM_SHRINK_STRICT) {
            free(cands);
            set_reason(reason, PR_INSUFFICIENT_ELIGIBLE_DS);
            return MDS_ERR_NOSPC;
        }
        sc = n_c / mirror_count;
        if (sc == 0) {
            sc = 1;
        }
        atomic_fetch_add_explicit(&g_branch_metrics.placement_degraded_total,
                                  1, memory_order_relaxed);
    }

    if (ctx->mode == PM_RR || ctx->mode == PM_LEGACY) {
        uint32_t start;

        if (rr_key != 0) {
            start = (uint32_t)(rr_key % (uint64_t)n_c);
        } else if (ctx->rr_counter != NULL) {
            start = atomic_fetch_add_explicit(ctx->rr_counter, sc * mirror_count,
                                              memory_order_relaxed);
        } else {
            start = 0;
        }
        for (s = 0; s < sc; s++) {
            for (m = 0; m < mirror_count; m++) {
                uint32_t k = (start + s * mirror_count + m) % n_c;
                entries[s * mirror_count + m].ds_id = cands[k].ds_id;
            }
        }
        free(cands);
        *stripe_count = sc;
        return MDS_OK;
    }

    /* fill / smart: weighted heads, distinct DS per layout. */
    memset(taken, 0, sizeof(taken));
    for (s = 0; s < sc; s++) {
        uint32_t head;
        uint32_t i;
        uint32_t placed_mirrors;

        for (i = 0; i < n_c; i++) {
            masked[i] = taken[i] ? 0 : cands[i].weight;
        }
        if (mds_wrr_weighted_pick2(masked, n_c, &head) != 0) {
            break; /* nothing positive left */
        }
        taken[head] = true;
        entries[s * mirror_count].ds_id = cands[head].ds_id;
        placed_mirrors = 1;
        for (i = 1; i < n_c && placed_mirrors < mirror_count; i++) {
            uint32_t k = (head + i) % n_c;
            if (!taken[k]) {
                taken[k] = true;
                entries[s * mirror_count + placed_mirrors].ds_id = cands[k].ds_id;
                placed_mirrors++;
            }
        }
        if (placed_mirrors < mirror_count) {
            /* undo this stripe: not enough distinct DS for its mirrors */
            for (i = 0; i < placed_mirrors; i++) {
                entries[s * mirror_count + i].ds_id = 0;
            }
            break;
        }
    }
    free(cands);
    if (s == 0) {
        memset(entries, 0, (size_t)(*stripe_count) * mirror_count * sizeof(*entries));
        set_reason(reason, PR_INSUFFICIENT_ELIGIBLE_DS);
        return MDS_ERR_NOSPC;
    }
    if (s < sc) {
        if (ctx->shrink == PM_SHRINK_STRICT) {
            memset(entries, 0, (size_t)(*stripe_count) * mirror_count * sizeof(*entries));
            set_reason(reason, PR_INSUFFICIENT_ELIGIBLE_DS);
            return MDS_ERR_NOSPC;
        }
        atomic_fetch_add_explicit(&g_branch_metrics.placement_degraded_total,
                                  1, memory_order_relaxed);
        sc = s;
    }
    *stripe_count = sc;
    return MDS_OK;
}

bool placement_ds_admitted(const struct placement_ctx *ctx,
                           const struct mds_ds_info *ds_list, uint32_t n,
                           uint32_t ds_id, enum placement_reason *reason)
{
    struct placement_candidate *cands;
    struct placement_reject_counts why;
    uint32_t n_c;
    uint32_t i;
    bool found = false;

    set_reason(reason, PR_DS_OFFLINE);
    if (ctx == NULL || ds_list == NULL || n == 0) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (ds_list[i].ds_id == ds_id) {
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    if (ctx->mode == PM_RR || ctx->mode == PM_LEGACY) {
        if (ds_list[i].state == DS_ONLINE) {
            set_reason(reason, PR_NONE);
            return true;
        }
        return false;
    }
    cands = calloc(n, sizeof(*cands));
    if (cands == NULL) {
        return false;
    }
    n_c = placement_candidates(ctx, ds_list, n, cands, &why);
    for (i = 0; i < n_c; i++) {
        if (cands[i].ds_id == ds_id) {
            free(cands);
            set_reason(reason, PR_NONE);
            return true;
        }
    }
    free(cands);
    /* Not a candidate: report the DS's own verdict by re-running the
     * gate on a one-element list (cheap, and exact for the reason). */
    {
        struct placement_candidate one;
        struct placement_reject_counts w1;
        uint32_t k;

        for (k = 0; k < n; k++) {
            if (ds_list[k].ds_id == ds_id) {
                break;
            }
        }
        (void)placement_candidates(ctx, &ds_list[k], 1, &one, &w1);
        for (k = 1; k < PR_COUNT; k++) {
            if (w1.by_reason[k] != 0) {
                set_reason(reason, (enum placement_reason)k);
                return false;
            }
        }
        /* Excluded only in the context of the full list (an alias
         * grade): report the list-level reason. */
        for (k = 1; k < PR_COUNT; k++) {
            if (why.by_reason[k] != 0 &&
                (k == PR_DOMAIN_MAP_CONTRADICTION ||
                 k == PR_SHARED_FS_ALIAS_UNMAPPED)) {
                set_reason(reason, (enum placement_reason)k);
                return false;
            }
        }
    }
    return false;
}

/* Alias diagnostics: a counter plus a rate-limited WARN (one per pair per 60 s). */
void placement_gate_note_alias_suspected(uint32_t a, uint32_t b)
{
    static _Atomic uint64_t last_warn_ms;
    uint64_t now = ds_cache_mono_ms();
    uint64_t last = atomic_load_explicit(&last_warn_ms, memory_order_relaxed);

    atomic_fetch_add_explicit(&g_branch_metrics.placement_alias_suspected_total,
                              1, memory_order_relaxed);
    if (now - last >= 60000ULL &&
        atomic_compare_exchange_strong(&last_warn_ms, &last, now)) {
        MDS_LOG_WARN(LOG_COMP_FSAL,
            "placement: ds %u and ds %u report the same filesystem id "
            "behind different host names; declare ds_capacity_domain for "
            "both if they share a filesystem (ALIAS_SUSPECTED)",
            (unsigned)a, (unsigned)b);
    }
}

/* =======================================================================
 * Process singleton: effective mode + published capacity view + tokens
 * ======================================================================= */

#include <pthread.h>
#include "ds_cache.h"

struct cap_view_ref {
    _Atomic uint32_t refs;
    struct placement_capacity_view view;
};

static struct {
    bool                  initialised;
    enum placement_mode   mode;
    enum placement_shrink shrink;
    uint32_t              max_age_ms;
    uint64_t              min_free;
    char                  domain_of[MDS_MAX_DS_NODES][PM_DOMAIN_ID_MAX];
    char                  generation[65];
    struct ds_cache      *cache;
    pthread_rwlock_t      lock;     /* guards cur */
    struct cap_view_ref  *cur;
    _Atomic uint32_t      rr_counter;
    _Atomic uint32_t      snapshot_gen;
} g;

static void view_unref(struct cap_view_ref *ref)
{
    if (ref != NULL &&
        atomic_fetch_sub_explicit(&ref->refs, 1, memory_order_acq_rel) == 1) {
        free(ref);
    }
}

enum placement_mode placement_gate_mode(void)
{
    return g.initialised ? g.mode : PM_LEGACY;
}

const char *placement_gate_generation(void)
{
    return g.initialised ? g.generation : "";
}

void placement_gate_publish_capacity(void)
{
    struct cap_view_ref *fresh;
    struct cap_view_ref *old;

    if (!g.initialised || g.cache == NULL) {
        return;
    }
    fresh = calloc(1, sizeof(*fresh));
    if (fresh == NULL) {
        return;
    }
    atomic_store_explicit(&fresh->refs, 1, memory_order_relaxed);
    fresh->view.count = ds_cache_capacity_view(g.cache, fresh->view.rows,
                                               MDS_MAX_DS_NODES);
    pthread_rwlock_wrlock(&g.lock);
    old = g.cur;
    g.cur = fresh;
    pthread_rwlock_unlock(&g.lock);
    atomic_fetch_add_explicit(&g.snapshot_gen, 1, memory_order_release);
    view_unref(old);
}

void placement_gate_ctx(struct placement_ctx *out, uint64_t now_mono_ms)
{
    struct cap_view_ref *ref = NULL;

    memset(out, 0, sizeof(*out));
    out->now_mono_ms = now_mono_ms;
    if (!g.initialised) {
        out->mode = PM_LEGACY;
        return;
    }
    out->mode = g.mode;
    out->shrink = g.shrink;
    out->capacity_max_age_ms = g.max_age_ms;
    out->min_free_bytes = g.min_free;
    out->domain_of = (const char (*)[PM_DOMAIN_ID_MAX])g.domain_of;
    out->rr_counter = &g.rr_counter;
    if (g.mode == PM_FILL || g.mode == PM_SMART) {
        pthread_rwlock_rdlock(&g.lock);
        ref = g.cur;
        if (ref != NULL) {
            atomic_fetch_add_explicit(&ref->refs, 1, memory_order_acq_rel);
        }
        pthread_rwlock_unlock(&g.lock);
        out->cap = (ref != NULL) ? &ref->view : NULL;
        out->view_ref = ref;
    }
}

void placement_gate_ctx_release(struct placement_ctx *ctx)
{
    if (ctx == NULL || ctx->view_ref == NULL) {
        return;
    }
    view_unref((struct cap_view_ref *)ctx->view_ref);
    ctx->view_ref = NULL;
    ctx->cap = NULL;
}

int placement_gate_init(const struct mds_config *cfg, struct ds_cache *cache)
{
    if (cfg == NULL || !cfg->placement_mode_set) {
        return -1;
    }
    if (g.initialised) {
        placement_gate_destroy();
    }
    memset(&g, 0, sizeof(g));
    g.mode = cfg->placement_mode;
    g.shrink = cfg->placement_stripe_shrink;
    g.max_age_ms = cfg->placement_capacity_max_age_ms;
    g.min_free = cfg->placement_min_free_bytes;
    memcpy(g.domain_of, cfg->ds_capacity_domain, sizeof(g.domain_of));
    memcpy(g.generation, cfg->placement_config_generation, sizeof(g.generation));
    g.generation[64] = '\0';
    g.cache = cache;
    if ((g.mode == PM_FILL || g.mode == PM_SMART) && mds_wrr_kernel_id() == 0) {
        MDS_LOG_ERROR(LOG_COMP_FSAL,
            "placement: mode %s needs the XinnorLab wrr kernel; this binary "
            "carries the community stub (kernel id 0)",
            placement_mode_name(g.mode));
        return -1;
    }
    if ((g.mode == PM_FILL || g.mode == PM_SMART) && cache == NULL) {
        MDS_LOG_ERROR(LOG_COMP_FSAL,
            "placement: mode %s needs the DS cache (capacity observations)",
            placement_mode_name(g.mode));
        return -1;
    }
    if (pthread_rwlock_init(&g.lock, NULL) != 0) {
        return -1;
    }
    g.initialised = true;
    atomic_store_explicit(&g_branch_metrics.placement_mode_gauge,
                          (uint64_t)g.mode, memory_order_relaxed);
    placement_gate_publish_capacity();
    MDS_LOG_INFO(LOG_COMP_FSAL,
        "placement_mode=%s generation=%.12s kernel=%08x shrink=%s "
        "max_age_ms=%u min_free=%llu",
        placement_mode_name(g.mode), g.generation,
        (unsigned)mds_wrr_kernel_id(), placement_shrink_name(g.shrink),
        (unsigned)g.max_age_ms, (unsigned long long)g.min_free);
    return 0;
}

void placement_gate_destroy(void)
{
    struct cap_view_ref *old;

    if (!g.initialised) {
        return;
    }
    pthread_rwlock_wrlock(&g.lock);
    old = g.cur;
    g.cur = NULL;
    pthread_rwlock_unlock(&g.lock);
    view_unref(old);
    pthread_rwlock_destroy(&g.lock);
    g.initialised = false;
    g.cache = NULL;
    atomic_store_explicit(&g_branch_metrics.placement_mode_gauge, 0,
                          memory_order_relaxed);
}

/* -----------------------------------------------------------------------
 * Site helper
 * ----------------------------------------------------------------------- */

static void note_refusal(enum placement_reason why)
{
    static _Atomic uint64_t last_warn_ms[PR_COUNT];
    uint64_t now;
    uint64_t last;

    if ((unsigned)why >= PR_COUNT) {
        return;
    }
    atomic_fetch_add_explicit(&g_branch_metrics.placement_rejections_total[why],
                              1, memory_order_relaxed);
    now = ds_cache_mono_ms();
    last = atomic_load_explicit(&last_warn_ms[why], memory_order_relaxed);
    if (now - last >= 10000ULL &&
        atomic_compare_exchange_strong(&last_warn_ms[why], &last, now)) {
        MDS_LOG_WARN(LOG_COMP_FSAL, "placement refused: %s (mode %s)",
                     placement_reason_name(why),
                     placement_mode_name(placement_gate_mode()));
    }
}

enum mds_status placement_select_gated(bool legacy_policy_enabled,
                                       enum mds_placement_policy legacy_policy,
                                       const struct mds_ds_info *ds_list, uint32_t n,
                                       uint32_t *stripe_count, uint32_t mirror_count,
                                       uint32_t stripe_unit, uint64_t rr_key,
                                       struct mds_ds_map_entry *entries,
                                       enum placement_reason *reason)
{
    struct placement_ctx ctx;
    enum mds_status st;
    enum placement_reason why = PR_NONE;

    if (reason != NULL) {
        *reason = PR_NONE;
    }
    if (placement_gate_mode() == PM_LEGACY) {
        if (!legacy_policy_enabled && rr_key != 0) {
            return placement_select_rr_at2(ds_list, n, stripe_count, mirror_count,
                                           stripe_unit, rr_key, entries);
        }
        if (legacy_policy_enabled) {
            return placement_select_ex2(legacy_policy, ds_list, n, stripe_count,
                                        mirror_count, stripe_unit, entries);
        }
        return placement_select2(ds_list, n, stripe_count, mirror_count,
                                 stripe_unit, entries);
    }
    (void)stripe_unit;
    placement_gate_ctx(&ctx, ds_cache_mono_ms());
    st = placement_admit(&ctx, ds_list, n, stripe_count, mirror_count, rr_key,
                         entries, &why);
    placement_gate_ctx_release(&ctx);
    if (st == MDS_ERR_NOSPC) {
        note_refusal(why);
    }
    if (reason != NULL) {
        *reason = why;
    }
    return st;
}
