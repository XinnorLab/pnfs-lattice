/*
 * SPDX-License-Identifier: MIT
 *
 * test_wrr_kernel.c -- the XinnorLab wrr kernel: identity, the refusing
 * pick2 entry point, the reproducible test seed and the 62-bit bound.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "wrr.h"

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
    fprintf(stdout, "  %-50s", #fn); \
    fflush(stdout); \
    fn(); \
    if (test_failed == 0) { tests_passed++; fprintf(stdout, "PASS\n"); } \
    else { fprintf(stdout, "FAILED\n"); } \
} while (0)

static void test_kernel_id_is_nonzero(void)
{
    ASSERT_EQ(mds_wrr_kernel_id(), 0x58494e01u);
}

static void test_pick2_refuses_empty_and_all_zero(void)
{
    uint64_t z[3] = {0, 0, 0};
    uint32_t out = 99;

    ASSERT_EQ(mds_wrr_weighted_pick2(NULL, 3, &out), -1);
    ASSERT_EQ(mds_wrr_weighted_pick2(z, 0, &out), -1);
    ASSERT_EQ(mds_wrr_weighted_pick2(z, 3, &out), -1);
    ASSERT_EQ(mds_wrr_weighted_pick2(z, 3, NULL), -1);
    ASSERT_EQ(out, 99u);
}

static void test_pick2_never_returns_a_zero_slot(void)
{
    uint64_t w[4] = {0, 5, 0, 5};
    uint32_t out;

    mds_wrr_test_seed(7);
    for (int i = 0; i < 2000; i++) {
        ASSERT_EQ(mds_wrr_weighted_pick2(w, 4, &out), 0);
        ASSERT_TRUE(out == 1 || out == 3);
    }
}

static void test_pick2_distribution_4_to_1(void)
{
    uint64_t w[2] = {80, 20};
    uint32_t out;
    uint32_t hits0 = 0;

    mds_wrr_test_seed(12345);
    for (int i = 0; i < 100000; i++) {
        ASSERT_EQ(mds_wrr_weighted_pick2(w, 2, &out), 0);
        if (out == 0) {
            hits0++;
        }
    }
    ASSERT_TRUE(hits0 > 78000 && hits0 < 82000); /* 80 % +- 2 % */
}

static void test_pick2_refuses_sum_at_2_pow_62(void)
{
    uint64_t w[2] = {UINT64_C(1) << 61, UINT64_C(1) << 61};
    uint64_t ok[2] = {(UINT64_C(1) << 61) - 1, UINT64_C(1) << 61};
    uint32_t out;

    ASSERT_EQ(mds_wrr_weighted_pick2(w, 2, &out), -1);
    ASSERT_EQ(mds_wrr_weighted_pick2(ok, 2, &out), 0); /* 2^62 - 1 is allowed */
}

static void test_seed_is_deterministic(void)
{
    uint64_t w[3] = {1, 2, 3};
    uint32_t a[50];
    uint32_t b[50];

    mds_wrr_test_seed(99);
    for (int i = 0; i < 50; i++) {
        (void)mds_wrr_weighted_pick2(w, 3, &a[i]);
    }
    mds_wrr_test_seed(99);
    for (int i = 0; i < 50; i++) {
        (void)mds_wrr_weighted_pick2(w, 3, &b[i]);
    }
    ASSERT_EQ(memcmp(a, b, sizeof(a)), 0);
}

static void test_legacy_picks_still_work(void)
{
    uint64_t w[3] = {0, 0, 9};
    uint64_t z[2] = {0, 0};

    ASSERT_EQ(mds_wrr_weighted_pick(w, 3), 2u);
    ASSERT_EQ(mds_wrr_weighted_pick(z, 2), 0u);   /* legacy contract: 0 on all-zero */
    ASSERT_EQ(mds_wrr_capacity_pick(w, 3), 2u);
}

int main(void)
{
    printf("test_wrr_kernel\n");
    RUN_TEST(test_kernel_id_is_nonzero);
    RUN_TEST(test_pick2_refuses_empty_and_all_zero);
    RUN_TEST(test_pick2_never_returns_a_zero_slot);
    RUN_TEST(test_pick2_distribution_4_to_1);
    RUN_TEST(test_pick2_refuses_sum_at_2_pow_62);
    RUN_TEST(test_seed_is_deterministic);
    RUN_TEST(test_legacy_picks_still_work);
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
