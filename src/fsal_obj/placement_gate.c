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
        /* A monotonic clock cannot run backwards inside one process;
         * an observation "from the future" is corrupt data -- fail
         * closed (not fresh) rather than trust it. */
        return false;
    }
    return (ctx->now_mono_ms - r->obs.observed_mono_ms) <=
           (uint64_t)ctx->capacity_max_age_ms;
}

/*
 * Weighted-mode candidate list (fill and, with an assessment view, smart).
 *
 * The caller's ds_list is only the NATIVE-eligible subset (ONLINE, profile,
 * io-limit).  Every registry-level fact -- a domain's alias count N, the
 * alias grades, the canonical observation -- comes from the published
 * view, which holds every registered DS whatever the caller filtered, so
 * a filtered list can neither inflate a domain's share nor hide an alias
 * (MODE-07, review finding 1).
 */
struct row_scratch {
    char domain[PM_DOMAIN_ID_MAX];
    bool declared;
    bool observed;   /* has an fsid and a total: usable for alias checks */
};

static uint32_t domain_weight_of(uint64_t avail, uint64_t total)
{
    uint64_t w;

    if (total == 0) {
        return 0;
    }
    if (avail > total) {
        avail = total;
    }
    /* Same guard as ds_capacity_derive_auto_weight: > 184 EiB totals. */
    if (total > UINT64_MAX / 100ULL) {
        w = avail / (total / 100ULL);
    } else {
        w = (avail * 100ULL) / total;
    }
    if (w == 0) {
        w = 1;
    }
    if (w > 100) {
        w = 100;
    }
    return (uint32_t)w;
}

static uint32_t candidates_weighted(const struct placement_ctx *ctx,
                                    const struct mds_ds_info *ds_list, uint32_t n,
                                    struct placement_candidate *out,
                                    struct placement_reject_counts *why)
{
    const struct placement_capacity_view *cap = ctx->cap;
    struct row_scratch *rs;
    uint32_t i, j;
    uint32_t n_out = 0;

    if (ctx->mode == PM_SMART && ctx->assess == NULL) {
        /* Stage A: smart has no assessment source yet. */
        for (i = 0; i < n; i++) {
            count_reason(why, PR_MODE_NOT_READY);
        }
        return 0;
    }
    if (cap == NULL || cap->count == 0) {
        for (i = 0; i < n; i++) {
            count_reason(why, PR_CAPACITY_UNKNOWN);
        }
        return 0;
    }

    rs = calloc(cap->count, sizeof(*rs));
    if (rs == NULL) {
        return 0;
    }
    for (j = 0; j < cap->count; j++) {
        domain_for(ctx, cap->rows[j].ds_id, rs[j].domain);
        rs[j].declared = declared_domain(ctx, cap->rows[j].ds_id);
        rs[j].observed = (cap->rows[j].obs.observed_mono_ms != 0 &&
                          cap->rows[j].obs.total_bytes != 0);
    }

    for (i = 0; i < n; i++) {
        const struct ds_capacity_view_row *ri = NULL;
        uint32_t r_i = 0;
        bool contradiction = false;
        bool unmapped = false;
        const struct ds_capacity_view_row *canon = NULL;
        uint64_t min_avail = UINT64_MAX;
        bool any_observed = false;
        uint32_t n_aliases = 0;
        uint64_t avail;
        uint64_t total;
        uint32_t domain_weight;
        uint64_t weight;
        bool ovf = false;

        if (ds_list[i].state != DS_ONLINE) {
            count_reason(why, PR_DS_OFFLINE);
            continue;
        }
        for (j = 0; j < cap->count; j++) {
            if (cap->rows[j].ds_id == ds_list[i].ds_id) {
                ri = &cap->rows[j];
                r_i = j;
                break;
            }
        }
        if (ri == NULL) {
            count_reason(why, PR_CAPACITY_UNKNOWN);   /* not in the registry view */
            continue;
        }

        /* Alias grades: this DS against every other registered DS. */
        if (rs[r_i].observed) {
            for (j = 0; j < cap->count; j++) {
                const struct ds_capacity_view_row *rj = &cap->rows[j];
                bool same_host;
                bool same_domain;

                if (j == r_i || !rs[j].observed) {
                    continue;
                }
                same_host = (strncmp(ri->host, rj->host, MDS_DS_HOST_MAX) == 0);
                same_domain = rs[r_i].declared && rs[j].declared &&
                              strcmp(rs[r_i].domain, rs[j].domain) == 0;
                if (same_domain) {
                    /* (a) one declared domain, one host, two filesystems */
                    if (same_host && ri->obs.fsid != rj->obs.fsid) {
                        contradiction = true;
                    }
                } else if (ri->obs.fsid == rj->obs.fsid) {
                    if (same_host) {
                        unmapped = true;          /* (b) proven, undeclared */
                    } else if (ri->ds_id < rj->ds_id) {
                        placement_gate_note_alias_suspected(ri->ds_id, rj->ds_id);   /* (c) */
                    }
                }
            }
        }
        if (contradiction) {
            count_reason(why, PR_DOMAIN_MAP_CONTRADICTION);
            continue;
        }
        if (unmapped) {
            count_reason(why, PR_SHARED_FS_ALIAS_UNMAPPED);
            continue;
        }

        /* Domain aggregation over ALL registered members (any state): N,
         * the canonical (lowest id, fresh) observation, the conservative
         * minimum when fresh siblings disagree by more than 1 %. */
        for (j = 0; j < cap->count; j++) {
            const struct ds_capacity_view_row *rj = &cap->rows[j];

            if (strcmp(rs[r_i].domain, rs[j].domain) != 0) {
                continue;
            }
            n_aliases++;
            if (rs[j].observed) {
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
        if (avail <= ctx->min_free_bytes) {
            count_reason(why, PR_CAPACITY_FULL);
            continue;
        }
        domain_weight = domain_weight_of(avail, total);
        weight = placement_weight(domain_weight, 1000000u, n_aliases, &ovf);
        if (weight == 0 || ovf) {
            count_reason(why, PR_WEIGHT_OVERFLOW);
            continue;
        }
        out[n_out].idx = i;
        out[n_out].ds_id = ds_list[i].ds_id;
        out[n_out].weight = weight;
        memcpy(out[n_out].domain, rs[r_i].domain, PM_DOMAIN_ID_MAX);
        n_out++;
    }

    free(rs);
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
    {
        struct placement_reject_counts why;

        n_c = placement_candidates(ctx, ds_list, n, cands, &why);
        placement_gate_note_rejections(&why);
    }
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
    struct placement_candidate one;
    struct placement_reject_counts why;
    uint32_t k;

    set_reason(reason, PR_DS_OFFLINE);
    if (ctx == NULL || ds_list == NULL || n == 0) {
        return false;
    }
    for (k = 0; k < n; k++) {
        if (ds_list[k].ds_id == ds_id) {
            break;
        }
    }
    if (k == n) {
        return false;
    }
    if (ctx->mode == PM_RR || ctx->mode == PM_LEGACY) {
        if (ds_list[k].state == DS_ONLINE) {
            set_reason(reason, PR_NONE);
            return true;
        }
        return false;
    }
    /* The registry-level facts come from the view, so a one-element list
     * yields exactly the verdict the DS would get inside the full list. */
    if (placement_candidates(ctx, &ds_list[k], 1, &one, &why) == 1) {
        set_reason(reason, PR_NONE);
        return true;
    }
    for (k = 1; k < PR_COUNT; k++) {
        if (why.by_reason[k] != 0) {
            set_reason(reason, (enum placement_reason)k);
            return false;
        }
    }
    set_reason(reason, PR_CAPACITY_UNKNOWN);
    return false;
}

/*
 * Per-DS rejection counts into the metrics, plus a rate-limited ERROR for
 * the two alias grades that need an operator (review finding 3).
 */
void placement_gate_note_rejections(const struct placement_reject_counts *why)
{
    static _Atomic uint64_t last_alias_err_ms;
    uint32_t r;

    if (why == NULL) {
        return;
    }
    for (r = 1; r < PR_COUNT; r++) {
        if (why->by_reason[r] != 0) {
            atomic_fetch_add_explicit(&g_branch_metrics.placement_rejections_total[r],
                                      why->by_reason[r], memory_order_relaxed);
        }
    }
    if (why->by_reason[PR_DOMAIN_MAP_CONTRADICTION] != 0 ||
        why->by_reason[PR_SHARED_FS_ALIAS_UNMAPPED] != 0) {
        uint64_t now = ds_cache_mono_ms();
        uint64_t last = atomic_load_explicit(&last_alias_err_ms, memory_order_relaxed);

        if (now - last >= 60000ULL &&
            atomic_compare_exchange_strong(&last_alias_err_ms, &last, now)) {
            MDS_LOG_ERROR(LOG_COMP_FSAL,
                "placement: %u DS excluded as DOMAIN_MAP_CONTRADICTION and %u as "
                "SHARED_FS_ALIAS_UNMAPPED -- declare ds_capacity_domain.<id> for "
                "every export of a shared filesystem (config show placement_ds.<id>)",
                (unsigned)why->by_reason[PR_DOMAIN_MAP_CONTRADICTION],
                (unsigned)why->by_reason[PR_SHARED_FS_ALIAS_UNMAPPED]);
        }
    }
}

/* Alias diagnostics: a counter plus a rate-limited WARN (one per pair per 60 s). */
void placement_gate_note_alias_suspected(uint32_t a, uint32_t b)
{
    static _Atomic uint64_t last_warn_ms;
    uint64_t now = ds_cache_mono_ms();
    uint64_t last = atomic_load_explicit(&last_warn_ms, memory_order_relaxed);

    if (now - last >= 60000ULL &&
        atomic_compare_exchange_strong(&last_warn_ms, &last, now)) {
        atomic_fetch_add_explicit(&g_branch_metrics.placement_alias_suspected_total,
                                  1, memory_order_relaxed);
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
    /* Compact registry (ds_id, state, host) derived from the rows so the
     * create boundary can run the same gate without a catalogue read. */
    struct mds_ds_info *infos;
    uint32_t            n_infos;
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
    pthread_rwlock_t      lock;     /* guards cur and cur_assess */
    struct cap_view_ref  *cur;
    struct assess_view_ref *cur_assess;
    struct placement_connector_facts conn;
    uint32_t              poll_ms;
    char                  domain_weight_id[PM_MAX_DOMAINS][PM_DOMAIN_ID_MAX];
    uint32_t              domain_weight[PM_MAX_DOMAINS];
    uint32_t              domain_weight_count;
    bool                  allow_manual_weights;
    _Atomic uint32_t      rr_counter;
    _Atomic uint32_t      snapshot_gen;
} g;

struct assess_view_ref {
    _Atomic uint32_t refs;
    struct placement_assessment_view view;
};

static void assess_unref(struct assess_view_ref *ref)
{
    if (ref != NULL &&
        atomic_fetch_sub_explicit(&ref->refs, 1, memory_order_acq_rel) == 1) {
        free(ref);
    }
}

static void view_unref(struct cap_view_ref *ref)
{
    if (ref != NULL &&
        atomic_fetch_sub_explicit(&ref->refs, 1, memory_order_acq_rel) == 1) {
        free(ref->infos);
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
    if (fresh->view.count > 0) {
        fresh->infos = calloc(fresh->view.count, sizeof(*fresh->infos));
        if (fresh->infos == NULL) {
            free(fresh);
            return;
        }
        for (uint32_t i = 0; i < fresh->view.count; i++) {
            fresh->infos[i].ds_id = fresh->view.rows[i].ds_id;
            fresh->infos[i].state = fresh->view.rows[i].state;
            memcpy(fresh->infos[i].host, fresh->view.rows[i].host,
                   sizeof(fresh->infos[i].host));
        }
        fresh->n_infos = fresh->view.count;
    }
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
    if (g.allow_manual_weights && g.domain_weight_count > 0) {
        out->domain_weight_id = (const char (*)[PM_DOMAIN_ID_MAX])g.domain_weight_id;
        out->domain_weight = g.domain_weight;
        out->domain_weight_count = g.domain_weight_count;
    }
    if (g.mode == PM_FILL || g.mode == PM_SMART) {
        struct assess_view_ref *aref = NULL;

        pthread_rwlock_rdlock(&g.lock);
        ref = g.cur;
        if (ref != NULL) {
            atomic_fetch_add_explicit(&ref->refs, 1, memory_order_acq_rel);
        }
        if (g.mode == PM_SMART) {
            aref = g.cur_assess;
            if (aref != NULL) {
                atomic_fetch_add_explicit(&aref->refs, 1, memory_order_acq_rel);
            }
        }
        pthread_rwlock_unlock(&g.lock);
        out->cap = (ref != NULL) ? &ref->view : NULL;
        out->view_ref = ref;
        out->assess = (aref != NULL) ? &aref->view : NULL;
        out->assess_ref = aref;
    }
}

void placement_gate_ctx_release(struct placement_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->view_ref != NULL) {
        view_unref((struct cap_view_ref *)ctx->view_ref);
        ctx->view_ref = NULL;
        ctx->cap = NULL;
    }
    if (ctx->assess_ref != NULL) {
        assess_unref((struct assess_view_ref *)ctx->assess_ref);
        ctx->assess_ref = NULL;
        ctx->assess = NULL;
    }
}

void placement_gate_publish_assessments(const struct placement_assessment_view *view)
{
    struct assess_view_ref *fresh;
    struct assess_view_ref *old;

    if (!g.initialised || view == NULL) {
        return;
    }
    fresh = calloc(1, sizeof(*fresh));
    if (fresh == NULL) {
        return;
    }
    atomic_store_explicit(&fresh->refs, 1, memory_order_relaxed);
    fresh->view = *view;
    pthread_rwlock_wrlock(&g.lock);
    old = g.cur_assess;
    g.cur_assess = fresh;
    pthread_rwlock_unlock(&g.lock);
    atomic_fetch_add_explicit(&g.snapshot_gen, 1, memory_order_release);
    assess_unref(old);
}

void placement_gate_set_connector_facts(const struct placement_connector_facts *f)
{
    if (!g.initialised || f == NULL) {
        return;
    }
    pthread_rwlock_wrlock(&g.lock);
    g.conn = *f;
    pthread_rwlock_unlock(&g.lock);
}

void placement_gate_readiness(struct placement_readiness *out)
{
    struct placement_ctx ctx;
    uint64_t now = ds_cache_mono_ms();

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    (void)snprintf(out->coverage, sizeof(out->coverage), "n/a");
    if (!g.initialised || g.mode != PM_SMART) {
        return;
    }
    out->mode_active = true;
    pthread_rwlock_rdlock(&g.lock);
    out->connector_config_valid = g.conn.config_valid;
    out->connector_reachable = g.conn.reachable &&
        (g.conn.last_success_mono_ms != 0) &&
        (now - g.conn.last_success_mono_ms) <= 3ULL * g.poll_ms;
    out->last_batch_valid = g.conn.last_batch_valid;
    out->last_success_mono_ms = g.conn.last_success_mono_ms;
    memcpy(out->last_detail, g.conn.last_detail, sizeof(out->last_detail));
    pthread_rwlock_unlock(&g.lock);
    placement_gate_ctx(&ctx, now);
    if (ctx.cap != NULL) {
        out->registered_ds = ctx.cap->count;
    }
    if (ctx.assess != NULL) {
        uint32_t i;

        memcpy(out->config_digest, ctx.assess->config_digest, sizeof(out->config_digest));
        memcpy(out->profile_digest, ctx.assess->profile_digest, sizeof(out->profile_digest));
        for (i = 0; i < ctx.assess->count; i++) {
            const struct placement_assessment_row *r = &ctx.assess->rows[i];

            if (!r->present || !r->valid || now >= r->expires_mono_ms) {
                continue;
            }
            out->covered_ds++;
            if (r->allowed && r->multiplier_ppm > 0) {
                out->eligible_ds++;
            }
        }
    }
    placement_gate_ctx_release(&ctx);
    if (out->registered_ds == 0 || out->covered_ds == 0) {
        (void)snprintf(out->coverage, sizeof(out->coverage), "none");
    } else if (out->covered_ds >= out->registered_ds) {
        (void)snprintf(out->coverage, sizeof(out->coverage), "full");
    } else {
        (void)snprintf(out->coverage, sizeof(out->coverage), "partial");
    }
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
    g.poll_ms = cfg->ds_connector_poll_ms ? cfg->ds_connector_poll_ms : 1000;
    g.allow_manual_weights = cfg->placement_allow_manual_base_weights;
    g.domain_weight_count = cfg->placement_domain_weight_count;
    if (g.domain_weight_count > PM_MAX_DOMAINS) {
        g.domain_weight_count = PM_MAX_DOMAINS;
    }
    memcpy(g.domain_weight_id, cfg->placement_domain_weight_id, sizeof(g.domain_weight_id));
    memcpy(g.domain_weight, cfg->placement_domain_weight, sizeof(g.domain_weight));
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
    {
        struct assess_view_ref *aold;

        pthread_rwlock_wrlock(&g.lock);
        old = g.cur;
        g.cur = NULL;
        aold = g.cur_assess;
        g.cur_assess = NULL;
        pthread_rwlock_unlock(&g.lock);
        view_unref(old);
        assess_unref(aold);
    }
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

/* -----------------------------------------------------------------------
 * Create-boundary admission (design section 5a)
 * ----------------------------------------------------------------------- */

bool placement_token_valid(const struct placement_token *tok, uint32_t ds_id,
                           uint64_t now_mono_ms)
{
    if (tok == NULL || tok->ds_id != ds_id) {
        return false;
    }
    if (tok->purpose != PP_NEW_OBJECT && tok->purpose != PP_RECREATE_MISSING) {
        return false;
    }
    if (now_mono_ms < tok->minted_mono_ms) {
        return false;
    }
    if (g.initialised) {
        uint32_t cur = atomic_load_explicit(&g.snapshot_gen, memory_order_acquire);

        /* One publish may race the create; two mean the token predates the
         * previous snapshot -- the caller re-admits. */
        if (cur - tok->snapshot_gen > 1) {
            return false;
        }
    }
    return (now_mono_ms - tok->minted_mono_ms) <= PLACEMENT_TOKEN_MAX_AGE_MS;
}

static void mint_token(struct placement_token *tok, uint32_t ds_id,
                       enum placement_purpose p, uint64_t now)
{
    tok->ds_id = ds_id;
    tok->purpose = (uint32_t)p;
    tok->minted_mono_ms = now;
    tok->snapshot_gen = atomic_load_explicit(&g.snapshot_gen, memory_order_acquire);
}

enum mds_status placement_gate_admit_create(uint32_t ds_id, enum placement_purpose p,
                                            struct placement_token *tok,
                                            enum placement_reason *reason)
{
    struct placement_ctx ctx;
    struct cap_view_ref *ref;
    enum placement_reason why = PR_NONE;
    uint64_t now = ds_cache_mono_ms();
    bool ok;

    set_reason(reason, PR_NONE);
    if (tok == NULL) {
        return MDS_ERR_INVAL;
    }
    memset(tok, 0, sizeof(*tok));
    if (!g.initialised) {
        /* Legacy: the upstream helpers created without any check. */
        mint_token(tok, ds_id, p, now);
        return MDS_OK;
    }
    if (g.mode == PM_RR) {
        if (g.cache != NULL && !ds_cache_is_online(g.cache, ds_id)) {
            note_refusal(PR_DS_OFFLINE);
            set_reason(reason, PR_DS_OFFLINE);
            return MDS_ERR_NOSPC;
        }
        mint_token(tok, ds_id, p, now);
        return MDS_OK;
    }
    if (g.cache != NULL && !ds_cache_is_online(g.cache, ds_id)) {
        /* Admin state is read live: a DS set OFFLINE between two
         * publishes must not receive a new object (review finding 2). */
        note_refusal(PR_DS_OFFLINE);
        set_reason(reason, PR_DS_OFFLINE);
        return MDS_ERR_NOSPC;
    }
    placement_gate_ctx(&ctx, now);
    ref = (struct cap_view_ref *)ctx.view_ref;
    if (ref == NULL || ref->n_infos == 0) {
        placement_gate_ctx_release(&ctx);
        note_refusal(PR_CAPACITY_UNKNOWN);
        set_reason(reason, PR_CAPACITY_UNKNOWN);
        return MDS_ERR_NOSPC;
    }
    ok = placement_ds_admitted(&ctx, ref->infos, ref->n_infos, ds_id, &why);
    placement_gate_ctx_release(&ctx);
    if (!ok) {
        note_refusal(why);
        set_reason(reason, why);
        return MDS_ERR_NOSPC;
    }
    mint_token(tok, ds_id, p, now);
    return MDS_OK;
}

/* -----------------------------------------------------------------------
 * config show support (design section 9)
 * ----------------------------------------------------------------------- */

uint32_t placement_gate_ds_ids(uint32_t *ids, uint32_t cap)
{
    struct placement_ctx ctx;
    struct cap_view_ref *ref;
    uint32_t n = 0;

    if (ids == NULL || cap == 0) {
        return 0;
    }
    placement_gate_ctx(&ctx, ds_cache_mono_ms());
    ref = (struct cap_view_ref *)ctx.view_ref;
    if (ref != NULL) {
        for (uint32_t i = 0; i < ref->view.count && n < cap; i++) {
            ids[n++] = ref->view.rows[i].ds_id;
        }
    }
    placement_gate_ctx_release(&ctx);
    return n;
}

bool placement_gate_ds_status(uint32_t ds_id, struct placement_ds_status *out)
{
    struct placement_ctx ctx;
    struct cap_view_ref *ref;
    struct placement_candidate *cands = NULL;
    uint32_t n_c = 0;
    bool found = false;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->capacity_age_ms = UINT64_MAX;
    out->reason = PR_DS_OFFLINE;
    placement_gate_ctx(&ctx, ds_cache_mono_ms());
    ref = (struct cap_view_ref *)ctx.view_ref;
    if (ref == NULL || ref->n_infos == 0) {
        placement_gate_ctx_release(&ctx);
        return false;
    }
    for (uint32_t i = 0; i < ref->view.count; i++) {
        const struct ds_capacity_view_row *r = &ref->view.rows[i];
        if (r->ds_id != ds_id) {
            continue;
        }
        found = true;
        out->registered = true;
        out->state = r->state;
        out->avail_bytes = r->obs.avail_bytes;
        out->total_bytes = r->obs.total_bytes;
        if (r->obs.observed_mono_ms != 0 &&
            ctx.now_mono_ms >= r->obs.observed_mono_ms) {
            out->capacity_age_ms = ctx.now_mono_ms - r->obs.observed_mono_ms;
        }
        break;
    }
    if (!found) {
        placement_gate_ctx_release(&ctx);
        return false;
    }
    domain_for(&ctx, ds_id, out->domain);
    cands = calloc(ref->n_infos, sizeof(*cands));
    if (cands != NULL) {
        n_c = placement_candidates(&ctx, ref->infos, ref->n_infos, cands, NULL);
        for (uint32_t i = 0; i < n_c; i++) {
            if (cands[i].ds_id == ds_id) {
                out->weight = cands[i].weight;
                out->reason = PR_NONE;
                break;
            }
        }
        if (out->reason != PR_NONE) {
            (void)placement_ds_admitted(&ctx, ref->infos, ref->n_infos, ds_id,
                                        &out->reason);
        }
        free(cands);
    }
    placement_gate_ctx_release(&ctx);
    return true;
}
