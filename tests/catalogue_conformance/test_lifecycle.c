/*
 * Copyright (c) 2026 PeakAIO
 * SPDX-License-Identifier: MIT
 *
 * test_lifecycle.c -- Contract C7: ownership at close.
 *
 * ops->close() releases backend-owned resources only and the
 * dispatcher frees struct mds_catalogue (mds_catalogue_close).  Two
 * things go wrong when a backend gets this wrong: a double free when
 * the backend frees the handle too, and a leak when it forgets its own
 * state.  Neither is visible to a plain run, so this test is also run
 * under -fsanitize=address,undefined (see the conformance CMakeLists
 * for the second configure); the plain run still catches crashes and
 * cross-handle interference.
 *
 *   1. 200 open/use/close cycles on one thread.
 *   2. 8 handles open at the same time, each driven from its own
 *      thread, then closed in a shuffled (but reproducible) order.
 *      Handles are independent stores on memdb and independent
 *      connections on a shared store, so every thread only ever sees
 *      its own names.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "check.h"

#define CYCLES        200
#define HANDLES       8
#define OPS_PER_THREAD 64

static void test_open_close_cycles(void)
{
    int i;

    for (i = 0; i < CYCLES; i++) {
        struct mds_catalogue *cat = NULL;
        struct mds_inode child, seen;
        uint64_t dir = 0;

        REQUIRE_EQ(conformance_open(&cat), MDS_OK);
        REQUIRE(cat != NULL);
        REQUIRE_EQ(conformance_scratch_dir(cat, &dir), MDS_OK);
        memset(&child, 0, sizeof(child));
        REQUIRE_EQ(mds_cat_ns_create(cat, NULL, dir, "cycle", MDS_FTYPE_REG,
                                     0644, 0, 0, NULL, &child), MDS_OK);
        CHECK_EQ(mds_cat_ns_lookup(cat, dir, "cycle", &seen), MDS_OK);
        CHECK_EQ(seen.fileid, child.fileid);
        conformance_scratch_cleanup(cat, dir);
        mds_catalogue_close(cat);
    }
    /* NULL is always safe. */
    mds_catalogue_close(NULL);
}

struct worker {
    struct mds_catalogue *cat;
    uint64_t dir;
    int index;
    int failures;
};

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    int i;

    for (i = 0; i < OPS_PER_THREAD; i++) {
        struct mds_inode child, seen;
        char name[64];

        (void)snprintf(name, sizeof(name), "h%d-f%d", w->index, i);
        memset(&child, 0, sizeof(child));
        if (mds_cat_ns_create(w->cat, NULL, w->dir, name, MDS_FTYPE_REG,
                              0644, 0, 0, NULL, &child) != MDS_OK) {
            w->failures++;
            continue;
        }
        if (mds_cat_ns_lookup(w->cat, w->dir, name, &seen) != MDS_OK ||
            seen.fileid != child.fileid) {
            w->failures++;
        }
        if (mds_cat_ns_remove(w->cat, NULL, w->dir, name) != MDS_OK) {
            w->failures++;
        }
        if (mds_cat_ns_lookup(w->cat, w->dir, name, &seen) !=
            MDS_ERR_NOTFOUND) {
            w->failures++;
        }
    }
    return NULL;
}

/* Fixed-seed shuffle so a failing order is reproducible from the log. */
static void shuffle(int *order, int n)
{
    uint32_t state = 0x9E3779B9U;
    int i;

    for (i = n - 1; i > 0; i--) {
        int j;
        int tmp;

        state = state * 1664525U + 1013904223U;
        j = (int)(state % (uint32_t)(i + 1));
        tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
}

static void test_concurrent_handles(void)
{
    struct worker workers[HANDLES];
    pthread_t threads[HANDLES];
    int order[HANDLES];
    int opened = 0;
    int i;

    memset(workers, 0, sizeof(workers));
    for (i = 0; i < HANDLES; i++) {
        workers[i].index = i;
        if (conformance_open(&workers[i].cat) != MDS_OK ||
            workers[i].cat == NULL) {
            break;
        }
        opened++;
        if (conformance_scratch_dir(workers[i].cat, &workers[i].dir) !=
            MDS_OK) {
            workers[i].failures++;
        }
    }
    CHECK_EQ(opened, HANDLES);

    for (i = 0; i < opened; i++) {
        CHECK_EQ(pthread_create(&threads[i], NULL, worker_main,
                                &workers[i]), 0);
    }
    for (i = 0; i < opened; i++) {
        (void)pthread_join(threads[i], NULL);
        CHECK_EQ(workers[i].failures, 0);
    }

    for (i = 0; i < opened; i++) {
        order[i] = i;
    }
    shuffle(order, opened);
    (void)printf("  close order:");
    for (i = 0; i < opened; i++) {
        struct worker *w = &workers[order[i]];

        (void)printf(" %d", order[i]);
        conformance_scratch_cleanup(w->cat, w->dir);
        mds_catalogue_close(w->cat);
        w->cat = NULL;
    }
    (void)printf("\n");
}

int main(void)
{
    /* Probe the backend once so an unavailable backend skips before
     * any counting happens. */
    struct mds_catalogue *probe = conformance_open_checked();
    int rc;

    (void)printf("test_lifecycle (backend=%s):\n", conformance_backend_name());
    mds_catalogue_close(probe);

    test_open_close_cycles();
    test_concurrent_handles();

    rc = check_summary("test_lifecycle");
    conformance_shutdown();
    return rc;
}
