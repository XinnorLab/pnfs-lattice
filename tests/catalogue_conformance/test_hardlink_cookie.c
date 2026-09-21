/*
 * Copyright (c) 2026 PeakAIO
 * SPDX-License-Identifier: MIT
 *
 * test_hardlink_cookie.c -- Phase 0b: hard-link-safe READDIR cookies.
 *
 * Two hard links to one inode in one directory, listed through
 * mds_cat_ns_readdir_plus_from_cookie() with a page size of one and
 * the resume cookie taken from the last delivered entry: both names
 * must come back exactly once, every cookie must be >= 3 and the
 * cookies must be strictly increasing across pages.  A backend that
 * derives the cookie from the child fileid hands out the same cookie
 * for both links, so the second page (cookie > first) drops one name.
 *
 * RonDB resumes over ix_dirents_parent_child and has no per-dirent
 * ordering column until its schema change lands; on that backend the
 * failure is documented and the test reports XFAIL with exit 77.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "check.h"

#define MAX_PAGES 8

struct page {
    uint32_t count;
    uint64_t fileid;
    uint64_t cookie;
    char     name[MDS_MAX_NAME + 1];
};

static int page_cb(const struct mds_cat_dirent *entry,
                   const struct mds_inode *inode, bool inode_valid,
                   void *arg)
{
    struct page *p = arg;

    (void)inode;
    (void)inode_valid;
    if (p->count == 0) {
        p->fileid = entry->fileid;
        p->cookie = entry->cookie;
        (void)snprintf(p->name, sizeof(p->name), "%s", entry->name);
    }
    p->count++;
    return 0;
}

static void test_two_links_page_size_one(struct mds_catalogue *cat,
                                         uint64_t dir)
{
    struct mds_inode file, seen;
    uint64_t cookie = 0;
    uint64_t prev_cookie = 0;
    unsigned seen_a = 0, seen_b = 0, seen_other = 0;
    unsigned pages = 0;

    memset(&file, 0, sizeof(file));
    REQUIRE_EQ(mds_cat_ns_create(cat, NULL, dir, "a", MDS_FTYPE_REG, 0644,
                                 0, 0, NULL, &file), MDS_OK);
    REQUIRE_EQ(mds_cat_ns_link(cat, NULL, dir, "b", file.fileid), MDS_OK);
    REQUIRE_EQ(mds_cat_ns_getattr(cat, file.fileid, &seen), MDS_OK);
    CHECK_EQ(seen.nlink, 2);

    for (;;) {
        struct page p;

        memset(&p, 0, sizeof(p));
        REQUIRE_EQ(mds_cat_ns_readdir_plus_from_cookie(cat, dir, cookie, 1,
                                                       NULL, page_cb, &p),
                   MDS_OK);
        if (p.count == 0) {
            break; /* drained */
        }
        pages++;
        CHECK_EQ(p.count, 1);            /* page size honoured */
        CHECK(p.cookie >= 3);            /* never a reserved cookie */
        CHECK(p.cookie > prev_cookie);   /* strictly increasing */
        CHECK_EQ(p.fileid, file.fileid);
        if (strcmp(p.name, "a") == 0) {
            seen_a++;
        } else if (strcmp(p.name, "b") == 0) {
            seen_b++;
        } else {
            seen_other++;
        }
        prev_cookie = p.cookie;
        cookie = p.cookie;
        if (pages >= MAX_PAGES) {
            break; /* bounded: a broken cursor must not loop forever */
        }
    }

    CHECK_EQ(seen_a, 1);
    CHECK_EQ(seen_b, 1);
    CHECK_EQ(seen_other, 0);
    CHECK_EQ(pages, 2);
}

int main(void)
{
    struct mds_catalogue *cat;
    uint64_t dir = 0;

    if (conformance_backend_is("rondb")) {
        (void)printf("XFAIL: RonDB assigns cookie = child fileid and "
                     "resumes over ix_dirents_parent_child, so two hard "
                     "links in one directory share a cookie until the "
                     "per-dirent sequence column / ordered index schema "
                     "change lands (documented deviation, "
                     "catalogue_internal.h ns_readdir_plus_from)\n");
        return CONFORMANCE_SKIP;
    }

    cat = conformance_open_checked();
    (void)printf("test_hardlink_cookie (backend=%s):\n",
                 conformance_backend_name());

    if (conformance_scratch_dir(cat, &dir) != MDS_OK) {
        (void)fprintf(stderr, "  FAIL: cannot create scratch directory\n");
        mds_catalogue_close(cat);
        return 1;
    }

    test_two_links_page_size_one(cat, dir);

    conformance_scratch_cleanup(cat, dir);
    mds_catalogue_close(cat);
    return check_summary("test_hardlink_cookie");
}
