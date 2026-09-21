/*
 * Copyright (c) 2026 PeakAIO
 * SPDX-License-Identifier: MIT
 *
 * fdb_codec.h -- Value formats of the FoundationDB catalogue backend.
 *
 * Every value is an explicit, versioned, little-endian byte layout;
 * struct bytes are never written raw, so the on-disk format does not
 * depend on the compiler's padding or the host's endianness and can be
 * evolved by bumping the version byte.  Every decoder rejects a short
 * buffer, an oversized buffer, an unknown version and any field outside
 * its documented bound; a rejected value is reported as MDS_ERR_IO by
 * the slot that read it, never silently defaulted.
 *
 * Integer helpers: fdb_le64_put/get and fdb_le32_put/get are the
 * little-endian counterparts of the big-endian key helpers in
 * endian_helpers.h; FoundationDB atomic ADD operates on little-endian
 * integers, so every counter value (META allocators, directory side
 * keys, witness sequence) uses them.
 *
 * Inode blob (version 1), FDB_INODE_ENC_FIXED + inline_fh_len bytes:
 *   u8 version | u8 type | u16 reserved(0) | u32 mode | u32 nlink |
 *   u32 flags | u64 fileid | u64 uid | u64 gid | u64 size |
 *   u64 space_used | i64 atime_sec u32 atime_nsec | i64 mtime_sec
 *   u32 mtime_nsec | i64 ctime_sec u32 ctime_nsec | u64 change |
 *   u64 generation | u64 create_verf | u64 parent_fileid |
 *   u32 synth_suid | u32 synth_sgid | u32 stripe_count |
 *   u32 stripe_unit | u32 mirror_count | u32 inline_ds_id |
 *   u32 inline_fh_len | u8 inline_fh[inline_fh_len]
 * ds_map is never serialised (decoded inodes carry ds_map = NULL).
 *
 * Directory split: a directory's nlink, change, mtime and ctime live
 * in the INODE side keys (fdb_keys.h) so CREATE/REMOVE can advance them
 * with blind atomic ADD / set instead of rewriting the blob.  The blob
 * copies of those four fields are DEAD: fdb_inode_split zeroes them
 * before the blob is encoded and fdb_inode_compose overwrites them
 * from the side keys after the blob is decoded.  Files keep every
 * field in the blob.
 */

#ifndef FDB_CODEC_H
#define FDB_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "pnfs_mds.h"

/** Current inode blob version. */
#define FDB_INODE_VERSION     1U
/** Bytes before the variable-length inline file handle. */
#define FDB_INODE_ENC_FIXED   152U
/** Largest encoded inode (fixed part + MDS_NFS_FH_MAX). */
#define FDB_INODE_ENC_MAX     (FDB_INODE_ENC_FIXED + MDS_NFS_FH_MAX)

/** DIRENT value: u8 version | u64 child_fileid | u8 type | u64 seq. */
#define FDB_DIRENT_VERSION    1U
#define FDB_DIRENT_ENC_SIZE   18U

/** DIRENT_SEQ value: u8 version | u64 child_fileid | u8 type | name bytes. */
#define FDB_DIRENT_SEQ_VERSION   1U
#define FDB_DIRENT_SEQ_ENC_FIXED 10U
#define FDB_DIRENT_SEQ_ENC_MAX   (FDB_DIRENT_SEQ_ENC_FIXED + MDS_MAX_NAME)

/** STRIPE_HDR value: u8 version | u32 stripe_count | u32 stripe_unit |
 *  u32 mirror_count. */
#define FDB_STRIPE_HDR_VERSION  1U
#define FDB_STRIPE_HDR_ENC_SIZE 13U

/** STRIPE_ENT value: u8 version | u32 ds_id | u32 synth_suid |
 *  u32 synth_sgid | u32 nfs_fh_len | u8 nfs_fh[nfs_fh_len]. */
#define FDB_STRIPE_ENT_VERSION   1U
#define FDB_STRIPE_ENT_ENC_FIXED 17U
#define FDB_STRIPE_ENT_ENC_MAX   (FDB_STRIPE_ENT_ENC_FIXED + MDS_NFS_FH_MAX)

/** GC value: u8 version | u64 fileid | u32 ds_id | u32 owner_mds_id |
 *  u32 sweep_hint | u32 nfs_fh_len | u8 nfs_fh[nfs_fh_len]. */
#define FDB_GC_VERSION   1U
#define FDB_GC_ENC_FIXED 25U
#define FDB_GC_ENC_MAX   (FDB_GC_ENC_FIXED + MDS_NFS_FH_MAX)

_Static_assert(FDB_INODE_ENC_MAX < 100000U && FDB_GC_ENC_MAX < 100000U,
               "FoundationDB values must stay under 100 KB");

/* -----------------------------------------------------------------------
 * Little-endian integers
 * ----------------------------------------------------------------------- */

void     fdb_le64_put(uint8_t *dst, uint64_t v);
uint64_t fdb_le64_get(const uint8_t *src);
void     fdb_le32_put(uint8_t *dst, uint32_t v);
uint32_t fdb_le32_get(const uint8_t *src);

/**
 * Decode a counter value: exactly 8 bytes.  A shorter or longer value
 * is corrupt (returns false; *out untouched).
 */
bool fdb_le64_decode(const uint8_t *src, size_t len, uint64_t *out);

/** Nanoseconds since the epoch of @p ts as a signed 64-bit value. */
int64_t fdb_ts_to_ns(struct timespec ts);

/** Inverse of fdb_ts_to_ns (tv_nsec normalised to [0, 1e9)). */
struct timespec fdb_ns_to_ts(int64_t ns);

/* -----------------------------------------------------------------------
 * Inode
 * ----------------------------------------------------------------------- */

/**
 * Encode @p ino into @p buf.
 *
 * @param ino  Inode; inline_fh_len above MDS_NFS_FH_MAX is rejected.
 * @param buf  At least FDB_INODE_ENC_MAX bytes.
 * @param cap  Capacity of @p buf.
 * @param len  Receives the encoded length.
 * @return true on success; false when a field is out of bounds or the
 *         buffer is too small (nothing is written then).
 */
bool fdb_inode_encode(const struct mds_inode *ino, uint8_t *buf, size_t cap,
                      size_t *len);

/**
 * Decode an inode blob.  Rejects a wrong version, a buffer shorter than
 * the fixed part, an inline_fh_len above MDS_NFS_FH_MAX, a length that
 * is not exactly fixed + inline_fh_len, a type outside enum
 * mds_file_type and a tv_nsec at or above 1e9.  On success *out is
 * fully written with ds_map = NULL.
 */
bool fdb_inode_decode(const uint8_t *buf, size_t len, struct mds_inode *out);

/** True when @p type is a directory (the split applies). */
bool fdb_inode_is_dir(const struct mds_inode *ino);

/**
 * Directory side values as they are stored (LE u64 each).  nlink and
 * change are plain counters; mtime_ns / ctime_ns are fdb_ts_to_ns of
 * the timestamps, stored as two's complement.
 */
struct fdb_dir_counters {
    uint64_t nlink;
    uint64_t change;
    uint64_t mtime_ns;
    uint64_t ctime_ns;
};

/**
 * Split a directory inode for writing: fill @p counters from @p in and
 * write into @p blob_copy a copy of @p in whose nlink, change, mtime and
 * ctime are zeroed (dead).  For a non-directory @p blob_copy is an
 * unmodified copy and @p counters is zeroed.
 */
void fdb_inode_split(const struct mds_inode *in, struct mds_inode *blob_copy,
                     struct fdb_dir_counters *counters);

/**
 * Compose a directory inode after reading: overwrite the four dead
 * blob fields of @p io from @p counters.  nlink is clamped to
 * [0, UINT32_MAX] (a counter driven below zero reads as 0).  No-op
 * for a non-directory.
 */
void fdb_inode_compose(struct mds_inode *io, const struct fdb_dir_counters *counters);

/* -----------------------------------------------------------------------
 * Dirent and dirent-sequence index
 * ----------------------------------------------------------------------- */

struct fdb_dirent_val {
    uint64_t child_fileid;
    uint64_t seq;       /**< The entry's READDIR cookie / DIRENT_SEQ key. */
    uint8_t  type;      /**< enum mds_file_type of the child. */
};

/** Encode into exactly FDB_DIRENT_ENC_SIZE bytes (cap must allow it). */
bool fdb_dirent_encode(const struct fdb_dirent_val *v, uint8_t *buf, size_t cap,
                       size_t *len);
/** Decode; exactly FDB_DIRENT_ENC_SIZE bytes of the current version. */
bool fdb_dirent_decode(const uint8_t *buf, size_t len, struct fdb_dirent_val *out);

struct fdb_dirent_seq_val {
    uint64_t child_fileid;
    uint8_t  type;
    char     name[MDS_MAX_NAME + 1]; /**< NUL-terminated on decode. */
};

/** Encode; the name must be 1..MDS_MAX_NAME bytes. */
bool fdb_dirent_seq_encode(const struct fdb_dirent_seq_val *v, uint8_t *buf, size_t cap,
                           size_t *len);
/** Decode; rejects an empty or over-long name. */
bool fdb_dirent_seq_decode(const uint8_t *buf, size_t len, struct fdb_dirent_seq_val *out);

/* -----------------------------------------------------------------------
 * Stripe map rows
 * ----------------------------------------------------------------------- */

struct fdb_stripe_hdr_val {
    uint32_t stripe_count;
    uint32_t stripe_unit;
    uint32_t mirror_count;
};

/** Encode; stripe_count in 1..MDS_MAX_STRIPES, mirror_count in
 *  1..MDS_MAX_MIRRORS, stripe_unit non-zero. */
bool fdb_stripe_hdr_encode(const struct fdb_stripe_hdr_val *v, uint8_t *buf, size_t cap,
                           size_t *len);
bool fdb_stripe_hdr_decode(const uint8_t *buf, size_t len, struct fdb_stripe_hdr_val *out);

/** Encode one struct mds_ds_map_entry (nfs_fh_len <= MDS_NFS_FH_MAX). */
bool fdb_stripe_ent_encode(const struct mds_ds_map_entry *e, uint8_t *buf, size_t cap,
                           size_t *len);
bool fdb_stripe_ent_decode(const uint8_t *buf, size_t len, struct mds_ds_map_entry *out);

/* -----------------------------------------------------------------------
 * GC queue row
 * ----------------------------------------------------------------------- */

/** Encode a GC row; gc_seq is the key, not part of the value. */
bool fdb_gc_encode(const struct mds_gc_entry *e, uint8_t *buf, size_t cap, size_t *len);
/** Decode; the caller sets out->gc_seq from the key. */
bool fdb_gc_decode(const uint8_t *buf, size_t len, struct mds_gc_entry *out);

#endif /* FDB_CODEC_H */
