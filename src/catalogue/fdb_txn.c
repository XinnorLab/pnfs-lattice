/*
 * Copyright (c) 2026 PeakAIO
 * SPDX-License-Identifier: MIT
 *
 * fdb_txn.c -- FoundationDB transaction runner (see fdb_txn.h).
 *
 * Structure:
 *   - the real fdb_c call table and the body helpers;
 *   - the process-wide witness slot pool (one slot per thread for the
 *     thread's life, released by a pthread key destructor);
 *   - the outcome resolvers: witness read (1021) and fence
 *     (1025/1031/1039), each a fresh default transaction bounded by the
 *     operation deadline;
 *   - fdb_run_txn itself;
 *   - the thread-local id batch allocators over the META counters.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fdb_txn.h"
#include "fdb_codec.h"
#include "mds_log.h"

/* A new attempt is only started with this much of the deadline left;
 * less than that goes to outcome resolution, so a budget that is nearly
 * gone yields DELAY (nothing in flight) instead of a fresh attempt that
 * can only time out into a fence. */
#define FDB_MIN_ATTEMPT_MS 200U

#define NS_PER_MS 1000000ULL

/* -----------------------------------------------------------------------
 * Real call table
 * ----------------------------------------------------------------------- */

uint64_t fdb_txn_now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static const struct fdb_txn_calls g_real_calls = {
    .create_transaction            = fdb_database_create_transaction,
    .transaction_destroy           = fdb_transaction_destroy,
    .transaction_reset             = fdb_transaction_reset,
    .transaction_set_option        = fdb_transaction_set_option,
    .transaction_set               = fdb_transaction_set,
    .transaction_get               = fdb_transaction_get,
    .transaction_add_conflict_range = fdb_transaction_add_conflict_range,
    .transaction_commit            = fdb_transaction_commit,
    .transaction_on_error          = fdb_transaction_on_error,
    .future_block_until_ready      = fdb_future_block_until_ready,
    .future_get_error              = fdb_future_get_error,
    .future_get_value              = fdb_future_get_value,
    .future_destroy                = fdb_future_destroy,
    .error_predicate               = fdb_error_predicate,
    .now_ns                        = fdb_txn_now_ns,
};

const struct fdb_txn_calls *fdb_txn_calls_real(void)
{
    return &g_real_calls;
}

/* -----------------------------------------------------------------------
 * Body helpers (real fdb_c)
 * ----------------------------------------------------------------------- */

fdb_error_t fdb_txn_wait(FDBFuture *f)
{
    fdb_error_t err;

    if (f == NULL) {
        return FDB_ERR_PLATFORM_ERROR;
    }
    err = fdb_future_block_until_ready(f);
    if (err != 0) {
        return err;
    }
    return fdb_future_get_error(f);
}

FDBFuture *fdb_txn_get_start(FDBTransaction *tr, const struct fdb_key *k, bool snapshot)
{
    if (!fdb_key_ok(k)) {
        return NULL;
    }
    return fdb_transaction_get(tr, k->buf, (int)k->len, snapshot ? 1 : 0);
}

fdb_error_t fdb_txn_get_finish(FDBFuture *f, uint8_t *buf, size_t cap, size_t *len, bool *found)
{
    fdb_error_t err;
    fdb_bool_t present = 0;
    const uint8_t *val = NULL;
    int vlen = 0;

    *len = 0;
    *found = false;
    if (f == NULL) {
        return FDB_ERR_PLATFORM_ERROR;
    }
    err = fdb_txn_wait(f);
    if (err == 0) {
        err = fdb_future_get_value(f, &present, &val, &vlen);
    }
    if (err == 0 && present != 0) {
        if (vlen < 0 || (size_t)vlen > cap) {
            err = FDB_ERR_VALUE_TOO_LARGE;
        } else {
            if (vlen > 0) {
                memcpy(buf, val, (size_t)vlen);
            }
            *len = (size_t)vlen;
            *found = true;
        }
    }
    fdb_future_destroy(f);
    return err;
}

fdb_error_t fdb_txn_get(FDBTransaction *tr, const struct fdb_key *k, bool snapshot,
                        uint8_t *buf, size_t cap, size_t *len, bool *found)
{
    return fdb_txn_get_finish(fdb_txn_get_start(tr, k, snapshot), buf, cap, len, found);
}

void fdb_txn_set(FDBTransaction *tr, const struct fdb_key *k, const uint8_t *v, size_t len)
{
    fdb_transaction_set(tr, k->buf, (int)k->len, v, (int)len);
}

void fdb_txn_clear(FDBTransaction *tr, const struct fdb_key *k)
{
    fdb_transaction_clear(tr, k->buf, (int)k->len);
}

void fdb_txn_clear_range(FDBTransaction *tr, const struct fdb_key_range *r)
{
    fdb_transaction_clear_range(tr, r->begin.buf, (int)r->begin.len,
                                r->end.buf, (int)r->end.len);
}

void fdb_txn_set_le64(FDBTransaction *tr, const struct fdb_key *k, uint64_t v)
{
    uint8_t b[8];

    fdb_le64_put(b, v);
    fdb_txn_set(tr, k, b, sizeof(b));
}

void fdb_txn_add_le64(FDBTransaction *tr, const struct fdb_key *k, int64_t delta)
{
    uint8_t b[8];

    fdb_le64_put(b, (uint64_t)delta);
    fdb_transaction_atomic_op(tr, k->buf, (int)k->len, b, (int)sizeof(b),
                              FDB_MUTATION_TYPE_ADD);
}

FDBFuture *fdb_txn_get_range_start(FDBTransaction *tr, const struct fdb_key_range *r,
                                   int limit, bool snapshot, bool reverse)
{
    if (!fdb_key_ok(&r->begin) || !fdb_key_ok(&r->end) || limit <= 0) {
        return NULL;
    }
    return fdb_transaction_get_range(tr,
                                     FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(r->begin.buf,
                                                                       (int)r->begin.len),
                                     FDB_KEYSEL_FIRST_GREATER_OR_EQUAL(r->end.buf,
                                                                       (int)r->end.len),
                                     limit, 0 /* target_bytes: FDB default */,
                                     FDB_STREAMING_MODE_EXACT, 1 /* iteration */,
                                     snapshot ? 1 : 0, reverse ? 1 : 0);
}

fdb_error_t fdb_txn_get_range_wait(FDBFuture *f, const FDBKeyValue **kvs, int *count,
                                   bool *more)
{
    fdb_error_t err;
    fdb_bool_t more_flag = 0;

    *kvs = NULL;
    *count = 0;
    *more = false;
    if (f == NULL) {
        return FDB_ERR_PLATFORM_ERROR;
    }
    err = fdb_txn_wait(f);
    if (err == 0) {
        err = fdb_future_get_keyvalue_array(f, kvs, count, &more_flag);
    }
    if (err == 0) {
        *more = (more_flag != 0);
    }
    return err;
}

static fdb_error_t set_timeout_with(const struct fdb_txn_calls *c, FDBTransaction *tr,
                                    uint32_t ms)
{
    int64_t v = (int64_t)ms;

    return c->transaction_set_option(tr, FDB_TR_OPTION_TIMEOUT, (const uint8_t *)&v,
                                     (int)sizeof(v));
}

fdb_error_t fdb_txn_set_timeout(FDBTransaction *tr, uint32_t ms)
{
    return set_timeout_with(&g_real_calls, tr, ms);
}

/* -----------------------------------------------------------------------
 * Witness slot pool
 * ----------------------------------------------------------------------- */

struct witness_slot {
    bool     in_use;
    uint64_t attempt_seq; /**< Persists across owners: monotonic per slot. */
};

static pthread_mutex_t     g_slot_lock = PTHREAD_MUTEX_INITIALIZER;
static struct witness_slot g_slots[FDB_WITNESS_SLOTS];
static uint32_t            g_slot_hint;
static pthread_once_t      g_slot_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t       g_slot_key;
static int                 g_slot_key_rc = -1;
static _Atomic uint64_t    g_witness_epoch;
static _Thread_local int32_t tl_slot = -1;

/* pthread key destructor: the value is the owning thread's slot. */
static void slot_release(void *v)
{
    struct witness_slot *s = v;

    if (s == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&g_slot_lock);
    s->in_use = false;
    (void)pthread_mutex_unlock(&g_slot_lock);
}

static void slot_key_init(void)
{
    g_slot_key_rc = pthread_key_create(&g_slot_key, slot_release);
}

/* Acquire (once) this thread's slot; -1 when the pool is exhausted or
 * the key could not be created. */
static int32_t slot_acquire(void)
{
    uint32_t i;
    int32_t found = -1;

    if (tl_slot >= 0) {
        return tl_slot;
    }
    (void)pthread_once(&g_slot_key_once, slot_key_init);
    if (g_slot_key_rc != 0) {
        return -1;
    }
    (void)pthread_mutex_lock(&g_slot_lock);
    for (i = 0; i < FDB_WITNESS_SLOTS; i++) {
        uint32_t idx = (g_slot_hint + i) % FDB_WITNESS_SLOTS;

        if (!g_slots[idx].in_use) {
            g_slots[idx].in_use = true;
            g_slot_hint = (idx + 1U) % FDB_WITNESS_SLOTS;
            found = (int32_t)idx;
            break;
        }
    }
    (void)pthread_mutex_unlock(&g_slot_lock);
    if (found < 0) {
        return -1;
    }
    if (pthread_setspecific(g_slot_key, &g_slots[found]) != 0) {
        slot_release(&g_slots[found]);
        return -1;
    }
    tl_slot = found;
    return found;
}

void fdb_txn_set_witness_epoch(uint64_t epoch)
{
    atomic_store(&g_witness_epoch, epoch);
}

uint64_t fdb_txn_witness_epoch(void)
{
    return atomic_load(&g_witness_epoch);
}

bool fdb_txn_witness_key(const struct fdb_backend *b, struct fdb_key *k, uint64_t *seq_out)
{
    int32_t slot = slot_acquire();

    if (slot < 0) {
        return false;
    }
    /* Only the owning thread touches its slot's sequence while it owns
     * it; the pool mutex orders an owner change. */
    *seq_out = ++g_slots[slot].attempt_seq;
    fdb_key_witness(k, &b->prefix, b->mds_id, b->witness_epoch, (uint32_t)slot);
    return fdb_key_ok(k);
}

/* -----------------------------------------------------------------------
 * Deadline arithmetic
 * ----------------------------------------------------------------------- */

struct run_state {
    struct fdb_backend         *b;
    const struct fdb_txn_calls *c;
    const char                 *op;
    uint64_t                    deadline_ns;
};

static void stat_inc(_Atomic uint64_t *ctr)
{
    atomic_fetch_add_explicit(ctr, 1U, memory_order_relaxed);
}

static uint64_t remaining_ms(const struct run_state *rs)
{
    uint64_t now = rs->c->now_ns();

    if (now >= rs->deadline_ns) {
        return 0;
    }
    return (rs->deadline_ns - now) / NS_PER_MS;
}

/* Per-attempt timeout: the configured value, capped by what is left of
 * the operation deadline (never 0, which would disable the timeout). */
static uint32_t attempt_timeout_ms(const struct run_state *rs)
{
    uint64_t left = remaining_ms(rs);
    uint32_t t = rs->b->txn_timeout_ms;

    if (left < t) {
        t = (uint32_t)left;
    }
    return t == 0 ? 1U : t;
}

static bool is_retryable_abort(const struct run_state *rs, fdb_error_t err)
{
    return err == FDB_ERR_NOT_COMMITTED || err == FDB_ERR_TRANSACTION_TOO_OLD ||
           rs->c->error_predicate(FDB_ERROR_PREDICATE_RETRYABLE_NOT_COMMITTED, err) != 0;
}

static bool is_maybe_in_flight(fdb_error_t err)
{
    return err == FDB_ERR_TRANSACTION_CANCELLED || err == FDB_ERR_TRANSACTION_TIMED_OUT ||
           err == FDB_ERR_CLUSTER_VERSION_CHANGED;
}

/* Run on_error for @p err on @p tr: 0 when the transaction was reset
 * and may be retried, else the terminal error. */
static fdb_error_t back_off(const struct run_state *rs, FDBTransaction *tr, fdb_error_t err)
{
    FDBFuture *f = rs->c->transaction_on_error(tr, err);
    fdb_error_t rerr;

    if (f == NULL) {
        return err;
    }
    rerr = rs->c->future_block_until_ready(f);
    if (rerr == 0) {
        rerr = rs->c->future_get_error(f);
    }
    rs->c->future_destroy(f);
    return rerr;
}

/* Commit @p tr and wait for the outcome: 0 = committed, else the
 * commit future's error, to be classified by the caller. */
static fdb_error_t commit_wait(const struct run_state *rs, FDBTransaction *tr)
{
    FDBFuture *cf = rs->c->transaction_commit(tr);
    fdb_error_t err;

    if (cf == NULL) {
        return FDB_ERR_PLATFORM_ERROR;
    }
    err = rs->c->future_block_until_ready(cf);
    if (err == 0) {
        err = rs->c->future_get_error(cf);
    }
    rs->c->future_destroy(cf);
    return err;
}

/* -----------------------------------------------------------------------
 * Outcome resolution
 * ----------------------------------------------------------------------- */

enum outcome {
    OUTCOME_LANDED,
    OUTCOME_NOT_LANDED,
    OUTCOME_UNKNOWN,
};

/* Read the witness key in @p tr2 and compare with @p seq.  *saw is only
 * meaningful when 0 is returned. */
static fdb_error_t witness_probe(const struct run_state *rs, FDBTransaction *tr2,
                                 const struct fdb_key *wk, uint64_t seq, bool *saw)
{
    FDBFuture *f;
    fdb_error_t err;
    fdb_bool_t present = 0;
    const uint8_t *val = NULL;
    int vlen = 0;

    *saw = false;
    f = rs->c->transaction_get(tr2, wk->buf, (int)wk->len, 0 /* conflicting read */);
    if (f == NULL) {
        return FDB_ERR_PLATFORM_ERROR;
    }
    err = rs->c->future_block_until_ready(f);
    if (err == 0) {
        err = rs->c->future_get_error(f);
    }
    if (err == 0) {
        err = rs->c->future_get_value(f, &present, &val, &vlen);
    }
    if (err == 0 && present != 0 && vlen == 8) {
        *saw = (fdb_le64_get(val) == seq);
    }
    rs->c->future_destroy(f);
    return err;
}

/* Fence in @p tr2 (which already holds the probe read): a WRITE
 * conflict range on the witness key, no mutation.  Committing it
 * intersects the in-flight attempt's READ conflict range, so the
 * attempt can no longer commit after this point. */
static fdb_error_t fence_commit(const struct run_state *rs, FDBTransaction *tr2,
                                const struct fdb_key *wk)
{
    struct fdb_key_range r;
    fdb_error_t err;

    fdb_key_range_single(&r, wk);
    err = rs->c->transaction_add_conflict_range(tr2, r.begin.buf, (int)r.begin.len,
                                                r.end.buf, (int)r.end.len,
                                                FDB_CONFLICT_RANGE_TYPE_WRITE);
    if (err == 0) {
        err = commit_wait(rs, tr2);
    }
    stat_inc(&rs->b->stats.fences);
    return err;
}

/* A probe or fence round failed with @p err.  A conflict backs off
 * through on_error; a timeout, a cancel, a version change or an
 * unknown fence outcome just resets the transaction (the next round
 * re-probes, so a fence that did land is seen).  True when the round
 * may be repeated; false on a terminal error. */
static bool resolve_round_retryable(const struct run_state *rs, FDBTransaction *tr2,
                                    fdb_error_t err)
{
    if (is_retryable_abort(rs, err)) {
        return back_off(rs, tr2, err) == 0;
    }
    if (is_maybe_in_flight(err) || err == FDB_ERR_COMMIT_UNKNOWN_RESULT) {
        rs->c->transaction_reset(tr2);
        return true;
    }
    return false;
}

/*
 * Resolve an attempt whose commit came back 1021 (no longer in flight)
 * or 1025/1031/1039 (maybe still in flight).  The read alone decides a
 * landing; a non-landing needs the fence commit when the attempt may
 * still be in flight.  Every round uses a fresh default transaction
 * (no GRV cache, no causal-read-risky) so its read version is causally
 * after any landed commit.  Bounded by the operation deadline; a
 * terminal error leaves the outcome unknown.
 */
static enum outcome resolve_outcome(const struct run_state *rs, const struct fdb_key *wk,
                                    uint64_t seq, bool need_fence)
{
    FDBTransaction *tr2 = NULL;
    enum outcome out = OUTCOME_UNKNOWN;

    if (rs->c->create_transaction(rs->b->db, &tr2) != 0) {
        return OUTCOME_UNKNOWN;
    }
    while (remaining_ms(rs) > 0) {
        bool saw = false;
        fdb_error_t err = set_timeout_with(rs->c, tr2, attempt_timeout_ms(rs));

        if (err == 0) {
            err = witness_probe(rs, tr2, wk, seq, &saw);
        }
        if (err == 0 && !saw && need_fence) {
            err = fence_commit(rs, tr2, wk);
        }
        if (err == 0) {
            out = saw ? OUTCOME_LANDED : OUTCOME_NOT_LANDED;
            break;
        }
        if (!resolve_round_retryable(rs, tr2, err)) {
            break;
        }
    }
    rs->c->transaction_destroy(tr2);
    return out;
}

/* -----------------------------------------------------------------------
 * Runner
 * ----------------------------------------------------------------------- */

static enum mds_status finish_indoubt(const struct run_state *rs, fdb_error_t err)
{
    stat_inc(&rs->b->stats.indoubt);
    MDS_LOG_ERROR(LOG_COMP_CAT,
                  "fdb %s: commit outcome unresolved within %u ms (last error %d %s): "
                  "MDS_ERR_INDOUBT", rs->op, rs->b->op_deadline_ms, (int)err,
                  fdb_get_error(err));
    return MDS_ERR_INDOUBT;
}

static enum mds_status finish_delay(const struct run_state *rs)
{
    stat_inc(&rs->b->stats.delay_exhausted);
    MDS_LOG_WARN(LOG_COMP_CAT, "fdb %s: deadline (%u ms) exhausted after definitive "
                 "aborts: MDS_ERR_DELAY", rs->op, rs->b->op_deadline_ms);
    return MDS_ERR_DELAY;
}

static enum mds_status finish_io(const struct run_state *rs, fdb_error_t err)
{
    stat_inc(&rs->b->stats.io_errors);
    MDS_LOG_ERROR(LOG_COMP_CAT, "fdb %s: error %d (%s): MDS_ERR_IO", rs->op, (int)err,
                  fdb_get_error(err));
    return MDS_ERR_IO;
}

/* Commit @p tr with the witness protocol.  Returns the status to hand
 * back, or MDS_OK with *rerun set when the body must be run again. */
static enum mds_status commit_attempt(const struct run_state *rs, FDBTransaction *tr,
                                      enum mds_status body_st, bool *rerun,
                                      fdb_error_t *err_out)
{
    struct fdb_key wk;
    struct fdb_key_range r;
    uint64_t seq = 0;
    uint8_t v[8];
    fdb_error_t err;
    enum outcome out;

    *rerun = false;
    if (!fdb_txn_witness_key(rs->b, &wk, &seq)) {
        MDS_LOG_ERROR(LOG_COMP_CAT, "fdb %s: witness slot pool exhausted (%u slots)",
                      rs->op, FDB_WITNESS_SLOTS);
        stat_inc(&rs->b->stats.delay_exhausted);
        *err_out = 0;
        return MDS_ERR_DELAY;
    }
    fdb_le64_put(v, seq);
    rs->c->transaction_set(tr, wk.buf, (int)wk.len, v, (int)sizeof(v));
    fdb_key_range_single(&r, &wk);
    err = rs->c->transaction_add_conflict_range(tr, r.begin.buf, (int)r.begin.len, r.end.buf,
                                                (int)r.end.len, FDB_CONFLICT_RANGE_TYPE_READ);
    if (err != 0) {
        *err_out = err;
        return MDS_ERR_IO;
    }
    err = commit_wait(rs, tr);
    *err_out = err;
    if (err == 0) {
        stat_inc(&rs->b->stats.commits);
        return body_st;
    }
    if (err != FDB_ERR_COMMIT_UNKNOWN_RESULT && !is_maybe_in_flight(err)) {
        return MDS_ERR_IO; /* the caller classifies retryable aborts */
    }
    if (err == FDB_ERR_COMMIT_UNKNOWN_RESULT) {
        stat_inc(&rs->b->stats.unknown_results);
    }
    out = resolve_outcome(rs, &wk, seq, err != FDB_ERR_COMMIT_UNKNOWN_RESULT);
    if (out == OUTCOME_LANDED) {
        stat_inc(err == FDB_ERR_COMMIT_UNKNOWN_RESULT ? &rs->b->stats.unknown_landed :
                                                        &rs->b->stats.fence_landed);
        stat_inc(&rs->b->stats.commits);
        return body_st;
    }
    if (out == OUTCOME_NOT_LANDED) {
        *rerun = true;
        return MDS_OK;
    }
    return finish_indoubt(rs, err);
}

enum attempt_action {
    ATTEMPT_RETURN, /**< *result is final. */
    ATTEMPT_RERUN,  /**< Transaction reset; run the body again. */
    ATTEMPT_FAILED, /**< *err is this attempt's definitive failure. */
};

/* One attempt: arm the timeout, run the body, commit when asked. */
static enum attempt_action run_attempt(const struct run_state *rs, FDBTransaction *tr,
                                       enum fdb_txn_kind kind, fdb_txn_body body, void *ctx,
                                       enum mds_status *result, fdb_error_t *err)
{
    enum mds_status st = MDS_ERR_IO;
    bool rerun = false;
    int rc;

    *err = set_timeout_with(rs->c, tr, attempt_timeout_ms(rs));
    if (*err != 0) {
        return ATTEMPT_FAILED;
    }
    stat_inc(&rs->b->stats.attempts);
    rc = body(tr, ctx, &st);
    if (rc == FDB_BODY_DONE || (rc == FDB_BODY_COMMIT && kind == FDB_TXN_READONLY)) {
        *result = st;
        return ATTEMPT_RETURN;
    }
    if (rc != FDB_BODY_COMMIT) {
        *err = (fdb_error_t)rc;
        return ATTEMPT_FAILED;
    }
    *result = commit_attempt(rs, tr, st, &rerun, err);
    if (rerun) {
        rs->c->transaction_reset(tr);
        return ATTEMPT_RERUN;
    }
    /* Committed, resolved (INDOUBT / DELAY), or an outcome the witness
     * protocol has already settled: final.  Anything else is a
     * definitive failure of the commit, classified like a body error. */
    if (*err == 0 || *result == MDS_ERR_INDOUBT || *result == MDS_ERR_DELAY ||
        *err == FDB_ERR_COMMIT_UNKNOWN_RESULT || is_maybe_in_flight(*err)) {
        return ATTEMPT_RETURN;
    }
    return ATTEMPT_FAILED;
}

/* @p *err is a definitive failure of an attempt (nothing was handed to
 * the commit path, or the commit itself aborted definitively): retry
 * the retryable ones through on_error's backoff; a timeout / cancel /
 * version change that hit a READ is equally definitive and only needs
 * a reset.  True when the attempt may be re-run; false when *err (the
 * backoff's error when that failed) is terminal. */
static bool attempt_failure_retryable(const struct run_state *rs, FDBTransaction *tr,
                                      fdb_error_t *err)
{
    if (is_retryable_abort(rs, *err)) {
        fdb_error_t rerr = back_off(rs, tr, *err);

        if (rerr != 0) {
            *err = rerr;
            return false;
        }
    } else if (is_maybe_in_flight(*err)) {
        rs->c->transaction_reset(tr);
    } else {
        return false;
    }
    stat_inc(&rs->b->stats.retries);
    return true;
}

enum mds_status fdb_run_txn(struct fdb_backend *b, enum fdb_txn_kind kind, const char *op,
                            fdb_txn_body body, void *ctx)
{
    struct run_state rs;
    FDBTransaction *tr = NULL;
    enum mds_status result = MDS_ERR_IO;
    fdb_error_t err;

    if (b == NULL || body == NULL) {
        return MDS_ERR_INVAL;
    }
    rs.b = b;
    rs.c = (b->calls != NULL) ? b->calls : &g_real_calls;
    rs.op = (op != NULL) ? op : "?";
    rs.deadline_ns = rs.c->now_ns() + (uint64_t)b->op_deadline_ms * NS_PER_MS;

    err = rs.c->create_transaction(b->db, &tr);
    if (err != 0 || tr == NULL) {
        MDS_LOG_ERROR(LOG_COMP_CAT, "fdb %s: create_transaction: %d %s", rs.op, (int)err,
                      fdb_get_error(err));
        stat_inc(&b->stats.io_errors);
        return MDS_ERR_IO;
    }

    for (;;) {
        enum attempt_action act;

        if (remaining_ms(&rs) < FDB_MIN_ATTEMPT_MS) {
            result = finish_delay(&rs);
            break;
        }
        act = run_attempt(&rs, tr, kind, body, ctx, &result, &err);
        if (act == ATTEMPT_RETURN) {
            break;
        }
        if (act == ATTEMPT_FAILED && !attempt_failure_retryable(&rs, tr, &err)) {
            result = finish_io(&rs, err);
            break;
        }
    }
    rs.c->transaction_destroy(tr);
    return result;
}

/* -----------------------------------------------------------------------
 * Id allocators
 * ----------------------------------------------------------------------- */

struct id_pool {
    uint64_t instance_seq;
    uint64_t next;
    uint32_t remaining;
};

#define ID_POOL_COUNT ((unsigned)FDB_META_COOKIE_SEQ + 1U)

static _Thread_local struct id_pool tl_pools[ID_POOL_COUNT];

struct refill_ctx {
    struct fdb_key key;
    uint64_t       first; /**< First id of the reserved batch. */
};

static int refill_body(FDBTransaction *tr, void *arg, enum mds_status *st_out)
{
    struct refill_ctx *rc = arg;
    uint8_t buf[8];
    size_t len = 0;
    bool found = false;
    uint64_t cur;
    fdb_error_t err;

    err = fdb_txn_get(tr, &rc->key, false, buf, sizeof(buf), &len, &found);
    if (err != 0) {
        return (int)err;
    }
    if (!found || !fdb_le64_decode(buf, len, &cur)) {
        *st_out = MDS_ERR_IO; /* counter absent or corrupt: not bootstrapped */
        return FDB_BODY_DONE;
    }
    if (cur > UINT64_MAX - MDS_FILEID_BATCH) {
        *st_out = MDS_ERR_NOSPC;
        return FDB_BODY_DONE;
    }
    fdb_txn_set_le64(tr, &rc->key, cur + MDS_FILEID_BATCH);
    rc->first = cur + 1U;
    *st_out = MDS_OK;
    return FDB_BODY_COMMIT;
}

enum mds_status fdb_backend_alloc_id(struct fdb_backend *b, enum fdb_meta_key counter,
                                     uint64_t *id)
{
    struct id_pool *pool;
    unsigned idx = (unsigned)counter;

    if (b == NULL || id == NULL || idx >= ID_POOL_COUNT || counter == FDB_META_SCHEMA_VERSION) {
        return MDS_ERR_INVAL;
    }
    pool = &tl_pools[idx];
    if (pool->instance_seq != b->instance_seq) {
        /* A different backend instance filled this pool: its ids belong
         * to another keyspace, discard the remainder. */
        pool->instance_seq = b->instance_seq;
        pool->remaining = 0;
    }
    if (pool->remaining == 0) {
        struct refill_ctx rc;
        enum mds_status st;

        memset(&rc, 0, sizeof(rc));
        fdb_key_meta(&rc.key, &b->prefix, counter);
        st = fdb_run_txn(b, FDB_TXN_MUTATING, "alloc_id", refill_body, &rc);
        if (st != MDS_OK) {
            return st;
        }
        pool->next = rc.first;
        pool->remaining = MDS_FILEID_BATCH;
    }
    *id = pool->next;
    pool->next++;
    pool->remaining--;
    return MDS_OK;
}
