/*
 * Copyright (c) 2026 PeakAIO
 * SPDX-License-Identifier: MIT
 *
 * lab_cluster_drive.c -- Lab driver for the cluster-service contract
 * and the namespace guards against a LIVE store.
 *
 * Not a ctest: it opens the RonDB catalogue named by RONDB_CONF (the
 * same file the daemon uses) and drives the public dispatchers
 * (mds_cluster_*, mds_coord_recovery_*, mds_cat_ns_*) so the lab can
 * observe the store's answers next to running daemons -- dump the node
 * registry and partition map, heartbeat or deregister with a chosen
 * boot_epoch, list recovery rows by owner, and run the RMDIR / RENAME
 * / LINK guard probes that the in-tree tests only exercise on memdb.
 *
 * Usage:
 *   RONDB_CONF=/etc/pnfs-mds/rondb.conf lab_cluster_drive [--mds-id N] CMD ...
 *
 *   nodes                              dump mds_node_registry
 *   partitions                         dump mds_partition_map
 *   register ID EPOCH HOST             mds_cluster_node_register
 *   heartbeat ID EPOCH                 mds_cluster_node_heartbeat
 *   deregister ID EPOCH                mds_cluster_node_deregister
 *   scan-stale STALE_MS                rows older than now - STALE_MS
 *   partition-put PID OWNER STATE PATH INSERT_ONLY(0|1)
 *   partition-cas PID EXPECTED NEW STATE  mds_cluster_partition_cas
 *   recovery-list OWNER                mds_coord_recovery_list(OWNER)
 *   journal-list                       dump the 2PC rename journal
 *   journal-del TXN_ID ROLE            delete one journal row
 *   ns-semantics                       RMDIR / RENAME / LINK guard probes
 *   readdir PARENT_FILEID              dirent rows of a directory, each
 *                                      with its child inode's status
 *   lookup PARENT_FILEID NAME          fused dirent + inode read
 *   dirent-get PARENT_FILEID NAME      dirent row alone
 *   getattr FILEID                     inode row alone
 *   scrub                              dangling-dirent census: root and
 *                                      every conf-* / lab-ns-* scratch
 *                                      directory under it (two levels),
 *                                      each dirent's inode read back;
 *                                      run after every RonDB conformance
 *                                      run so a dirent left pointing at
 *                                      a deleted inode is caught with
 *                                      the run that produced it
 *
 * Every store answer is printed as its mds_status name.  Exit status:
 * 0 when the command ran (the printed status is the result), 2 when a
 * probe of ns-semantics failed or scrub found a dangling dirent, 1 on
 * usage or open errors.
 *
 * --mds-id sets the handle's own identity (default 99): recovery rows
 * written through this handle carry it as their owner, so it must not
 * collide with a live daemon's mds_id.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pnfs_mds.h"
#include "mds_catalogue.h"
#include "mds_cluster.h"
#include "mds_coordination.h"

#define LAB_DEFAULT_MDS_ID 99U

static int usage(void)
{
    (void)fprintf(stderr,
        "usage: RONDB_CONF=<rondb.conf> lab_cluster_drive [--mds-id N] CMD ...\n"
        "  nodes | partitions | register ID EPOCH HOST | heartbeat ID EPOCH |\n"
        "  deregister ID EPOCH | scan-stale STALE_MS |\n"
        "  partition-put PID OWNER STATE PATH INSERT_ONLY |\n"
        "  partition-cas PID EXPECTED NEW STATE | recovery-list OWNER |\n"
        "  journal-list | journal-del TXN_ID ROLE |\n"
        "  ns-semantics | readdir PARENT | lookup PARENT NAME |\n"
        "  dirent-get PARENT NAME | getattr FILEID | scrub\n");
    return 1;
}

static uint64_t now_realtime_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Wall-clock rendering of a CLOCK_REALTIME nanosecond stamp; a value
 * below 2020 cannot be one (see failover_watchdog.h) and is shown raw. */
static void format_ns(uint64_t ns, char *buf, size_t cap)
{
    time_t sec = (time_t)(ns / 1000000000ULL);
    struct tm tm_utc;

    if (ns < 1577836800000000000ULL || gmtime_r(&sec, &tm_utc) == NULL) {
        (void)snprintf(buf, cap, "(not realtime)");
        return;
    }
    if (strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) == 0) {
        (void)snprintf(buf, cap, "(unformattable)");
    }
}

static bool parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (s == NULL || *s == '\0') {
        return false;
    }
    v = strtoull(s, &end, 10);
    if (end == NULL || *end != '\0') {
        return false;
    }
    *out = (uint64_t)v;
    return true;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    uint64_t v;

    if (!parse_u64(s, &v) || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static struct mds_catalogue *open_rondb(uint32_t mds_id)
{
    const char *conf = getenv("RONDB_CONF");
    struct mds_config *cfg;
    struct mds_catalogue *cat = NULL;
    enum mds_status st;

    if (conf == NULL || conf[0] == '\0') {
        (void)fprintf(stderr, "RONDB_CONF is not set\n");
        return NULL;
    }
    /* struct mds_config is large; never on the stack. */
    cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL) {
        return NULL;
    }
    cfg->catalogue_backend = MDS_BACKEND_RONDB;
    (void)snprintf(cfg->catalogue_backend_conf,
                   sizeof(cfg->catalogue_backend_conf), "%s", conf);
    cfg->self.id = mds_id;
    (void)snprintf(cfg->self.hostname, sizeof(cfg->self.hostname),
                   "lab-drive");
    cfg->cluster_size = 1;
    cfg->ndb_conn_pool_size = 1;
    cfg->ndb_async_writes = false;
    cfg->catalog_image_mode = MDS_IMAGE_OFF;
    cfg->catalog_replay_mode = MDS_REPLAY_OFF;

    st = mds_catalogue_open(cfg, &cat);
    free(cfg);
    if (st != MDS_OK || cat == NULL) {
        (void)fprintf(stderr, "mds_catalogue_open: %s\n", mds_status_str(st));
        return NULL;
    }
    return cat;
}

/* ----------------------------------------------------------------------- */

static int print_node_cb(uint32_t mds_id, uint64_t boot_epoch,
                         const char *hostname, uint16_t nfs_port,
                         uint16_t grpc_port, uint64_t last_heartbeat_ns,
                         void *ctx)
{
    char when[40];

    (void)ctx;
    format_ns(last_heartbeat_ns, when, sizeof(when));
    (void)printf("mds_id=%" PRIu32 " boot_epoch=%" PRIu64 " host=%s nfs=%u "
                 "grpc=%u last_heartbeat_ns=%" PRIu64 " (%s)\n",
                 mds_id, boot_epoch, hostname != NULL ? hostname : "",
                 (unsigned)nfs_port, (unsigned)grpc_port, last_heartbeat_ns,
                 when);
    return 0;
}

static int print_stale_cb(uint32_t mds_id, uint64_t boot_epoch,
                          uint64_t last_heartbeat_ns, void *ctx)
{
    char when[40];

    (void)ctx;
    format_ns(last_heartbeat_ns, when, sizeof(when));
    (void)printf("stale: mds_id=%" PRIu32 " boot_epoch=%" PRIu64
                 " last_heartbeat_ns=%" PRIu64 " (%s)\n",
                 mds_id, boot_epoch, last_heartbeat_ns, when);
    return 0;
}

static int print_partition_cb(uint32_t partition_id, uint32_t owner_mds_id,
                              uint8_t state, const char *subtree_path,
                              void *ctx)
{
    (void)ctx;
    (void)printf("partition_id=%" PRIu32 " owner_mds_id=%" PRIu32
                 " state=%u path=%s\n",
                 partition_id, owner_mds_id, (unsigned)state,
                 subtree_path != NULL ? subtree_path : "");
    return 0;
}

static int print_journal_cb(const struct mds_coord_journal_record *rec,
                            void *ctx)
{
    (void)ctx;
    (void)printf("txn_id=%" PRIu64 " role=%u state=%u remote_mds=%" PRIu32
                 " src_parent=%" PRIu64 " dst_parent=%" PRIu64
                 " src_child=%" PRIu64 " src=%s dst=%s\n",
                 rec->txn_id, (unsigned)rec->role, (unsigned)rec->state,
                 rec->remote_mds_id, rec->src_parent_fileid,
                 rec->dst_parent_fileid, rec->src_child_fileid,
                 rec->src_name, rec->dst_name);
    return 0;
}

static int print_recovery_cb(uint64_t clientid, uint32_t owner_mds_id,
                             uint64_t owner_boot_epoch, void *ctx)
{
    (void)ctx;
    (void)printf("clientid=%" PRIu64 " owner_mds_id=%" PRIu32
                 " owner_boot_epoch=%" PRIu64 "\n",
                 clientid, owner_mds_id, owner_boot_epoch);
    return 0;
}

/* -----------------------------------------------------------------------
 * ns-semantics: the guards the RonDB shim decides inside its
 * transactions.  Each probe prints PASS/FAIL with the observed status.
 * ----------------------------------------------------------------------- */

struct probe {
    unsigned failed;
};

static void probe_result(struct probe *p, bool ok, const char *what,
                         const char *detail)
{
    (void)printf("  %s %s: %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok) {
        p->failed++;
    }
}

static bool mk(struct mds_catalogue *cat, uint64_t parent, const char *name,
               enum mds_file_type type, struct mds_inode *out)
{
    memset(out, 0, sizeof(*out));
    return mds_cat_ns_create(cat, NULL, parent, name, type, 0755, 0, 0, NULL,
                             out) == MDS_OK;
}

static void ns_semantics(struct mds_catalogue *cat, struct probe *p)
{
    struct mds_inode scratch, d_full, d_empty, child, victim_full, victim_empty;
    struct mds_inode src_a, src_b, seen, f, dirtarget;
    char scratch_name[64];
    char detail[256];
    enum mds_status st;

    (void)snprintf(scratch_name, sizeof(scratch_name), "lab-ns-%ld",
                   (long)time(NULL));
    if (!mk(cat, MDS_FILEID_ROOT, scratch_name, MDS_FTYPE_DIR, &scratch)) {
        probe_result(p, false, "setup", "cannot create the scratch directory");
        return;
    }

    /* RMDIR of a non-empty directory: NOTEMPTY, child and dir intact. */
    if (mk(cat, scratch.fileid, "full", MDS_FTYPE_DIR, &d_full) &&
        mk(cat, d_full.fileid, "kid", MDS_FTYPE_REG, &child)) {
        st = mds_cat_ns_remove(cat, NULL, scratch.fileid, "full");
        (void)snprintf(detail, sizeof(detail),
                       "remove -> %s; dir getattr -> %s; child lookup -> %s",
                       mds_status_str(st),
                       mds_status_str(mds_cat_ns_getattr(cat, d_full.fileid,
                                                         &seen)),
                       mds_status_str(mds_cat_ns_lookup(cat, d_full.fileid,
                                                        "kid", &seen)));
        probe_result(p, st == MDS_ERR_NOTEMPTY &&
                     mds_cat_ns_getattr(cat, d_full.fileid, &seen) == MDS_OK &&
                     mds_cat_ns_lookup(cat, d_full.fileid, "kid", &seen) ==
                         MDS_OK,
                     "rmdir non-empty", detail);
        (void)mds_cat_ns_remove(cat, NULL, d_full.fileid, "kid");
        (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "full");
    } else {
        probe_result(p, false, "rmdir non-empty", "setup failed");
    }

    /* RMDIR of an empty directory: OK, inode row gone (NOTFOUND). */
    if (mk(cat, scratch.fileid, "empty", MDS_FTYPE_DIR, &d_empty)) {
        st = mds_cat_ns_remove(cat, NULL, scratch.fileid, "empty");
        (void)snprintf(detail, sizeof(detail),
                       "remove -> %s; old inode getattr -> %s",
                       mds_status_str(st),
                       mds_status_str(mds_cat_ns_getattr(cat, d_empty.fileid,
                                                         &seen)));
        probe_result(p, st == MDS_OK &&
                     mds_cat_ns_getattr(cat, d_empty.fileid, &seen) ==
                         MDS_ERR_NOTFOUND,
                     "rmdir empty", detail);
    } else {
        probe_result(p, false, "rmdir empty", "setup failed");
    }

    /* RENAME over a non-empty directory: NOTEMPTY (or EXISTS), nothing
     * changes: source still resolves, victim and its child intact. */
    if (mk(cat, scratch.fileid, "src_a", MDS_FTYPE_DIR, &src_a) &&
        mk(cat, scratch.fileid, "vfull", MDS_FTYPE_DIR, &victim_full) &&
        mk(cat, victim_full.fileid, "vkid", MDS_FTYPE_REG, &child)) {
        st = mds_cat_ns_rename(cat, NULL, scratch.fileid, "src_a",
                               scratch.fileid, "vfull");
        bool src_ok = mds_cat_ns_lookup(cat, scratch.fileid, "src_a", &seen) ==
                          MDS_OK && seen.fileid == src_a.fileid;
        bool vic_ok = mds_cat_ns_lookup(cat, scratch.fileid, "vfull", &seen) ==
                          MDS_OK && seen.fileid == victim_full.fileid;
        bool kid_ok = mds_cat_ns_lookup(cat, victim_full.fileid, "vkid",
                                        &seen) == MDS_OK;
        (void)snprintf(detail, sizeof(detail),
                       "rename -> %s; src intact=%d victim intact=%d "
                       "victim child intact=%d",
                       mds_status_str(st), (int)src_ok, (int)vic_ok,
                       (int)kid_ok);
        probe_result(p, (st == MDS_ERR_NOTEMPTY || st == MDS_ERR_EXISTS) &&
                     src_ok && vic_ok && kid_ok,
                     "rename over non-empty dir", detail);
        (void)mds_cat_ns_remove(cat, NULL, victim_full.fileid, "vkid");
        (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "vfull");
        (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "src_a");
    } else {
        probe_result(p, false, "rename over non-empty dir", "setup failed");
    }

    /* RENAME over an empty directory: OK, victim inode gone, name
     * rebound to the source inode. */
    if (mk(cat, scratch.fileid, "src_b", MDS_FTYPE_DIR, &src_b) &&
        mk(cat, scratch.fileid, "vempty", MDS_FTYPE_DIR, &victim_empty)) {
        st = mds_cat_ns_rename(cat, NULL, scratch.fileid, "src_b",
                               scratch.fileid, "vempty");
        bool rebound = mds_cat_ns_lookup(cat, scratch.fileid, "vempty",
                                         &seen) == MDS_OK &&
                       seen.fileid == src_b.fileid;
        enum mds_status vst = mds_cat_ns_getattr(cat, victim_empty.fileid,
                                                 &seen);
        (void)snprintf(detail, sizeof(detail),
                       "rename -> %s; name rebound to source=%d; "
                       "victim inode getattr -> %s",
                       mds_status_str(st), (int)rebound, mds_status_str(vst));
        probe_result(p, st == MDS_OK && rebound && vst == MDS_ERR_NOTFOUND,
                     "rename over empty dir", detail);
        (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "vempty");
        (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "src_b");
    } else {
        probe_result(p, false, "rename over empty dir", "setup failed");
    }

    /* LINK: success bumps nlink; a directory target is ISDIR; a name
     * collision is EXISTS; the final unlink then removes the inode. */
    if (mk(cat, scratch.fileid, "f", MDS_FTYPE_REG, &f) &&
        mk(cat, scratch.fileid, "dtarget", MDS_FTYPE_DIR, &dirtarget)) {
        enum mds_status st_link = mds_cat_ns_link(cat, NULL, scratch.fileid,
                                                  "f2", f.fileid);
        enum mds_status st_isdir = mds_cat_ns_link(cat, NULL, scratch.fileid,
                                                   "dlink", dirtarget.fileid);
        enum mds_status st_exists = mds_cat_ns_link(cat, NULL, scratch.fileid,
                                                    "f", f.fileid);
        uint32_t nlink = 0;

        if (mds_cat_ns_getattr(cat, f.fileid, &seen) == MDS_OK) {
            nlink = seen.nlink;
        }
        (void)snprintf(detail, sizeof(detail),
                       "link -> %s (nlink=%u); dir target -> %s; "
                       "collision -> %s",
                       mds_status_str(st_link), (unsigned)nlink,
                       mds_status_str(st_isdir), mds_status_str(st_exists));
        probe_result(p, st_link == MDS_OK && nlink == 2 &&
                     st_isdir == MDS_ERR_ISDIR && st_exists == MDS_ERR_EXISTS,
                     "link", detail);
        (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "f");
        st = mds_cat_ns_remove(cat, NULL, scratch.fileid, "f2");
        (void)snprintf(detail, sizeof(detail),
                       "final unlink -> %s; inode getattr -> %s",
                       mds_status_str(st),
                       mds_status_str(mds_cat_ns_getattr(cat, f.fileid,
                                                         &seen)));
        probe_result(p, st == MDS_OK &&
                     mds_cat_ns_getattr(cat, f.fileid, &seen) ==
                         MDS_ERR_NOTFOUND,
                     "final unlink after link", detail);
        (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "dtarget");
    } else {
        probe_result(p, false, "link", "setup failed");
    }

    /* ns_remove_known_gc with a STALE child snapshot (the name was
     * removed and re-created, so the snapshot's fileid is gone): the
     * contract is MDS_ERR_STALE with nothing changed -- the current
     * inode keeps its name, the GC queue is untouched. */
    {
        struct mds_inode c1, c2, cur;
        struct mds_ds_map_entry gc_entry;
        uint32_t gc_before = 0, gc_after = 0;
        bool folded = false;

        if (mk(cat, scratch.fileid, "g", MDS_FTYPE_REG, &c1) &&
            mds_cat_ns_remove(cat, NULL, scratch.fileid, "g") == MDS_OK &&
            mk(cat, scratch.fileid, "g", MDS_FTYPE_REG, &c2) &&
            mds_cat_gc_count(cat, &gc_before) == MDS_OK) {
            enum mds_status st_name;
            enum mds_status st_cur;

            memset(&gc_entry, 0, sizeof(gc_entry));
            gc_entry.ds_id = 1;
            gc_entry.nfs_fh_len = 4;
            gc_entry.nfs_fh[0] = 0xAB;
            st = mds_cat_ns_remove_known_gc(cat, NULL, scratch.fileid, "g",
                                            &c1, 1, &gc_entry, 1,
                                            MDS_GC_SWEEP_GEOM(1, 1), &folded);
            st_name = mds_cat_ns_lookup(cat, scratch.fileid, "g", &cur);
            st_cur = mds_cat_ns_getattr(cat, c2.fileid, &seen);
            (void)mds_cat_gc_count(cat, &gc_after);
            (void)snprintf(detail, sizeof(detail),
                           "remove_known_gc(stale) -> %s folded=%d; name -> "
                           "%s (fileid %s); current inode -> %s; gc rows %u->%u",
                           mds_status_str(st), (int)folded,
                           mds_status_str(st_name),
                           (st_name == MDS_OK && cur.fileid == c2.fileid)
                               ? "current" : "other/none",
                           mds_status_str(st_cur), gc_before, gc_after);
            probe_result(p, st == MDS_ERR_STALE && !folded &&
                         st_name == MDS_OK && cur.fileid == c2.fileid &&
                         st_cur == MDS_OK && gc_after == gc_before,
                         "stale remove_known_gc", detail);
            (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "g");
        } else {
            probe_result(p, false, "stale remove_known_gc", "setup failed");
        }
    }

    /* Wrong-inode variant: the snapshot's inode is still ALIVE under a
     * second name ("h") while "g" was re-created as another inode.  The
     * dirent guard must refuse (STALE); "g" keeps c2, c1 keeps "h" and
     * its data (no GC row for it). */
    {
        struct mds_inode c1, c2, cur;
        struct mds_ds_map_entry gc_entry;
        uint32_t gc_before = 0, gc_after = 0;
        bool folded = false;

        if (mk(cat, scratch.fileid, "g", MDS_FTYPE_REG, &c1) &&
            mds_cat_ns_link(cat, NULL, scratch.fileid, "h", c1.fileid) ==
                MDS_OK &&
            mds_cat_ns_remove(cat, NULL, scratch.fileid, "g") == MDS_OK &&
            mk(cat, scratch.fileid, "g", MDS_FTYPE_REG, &c2) &&
            mds_cat_gc_count(cat, &gc_before) == MDS_OK) {
            enum mds_status st_name;
            enum mds_status st_alive;
            uint32_t c1_nlink = 0;

            memset(&gc_entry, 0, sizeof(gc_entry));
            gc_entry.ds_id = 1;
            gc_entry.nfs_fh_len = 4;
            gc_entry.nfs_fh[0] = 0xAB;
            /* c1 is the CREATE-time snapshot: nlink 1, i.e. "final". */
            st = mds_cat_ns_remove_known_gc(cat, NULL, scratch.fileid, "g",
                                            &c1, 1, &gc_entry, 1,
                                            MDS_GC_SWEEP_GEOM(1, 1), &folded);
            st_name = mds_cat_ns_lookup(cat, scratch.fileid, "g", &cur);
            st_alive = mds_cat_ns_getattr(cat, c1.fileid, &seen);
            if (st_alive == MDS_OK) {
                c1_nlink = seen.nlink;
            }
            (void)mds_cat_gc_count(cat, &gc_after);
            (void)snprintf(detail, sizeof(detail),
                           "remove_known_gc(c1 snapshot) -> %s folded=%d; g -> "
                           "%s; c1 -> %s nlink=%u; gc rows %u->%u",
                           mds_status_str(st), (int)folded,
                           (st_name == MDS_OK && cur.fileid == c2.fileid)
                               ? "c2" : "other/none",
                           mds_status_str(st_alive), (unsigned)c1_nlink,
                           gc_before, gc_after);
            probe_result(p, st == MDS_ERR_STALE && !folded &&
                         st_name == MDS_OK && cur.fileid == c2.fileid &&
                         st_alive == MDS_OK && c1_nlink == 1 &&
                         gc_after == gc_before,
                         "stale remove_known_gc, inode alive elsewhere",
                         detail);
            (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "g");
            (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "h");
        } else {
            probe_result(p, false,
                         "stale remove_known_gc, inode alive elsewhere",
                         "setup failed");
        }
    }

    /* Stale link-count shape: the name still resolves to the snapshot's
     * inode, but a LINK landed since (snapshot says final, live nlink
     * 2), then the other name was removed (snapshot says non-final,
     * live nlink 1).  Both are refused with STALE and nothing changes;
     * a matching snapshot then removes the last link. */
    {
        struct mds_inode c1, snap2;
        struct mds_ds_map_entry gc_entry;
        uint32_t gc_before = 0, gc_after = 0;
        bool folded = false;

        if (mk(cat, scratch.fileid, "g", MDS_FTYPE_REG, &c1) &&
            mds_cat_ns_link(cat, NULL, scratch.fileid, "h", c1.fileid) ==
                MDS_OK &&
            mds_cat_gc_count(cat, &gc_before) == MDS_OK) {
            enum mds_status st_final_stale;
            enum mds_status st_nonfinal_stale = MDS_ERR_IO;
            enum mds_status st_fresh = MDS_ERR_IO;
            uint32_t nlink_after_first = 0;
            uint32_t nlink_after_second = 0;
            bool folded_first;
            bool folded_second = true;

            memset(&gc_entry, 0, sizeof(gc_entry));
            gc_entry.ds_id = 1;
            gc_entry.nfs_fh_len = 4;
            gc_entry.nfs_fh[0] = 0xAB;
            st_final_stale = mds_cat_ns_remove_known_gc(
                cat, NULL, scratch.fileid, "g", &c1, 1, &gc_entry, 1,
                MDS_GC_SWEEP_GEOM(1, 1), &folded);
            folded_first = folded;
            if (mds_cat_ns_getattr(cat, c1.fileid, &seen) == MDS_OK) {
                nlink_after_first = seen.nlink;
            }
            if (mds_cat_ns_getattr(cat, c1.fileid, &snap2) == MDS_OK &&
                mds_cat_ns_remove(cat, NULL, scratch.fileid, "h") == MDS_OK) {
                st_nonfinal_stale = mds_cat_ns_remove_known_gc(
                    cat, NULL, scratch.fileid, "g", &snap2, 1, &gc_entry, 1,
                    MDS_GC_SWEEP_GEOM(1, 1), &folded_second);
                if (mds_cat_ns_getattr(cat, c1.fileid, &seen) == MDS_OK) {
                    nlink_after_second = seen.nlink;
                }
                if (mds_cat_ns_getattr(cat, c1.fileid, &snap2) == MDS_OK) {
                    st_fresh = mds_cat_ns_remove_known_gc(
                        cat, NULL, scratch.fileid, "g", &snap2, 1,
                        &gc_entry, 1, MDS_GC_SWEEP_GEOM(1, 1), &folded);
                }
            }
            (void)mds_cat_gc_count(cat, &gc_after);
            (void)snprintf(detail, sizeof(detail),
                           "final-shape stale -> %s (nlink %u); non-final-shape "
                           "stale -> %s (nlink %u); fresh -> %s folded=%d; "
                           "inode -> %s; gc rows %u->%u",
                           mds_status_str(st_final_stale), nlink_after_first,
                           mds_status_str(st_nonfinal_stale),
                           nlink_after_second, mds_status_str(st_fresh),
                           (int)folded,
                           mds_status_str(mds_cat_ns_getattr(cat, c1.fileid,
                                                             &seen)),
                           gc_before, gc_after);
            probe_result(p, st_final_stale == MDS_ERR_STALE && !folded_first &&
                         nlink_after_first == 2 &&
                         st_nonfinal_stale == MDS_ERR_STALE &&
                         !folded_second && nlink_after_second == 1 &&
                         st_fresh == MDS_OK && folded &&
                         mds_cat_ns_getattr(cat, c1.fileid, &seen) ==
                             MDS_ERR_NOTFOUND &&
                         gc_after == gc_before + 1,
                         "stale remove_known_gc, link count moved", detail);
            (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "g");
            (void)mds_cat_ns_remove(cat, NULL, scratch.fileid, "h");
        } else {
            probe_result(p, false, "stale remove_known_gc, link count moved",
                         "setup failed");
        }
    }

    st = mds_cat_ns_remove(cat, NULL, MDS_FILEID_ROOT, scratch_name);
    (void)snprintf(detail, sizeof(detail), "scratch rmdir -> %s",
                   mds_status_str(st));
    probe_result(p, st == MDS_OK, "cleanup", detail);
}

/* -----------------------------------------------------------------------
 * Namespace inspection: the store's answers for one directory, one
 * name or one inode, so a suspected dangling dirent (row present,
 * inode gone) can be told apart from a client-cache artefact.
 * ----------------------------------------------------------------------- */

static void print_inode(const struct mds_inode *ino)
{
    (void)printf("fileid=%" PRIu64 " type=%u nlink=%" PRIu32 " size=%" PRIu64
                 " change=%" PRIu64 " gen=%" PRIu64 " flags=0x%" PRIx32
                 " parent=%" PRIu64 "\n",
                 ino->fileid, (unsigned)ino->type, ino->nlink, ino->size,
                 ino->change, ino->generation, ino->flags,
                 ino->parent_fileid);
}

struct readdir_inspect_ctx {
    struct mds_catalogue *cat;
    uint32_t entries;
    uint32_t dangling;
};

static int readdir_inspect_cb(const struct mds_cat_dirent *entry, void *arg)
{
    struct readdir_inspect_ctx *c = arg;
    struct mds_inode ino;
    enum mds_status st = mds_cat_ns_getattr(c->cat, entry->fileid, &ino);

    c->entries++;
    if (st != MDS_OK) {
        c->dangling++;
    }
    (void)printf("  name=%s child_fileid=%" PRIu64 " type=%u inode -> %s%s\n",
                 entry->name, entry->fileid, (unsigned)entry->type,
                 mds_status_str(st),
                 (st == MDS_OK) ? "" : "  (DANGLING DIRENT)");
    return 0;
}

/* -----------------------------------------------------------------------
 * scrub -- every dirent of root and of the conformance scratch
 * directories is read back through its inode.  A dirent whose inode
 * is gone is the D1 symptom (a final unlink that raced a LINK / RENAME
 * of the same inode); the census after each conformance run pins the
 * run that produced one.
 * ----------------------------------------------------------------------- */

#define SCRUB_PAGE 4096

struct scrub_page {
    uint32_t count;
    uint8_t  type[SCRUB_PAGE];
    uint64_t fid[SCRUB_PAGE];
    char     name[SCRUB_PAGE][MDS_MAX_NAME + 1];
};

struct scrub_totals {
    uint32_t dirs;
    uint32_t entries;
    uint32_t dangling;
};

static int scrub_collect_cb(const struct mds_cat_dirent *entry, void *arg)
{
    struct scrub_page *page = arg;

    if (page->count >= SCRUB_PAGE) {
        return 1;
    }
    page->type[page->count] = entry->type;
    page->fid[page->count] = entry->fileid;
    (void)snprintf(page->name[page->count], sizeof(page->name[0]), "%s",
                   entry->name);
    page->count++;
    return 0;
}

static bool scrub_is_scratch(const char *name)
{
    return strncmp(name, "conf-", 5) == 0 ||
           strncmp(name, "lab-ns-", 7) == 0;
}

static void scrub_dir(struct mds_catalogue *cat, uint64_t dir,
                      const char *dir_name, int depth,
                      struct scrub_totals *t)
{
    struct scrub_page *page = calloc(1, sizeof(*page));
    struct mds_inode ino;
    enum mds_status st;
    uint32_t i;

    if (page == NULL) {
        return;
    }
    st = mds_cat_ns_readdir(cat, dir, NULL, SCRUB_PAGE, NULL, scrub_collect_cb,
                            page);
    if (st != MDS_OK) {
        (void)printf("  readdir(%" PRIu64 " %s) -> %s\n", dir, dir_name,
                     mds_status_str(st));
        free(page);
        return;
    }
    t->dirs++;
    for (i = 0; i < page->count; i++) {
        st = mds_cat_ns_getattr(cat, page->fid[i], &ino);
        t->entries++;
        if (st != MDS_OK) {
            t->dangling++;
            (void)printf("  DANGLING parent=%" PRIu64 " (%s) name=%s "
                         "child_fileid=%" PRIu64 " type=%u inode -> %s\n",
                         dir, dir_name, page->name[i], page->fid[i],
                         (unsigned)page->type[i], mds_status_str(st));
            continue;
        }
        if (page->type[i] == (uint8_t)MDS_FTYPE_DIR && depth > 0 &&
            (dir != MDS_FILEID_ROOT || scrub_is_scratch(page->name[i]))) {
            scrub_dir(cat, page->fid[i], page->name[i], depth - 1, t);
        }
    }
    free(page);
}

static int run_scrub(struct mds_catalogue *cat)
{
    struct scrub_totals t = { 0, 0, 0 };

    (void)printf("scrub: root + conf-* / lab-ns-* scratch dirs (2 levels)\n");
    scrub_dir(cat, MDS_FILEID_ROOT, "/", 2, &t);
    (void)printf("scrub: %" PRIu32 " dir(s), %" PRIu32 " dirent(s), %" PRIu32
                 " dangling\n", t.dirs, t.entries, t.dangling);
    return (t.dangling == 0) ? 0 : 2;
}

static int run_inspect(struct mds_catalogue *cat, int argc, char **argv)
{
    const char *cmd = argv[0];
    struct mds_inode ino;
    enum mds_status st;
    uint64_t fid;

    if (strcmp(cmd, "readdir") == 0 && argc == 2) {
        struct readdir_inspect_ctx c = { cat, 0, 0 };

        if (!parse_u64(argv[1], &fid)) {
            return usage();
        }
        st = mds_cat_ns_readdir(cat, fid, NULL, 0, NULL, readdir_inspect_cb,
                                &c);
        (void)printf("readdir(%" PRIu64 ") -> %s; %" PRIu32 " entries, %"
                     PRIu32 " dangling\n", fid, mds_status_str(st), c.entries,
                     c.dangling);
        return 0;
    }
    if (strcmp(cmd, "lookup") == 0 && argc == 3) {
        if (!parse_u64(argv[1], &fid)) {
            return usage();
        }
        st = mds_cat_ns_lookup(cat, fid, argv[2], &ino);
        (void)printf("lookup(%" PRIu64 ", %s) -> %s\n", fid, argv[2],
                     mds_status_str(st));
        if (st == MDS_OK) {
            print_inode(&ino);
        }
        return 0;
    }
    if (strcmp(cmd, "dirent-get") == 0 && argc == 3) {
        uint64_t child = 0;
        uint8_t type = 0;

        if (!parse_u64(argv[1], &fid)) {
            return usage();
        }
        st = mds_cat_dirent_get(cat, fid, argv[2], &child, &type);
        (void)printf("dirent_get(%" PRIu64 ", %s) -> %s", fid, argv[2],
                     mds_status_str(st));
        if (st == MDS_OK) {
            (void)printf(" child_fileid=%" PRIu64 " type=%u", child,
                         (unsigned)type);
        }
        (void)printf("\n");
        return 0;
    }
    if (strcmp(cmd, "getattr") == 0 && argc == 2) {
        if (!parse_u64(argv[1], &fid)) {
            return usage();
        }
        st = mds_cat_ns_getattr(cat, fid, &ino);
        (void)printf("getattr(%" PRIu64 ") -> %s\n", fid, mds_status_str(st));
        if (st == MDS_OK) {
            print_inode(&ino);
        }
        return 0;
    }
    return usage();
}

/* ----------------------------------------------------------------------- */

static int run(struct mds_catalogue *cat, int argc, char **argv)
{
    const char *cmd = argv[0];
    enum mds_status st;

    if (strcmp(cmd, "readdir") == 0 || strcmp(cmd, "lookup") == 0 ||
        strcmp(cmd, "dirent-get") == 0 || strcmp(cmd, "getattr") == 0) {
        return run_inspect(cat, argc, argv);
    }
    if (strcmp(cmd, "scrub") == 0 && argc == 1) {
        return run_scrub(cat);
    }

    if (strcmp(cmd, "nodes") == 0) {
        st = mds_cluster_node_list(cat, print_node_cb, NULL);
        (void)printf("node_list -> %s\n", mds_status_str(st));
        return 0;
    }
    if (strcmp(cmd, "partitions") == 0) {
        st = mds_cluster_partition_list(cat, print_partition_cb, NULL);
        (void)printf("partition_list -> %s\n", mds_status_str(st));
        return 0;
    }
    if (strcmp(cmd, "register") == 0 && argc == 4) {
        uint32_t id;
        uint64_t epoch;

        if (!parse_u32(argv[1], &id) || !parse_u64(argv[2], &epoch)) {
            return usage();
        }
        st = mds_cluster_node_register(cat, id, epoch, argv[3], 2049, 50051);
        (void)printf("node_register(%" PRIu32 ", %" PRIu64 ") -> %s\n", id,
                     epoch, mds_status_str(st));
        return 0;
    }
    if (strcmp(cmd, "heartbeat") == 0 && argc == 3) {
        uint32_t id;
        uint64_t epoch;

        if (!parse_u32(argv[1], &id) || !parse_u64(argv[2], &epoch)) {
            return usage();
        }
        st = mds_cluster_node_heartbeat(cat, id, epoch);
        (void)printf("node_heartbeat(%" PRIu32 ", %" PRIu64 ") -> %s\n", id,
                     epoch, mds_status_str(st));
        return 0;
    }
    if (strcmp(cmd, "deregister") == 0 && argc == 3) {
        uint32_t id;
        uint64_t epoch;

        if (!parse_u32(argv[1], &id) || !parse_u64(argv[2], &epoch)) {
            return usage();
        }
        st = mds_cluster_node_deregister(cat, id, epoch);
        (void)printf("node_deregister(%" PRIu32 ", %" PRIu64 ") -> %s\n", id,
                     epoch, mds_status_str(st));
        return 0;
    }
    if (strcmp(cmd, "scan-stale") == 0 && argc == 2) {
        uint64_t stale_ms;
        uint64_t now = now_realtime_ns();
        uint64_t threshold;

        if (!parse_u64(argv[1], &stale_ms) || now == 0) {
            return usage();
        }
        threshold = (now > stale_ms * 1000000ULL) ? now - stale_ms * 1000000ULL
                                                  : 0;
        st = mds_cluster_node_scan_stale(cat, threshold, print_stale_cb, NULL);
        (void)printf("node_scan_stale(threshold=%" PRIu64 ") -> %s\n",
                     threshold, mds_status_str(st));
        return 0;
    }
    if (strcmp(cmd, "partition-put") == 0 && argc == 6) {
        uint32_t pid, owner, state, insert_only;

        if (!parse_u32(argv[1], &pid) || !parse_u32(argv[2], &owner) ||
            !parse_u32(argv[3], &state) || state > UINT8_MAX ||
            !parse_u32(argv[5], &insert_only) || insert_only > 1) {
            return usage();
        }
        st = mds_cluster_partition_put(cat, pid, owner, (uint8_t)state,
                                       argv[4], insert_only == 1);
        (void)printf("partition_put(%" PRIu32 ", owner %" PRIu32
                     ", insert_only=%u) -> %s\n",
                     pid, owner, insert_only, mds_status_str(st));
        return 0;
    }
    if (strcmp(cmd, "partition-cas") == 0 && argc == 5) {
        uint32_t pid, expected, owner, state;

        if (!parse_u32(argv[1], &pid) || !parse_u32(argv[2], &expected) ||
            !parse_u32(argv[3], &owner) || !parse_u32(argv[4], &state) ||
            state > UINT8_MAX) {
            return usage();
        }
        st = mds_cluster_partition_cas(cat, pid, expected, owner,
                                       (uint8_t)state);
        (void)printf("partition_cas(%" PRIu32 ", expected %" PRIu32
                     " -> owner %" PRIu32 ") -> %s\n",
                     pid, expected, owner, mds_status_str(st));
        return 0;
    }
    if (strcmp(cmd, "recovery-list") == 0 && argc == 2) {
        uint32_t owner;

        if (!parse_u32(argv[1], &owner)) {
            return usage();
        }
        st = mds_coord_recovery_list(cat, owner, print_recovery_cb, NULL);
        (void)printf("recovery_list(owner %" PRIu32 ") -> %s\n", owner,
                     mds_status_str(st));
        return 0;
    }
    if (strcmp(cmd, "journal-list") == 0 && argc == 1) {
        st = mds_coord_journal_scan(cat, print_journal_cb, NULL);
        (void)printf("journal_scan -> %s\n", mds_status_str(st));
        return 0;
    }
    if (strcmp(cmd, "journal-del") == 0 && argc == 3) {
        uint64_t txn_id;
        uint32_t role;

        if (!parse_u64(argv[1], &txn_id) || !parse_u32(argv[2], &role) ||
            role > UINT8_MAX) {
            return usage();
        }
        st = mds_coord_journal_del(cat, NULL, txn_id, (uint8_t)role);
        (void)printf("journal_del(%" PRIu64 ", role %u) -> %s\n", txn_id,
                     role, mds_status_str(st));
        return 0;
    }
    if (strcmp(cmd, "ns-semantics") == 0 && argc == 1) {
        struct probe p = { 0 };

        (void)printf("ns-semantics against the live store:\n");
        ns_semantics(cat, &p);
        (void)printf("ns-semantics: %u probe(s) failed\n", p.failed);
        return (p.failed == 0) ? 0 : 2;
    }
    return usage();
}

int main(int argc, char **argv)
{
    uint32_t mds_id = LAB_DEFAULT_MDS_ID;
    struct mds_catalogue *cat;
    int rc;

    argv++;
    argc--;
    if (argc >= 2 && strcmp(argv[0], "--mds-id") == 0) {
        if (!parse_u32(argv[1], &mds_id) || mds_id == 0 ||
            mds_id > MDS_MAX_NODES) {
            return usage();
        }
        argv += 2;
        argc -= 2;
    }
    if (argc < 1) {
        return usage();
    }

    cat = open_rondb(mds_id);
    if (cat == NULL) {
        return 1;
    }
    rc = run(cat, argc, argv);
    mds_catalogue_close(cat);
    return rc;
}
