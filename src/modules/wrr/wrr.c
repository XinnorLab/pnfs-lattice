/*
 * SPDX-License-Identifier: MIT
 *
 * wrr.c -- weighted-round-robin and capacity-derived placement kernels.
 *
 * Independent implementation of the two selection kernels declared in
 * include/wrr.h (MIT).  Written from the header contract and the
 * caller in src/fsal_obj/placement.c only; no enterprise code was
 * consulted.  Linked into pnfs_fsal_obj when ENABLE_WRR=ON.
 *
 * Contract (include/wrr.h):
 *   - weights[] holds one non-negative weight per ONLINE DS; the
 *     caller zeroes the weight of every slot already taken by an
 *     earlier stripe of the same file and expects a zero-weight slot
 *     never to be picked while a positive one exists.
 *   - both kernels return an index in [0, n); 0 when n == 0.
 *   - if every weight is zero the caller walks forward from the
 *     returned index to the next free slot, so returning 0 is safe.
 */

#include "wrr.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/*
 * Per-thread PRNG.  The output is a placement hint, not a security
 * token, so a plain LCG-seeded rand_r() state is enough; the
 * thread-local copy avoids sharing state between worker threads
 * without a lock.  Seeded lazily from time + thread-unique address so
 * two MDS processes do not replay the same sequence.
 */
static __thread unsigned int wrr_seed;
static __thread int wrr_seeded;

void mds_wrr_test_seed(uint32_t s)
{
    wrr_seed = (unsigned int)s;
    wrr_seeded = 1;
}

uint32_t mds_wrr_kernel_id(void)
{
    return MDS_WRR_KERNEL_XINNOR_V1;
}

static unsigned int wrr_rand(void)
{
    unsigned int *seed = &wrr_seed;
    int *seeded = &wrr_seeded;

    if (!*seeded) {
        *seed = (unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)seed;
        *seeded = 1;
    }
    return (unsigned int)rand_r(seed);
}

/* Uniform value in [0, bound) without modulo bias for 64-bit bounds. */
static uint64_t wrr_rand_below(uint64_t bound)
{
    uint64_t r;

    if (bound < 2) {
        return 0;
    }
    /* Two 31-bit draws give 62 bits, enough for any realistic
     * total-free-bytes sum on a lab or production cluster. */
    do {
        r = ((uint64_t)wrr_rand() << 31) | (uint64_t)wrr_rand();
        r &= (UINT64_C(1) << 62) - 1;
    } while (r >= ((UINT64_C(1) << 62) / bound) * bound);
    return r % bound;
}

uint32_t mds_wrr_weighted_pick(const uint64_t *free_bytes, uint32_t n)
{
    uint64_t total = 0;
    uint64_t r;
    uint32_t i;

    if (free_bytes == NULL || n == 0) {
        return 0;
    }

    /* Saturating sum so a pathological weight cannot wrap. */
    for (i = 0; i < n; i++) {
        if (free_bytes[i] > UINT64_MAX - total) {
            total = UINT64_MAX;
            break;
        }
        total += free_bytes[i];
    }
    if (total == 0) {
        return 0; /* nothing eligible -- caller walks forward */
    }

    /* Roulette-wheel selection: P(i) = w[i] / sum(w). */
    r = wrr_rand_below(total);
    for (i = 0; i < n; i++) {
        if (r < free_bytes[i]) {
            return i;
        }
        r -= free_bytes[i];
    }
    /* Unreachable unless the saturating sum clipped; fall back to the
     * last positive slot so the pick is still eligible. */
    for (i = n; i-- > 0;) {
        if (free_bytes[i] != 0) {
            return i;
        }
    }
    return 0;
}

int mds_wrr_weighted_pick2(const uint64_t *w, uint32_t n, uint32_t *out)
{
    uint64_t total = 0;
    uint64_t r;
    uint32_t i;

    if (w == NULL || n == 0 || out == NULL) {
        return -1;
    }
    /* The sampler draws 62 bits; refuse a sum that reaches 2^62 instead
     * of clipping it (the caller's weight bound guarantees it never
     * happens with the shipped configuration ranges). */
    for (i = 0; i < n; i++) {
        if (w[i] > ((UINT64_C(1) << 62) - 1) - total) {
            return -1;
        }
        total += w[i];
    }
    if (total == 0) {
        return -1;
    }
    r = wrr_rand_below(total);
    for (i = 0; i < n; i++) {
        if (r < w[i]) {
            *out = i;
            return 0;
        }
        r -= w[i];
    }
    return -1; /* unreachable: r < total */
}

uint32_t mds_wrr_capacity_pick(const uint64_t *free_bytes, uint32_t n)
{
    uint32_t best = 0;
    uint64_t best_w = 0;
    uint32_t i;

    if (free_bytes == NULL || n == 0) {
        return 0;
    }
    /* Strict '>' keeps the lowest index on ties, so a cluster of
     * equal-capacity DSes still spreads via the caller's taken[]
     * masking rather than always landing on the last one. */
    for (i = 0; i < n; i++) {
        if (free_bytes[i] > best_w) {
            best_w = free_bytes[i];
            best = i;
        }
    }
    return best; /* 0 when every weight is zero */
}
