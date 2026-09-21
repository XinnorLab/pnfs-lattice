/*
 * Copyright (c) 2026 PeakAIO
 * SPDX-License-Identifier: MIT
 *
 * fdb_codec.c -- Value formats of the FoundationDB catalogue backend.
 *
 * See fdb_codec.h for the layouts.  Encoders write through a bounded
 * cursor that refuses to run past the caller's buffer; decoders read
 * through a cursor that refuses to run past the value and then require
 * the value to be consumed exactly, so a truncated or padded value is
 * rejected rather than partially trusted.
 */

#include <string.h>

#include "fdb_codec.h"

#define NS_PER_SEC 1000000000LL

/* -----------------------------------------------------------------------
 * Little-endian integers
 * ----------------------------------------------------------------------- */

void fdb_le64_put(uint8_t *dst, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) {
        dst[i] = (uint8_t)(v >> (8U * i));
    }
}

uint64_t fdb_le64_get(const uint8_t *src)
{
    uint64_t v = 0;

    for (unsigned i = 0; i < 8; i++) {
        v |= (uint64_t)src[i] << (8U * i);
    }
    return v;
}

void fdb_le32_put(uint8_t *dst, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++) {
        dst[i] = (uint8_t)(v >> (8U * i));
    }
}

uint32_t fdb_le32_get(const uint8_t *src)
{
    uint32_t v = 0;

    for (unsigned i = 0; i < 4; i++) {
        v |= (uint32_t)src[i] << (8U * i);
    }
    return v;
}

bool fdb_le64_decode(const uint8_t *src, size_t len, uint64_t *out)
{
    if (src == NULL || out == NULL || len != 8) {
        return false;
    }
    *out = fdb_le64_get(src);
    return true;
}

int64_t fdb_ts_to_ns(struct timespec ts)
{
    /* Saturate rather than overflow for absurd tv_sec values (~292
     * years either side of the epoch fit). */
    if (ts.tv_sec > INT64_MAX / NS_PER_SEC - 1) {
        return INT64_MAX;
    }
    if (ts.tv_sec < INT64_MIN / NS_PER_SEC + 1) {
        return INT64_MIN;
    }
    return (int64_t)ts.tv_sec * NS_PER_SEC + (int64_t)ts.tv_nsec;
}

struct timespec fdb_ns_to_ts(int64_t ns)
{
    struct timespec ts;
    int64_t sec = ns / NS_PER_SEC;
    int64_t rem = ns % NS_PER_SEC;

    if (rem < 0) {
        rem += NS_PER_SEC;
        sec -= 1;
    }
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)rem;
    return ts;
}

/* -----------------------------------------------------------------------
 * Bounded cursors
 * ----------------------------------------------------------------------- */

struct wcur {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    bool     fail;
};

struct rcur {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    bool           fail;
};

static void w_bytes(struct wcur *w, const void *p, size_t n)
{
    if (w->fail || n > w->cap - w->pos) {
        w->fail = true;
        return;
    }
    if (n > 0) {
        memcpy(w->buf + w->pos, p, n);
    }
    w->pos += n;
}

static void w_u8(struct wcur *w, uint8_t v)
{
    w_bytes(w, &v, 1);
}

static void w_u16(struct wcur *w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };

    w_bytes(w, b, sizeof(b));
}

static void w_u32(struct wcur *w, uint32_t v)
{
    uint8_t b[4];

    fdb_le32_put(b, v);
    w_bytes(w, b, sizeof(b));
}

static void w_u64(struct wcur *w, uint64_t v)
{
    uint8_t b[8];

    fdb_le64_put(b, v);
    w_bytes(w, b, sizeof(b));
}

static void w_ts(struct wcur *w, struct timespec ts)
{
    w_u64(w, (uint64_t)(int64_t)ts.tv_sec);
    w_u32(w, (uint32_t)ts.tv_nsec);
}

static bool r_bytes(struct rcur *r, void *p, size_t n)
{
    if (r->fail || n > r->len - r->pos) {
        r->fail = true;
        return false;
    }
    if (n > 0) {
        memcpy(p, r->buf + r->pos, n);
    }
    r->pos += n;
    return true;
}

static uint8_t r_u8(struct rcur *r)
{
    uint8_t v = 0;

    (void)r_bytes(r, &v, 1);
    return v;
}

static uint16_t r_u16(struct rcur *r)
{
    uint8_t b[2] = { 0, 0 };

    (void)r_bytes(r, b, sizeof(b));
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t r_u32(struct rcur *r)
{
    uint8_t b[4] = { 0, 0, 0, 0 };

    (void)r_bytes(r, b, sizeof(b));
    return fdb_le32_get(b);
}

static uint64_t r_u64(struct rcur *r)
{
    uint8_t b[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    (void)r_bytes(r, b, sizeof(b));
    return fdb_le64_get(b);
}

/* Reads a timestamp; a tv_nsec outside [0, 1e9) marks the cursor failed. */
static struct timespec r_ts(struct rcur *r)
{
    struct timespec ts;
    uint32_t nsec;

    ts.tv_sec = (time_t)(int64_t)r_u64(r);
    nsec = r_u32(r);
    if (nsec >= (uint32_t)NS_PER_SEC) {
        r->fail = true;
        nsec = 0;
    }
    ts.tv_nsec = (long)nsec;
    return ts;
}

/* A decoder is complete only when it consumed the value exactly. */
static bool r_done(const struct rcur *r)
{
    return !r->fail && r->pos == r->len;
}

static bool type_ok(uint32_t t)
{
    return t >= (uint32_t)MDS_FTYPE_REG && t <= (uint32_t)MDS_FTYPE_SOCK;
}

/* -----------------------------------------------------------------------
 * Inode
 * ----------------------------------------------------------------------- */

bool fdb_inode_encode(const struct mds_inode *ino, uint8_t *buf, size_t cap, size_t *len)
{
    struct wcur w = { buf, cap, 0, false };

    if (ino == NULL || buf == NULL || len == NULL ||
        ino->inline_fh_len > MDS_NFS_FH_MAX || !type_ok((uint32_t)ino->type) ||
        (uint64_t)ino->atime.tv_nsec >= (uint64_t)NS_PER_SEC ||
        (uint64_t)ino->mtime.tv_nsec >= (uint64_t)NS_PER_SEC ||
        (uint64_t)ino->ctime.tv_nsec >= (uint64_t)NS_PER_SEC) {
        return false;
    }
    w_u8(&w, (uint8_t)FDB_INODE_VERSION);
    w_u8(&w, (uint8_t)ino->type);
    w_u16(&w, 0);
    w_u32(&w, ino->mode);
    w_u32(&w, ino->nlink);
    w_u32(&w, ino->flags);
    w_u64(&w, ino->fileid);
    w_u64(&w, ino->uid);
    w_u64(&w, ino->gid);
    w_u64(&w, ino->size);
    w_u64(&w, ino->space_used);
    w_ts(&w, ino->atime);
    w_ts(&w, ino->mtime);
    w_ts(&w, ino->ctime);
    w_u64(&w, ino->change);
    w_u64(&w, ino->generation);
    w_u64(&w, ino->create_verf);
    w_u64(&w, ino->parent_fileid);
    w_u32(&w, ino->synth_suid);
    w_u32(&w, ino->synth_sgid);
    w_u32(&w, ino->stripe_count);
    w_u32(&w, ino->stripe_unit);
    w_u32(&w, ino->mirror_count);
    w_u32(&w, ino->inline_ds_id);
    w_u32(&w, ino->inline_fh_len);
    w_bytes(&w, ino->inline_fh, ino->inline_fh_len);
    if (w.fail) {
        return false;
    }
    *len = w.pos;
    return true;
}

bool fdb_inode_decode(const uint8_t *buf, size_t len, struct mds_inode *out)
{
    struct rcur r = { buf, len, 0, false };
    struct mds_inode ino;
    uint8_t type;

    if (buf == NULL || out == NULL || len < FDB_INODE_ENC_FIXED || len > FDB_INODE_ENC_MAX) {
        return false;
    }
    memset(&ino, 0, sizeof(ino));
    if (r_u8(&r) != FDB_INODE_VERSION) {
        return false;
    }
    type = r_u8(&r);
    (void)r_u16(&r);
    ino.mode = r_u32(&r);
    ino.nlink = r_u32(&r);
    ino.flags = r_u32(&r);
    ino.fileid = r_u64(&r);
    ino.uid = r_u64(&r);
    ino.gid = r_u64(&r);
    ino.size = r_u64(&r);
    ino.space_used = r_u64(&r);
    ino.atime = r_ts(&r);
    ino.mtime = r_ts(&r);
    ino.ctime = r_ts(&r);
    ino.change = r_u64(&r);
    ino.generation = r_u64(&r);
    ino.create_verf = r_u64(&r);
    ino.parent_fileid = r_u64(&r);
    ino.synth_suid = r_u32(&r);
    ino.synth_sgid = r_u32(&r);
    ino.stripe_count = r_u32(&r);
    ino.stripe_unit = r_u32(&r);
    ino.mirror_count = r_u32(&r);
    ino.inline_ds_id = r_u32(&r);
    ino.inline_fh_len = r_u32(&r);
    if (r.fail || !type_ok(type) || ino.inline_fh_len > MDS_NFS_FH_MAX) {
        return false;
    }
    if (!r_bytes(&r, ino.inline_fh, ino.inline_fh_len) || !r_done(&r)) {
        return false;
    }
    ino.type = (enum mds_file_type)type;
    ino.ds_map = NULL;
    *out = ino;
    return true;
}

bool fdb_inode_is_dir(const struct mds_inode *ino)
{
    return ino != NULL && ino->type == MDS_FTYPE_DIR;
}

void fdb_inode_split(const struct mds_inode *in, struct mds_inode *blob_copy,
                     struct fdb_dir_counters *counters)
{
    *blob_copy = *in;
    memset(counters, 0, sizeof(*counters));
    if (!fdb_inode_is_dir(in)) {
        return;
    }
    counters->nlink = in->nlink;
    counters->change = in->change;
    counters->mtime_ns = (uint64_t)fdb_ts_to_ns(in->mtime);
    counters->ctime_ns = (uint64_t)fdb_ts_to_ns(in->ctime);
    /* Dead copies: anything that reads them by mistake sees zeros,
     * which is obviously wrong rather than plausibly stale. */
    blob_copy->nlink = 0;
    blob_copy->change = 0;
    blob_copy->mtime.tv_sec = 0;
    blob_copy->mtime.tv_nsec = 0;
    blob_copy->ctime.tv_sec = 0;
    blob_copy->ctime.tv_nsec = 0;
}

void fdb_inode_compose(struct mds_inode *io, const struct fdb_dir_counters *counters)
{
    int64_t nlink;

    if (!fdb_inode_is_dir(io)) {
        return;
    }
    /* The counter is an unsigned LE u64 that ADD may drive below zero
     * (two's complement wrap); read it as signed and clamp. */
    nlink = (int64_t)counters->nlink;
    if (nlink < 0) {
        io->nlink = 0;
    } else if (nlink > (int64_t)UINT32_MAX) {
        io->nlink = UINT32_MAX;
    } else {
        io->nlink = (uint32_t)nlink;
    }
    io->change = counters->change;
    io->mtime = fdb_ns_to_ts((int64_t)counters->mtime_ns);
    io->ctime = fdb_ns_to_ts((int64_t)counters->ctime_ns);
}

/* -----------------------------------------------------------------------
 * Dirent and dirent-sequence index
 * ----------------------------------------------------------------------- */

bool fdb_dirent_encode(const struct fdb_dirent_val *v, uint8_t *buf, size_t cap, size_t *len)
{
    struct wcur w = { buf, cap, 0, false };

    if (v == NULL || buf == NULL || len == NULL || !type_ok(v->type)) {
        return false;
    }
    w_u8(&w, (uint8_t)FDB_DIRENT_VERSION);
    w_u64(&w, v->child_fileid);
    w_u8(&w, v->type);
    w_u64(&w, v->seq);
    if (w.fail) {
        return false;
    }
    *len = w.pos;
    return true;
}

bool fdb_dirent_decode(const uint8_t *buf, size_t len, struct fdb_dirent_val *out)
{
    struct rcur r = { buf, len, 0, false };
    struct fdb_dirent_val v;

    if (buf == NULL || out == NULL || len != FDB_DIRENT_ENC_SIZE) {
        return false;
    }
    if (r_u8(&r) != FDB_DIRENT_VERSION) {
        return false;
    }
    v.child_fileid = r_u64(&r);
    v.type = r_u8(&r);
    v.seq = r_u64(&r);
    if (!r_done(&r) || !type_ok(v.type)) {
        return false;
    }
    *out = v;
    return true;
}

bool fdb_dirent_seq_encode(const struct fdb_dirent_seq_val *v, uint8_t *buf, size_t cap,
                           size_t *len)
{
    struct wcur w = { buf, cap, 0, false };
    size_t n;

    if (v == NULL || buf == NULL || len == NULL || !type_ok(v->type)) {
        return false;
    }
    n = strnlen(v->name, sizeof(v->name));
    if (n == 0 || n > MDS_MAX_NAME) {
        return false;
    }
    w_u8(&w, (uint8_t)FDB_DIRENT_SEQ_VERSION);
    w_u64(&w, v->child_fileid);
    w_u8(&w, v->type);
    w_bytes(&w, v->name, n);
    if (w.fail) {
        return false;
    }
    *len = w.pos;
    return true;
}

bool fdb_dirent_seq_decode(const uint8_t *buf, size_t len, struct fdb_dirent_seq_val *out)
{
    struct rcur r = { buf, len, 0, false };
    struct fdb_dirent_seq_val v;
    size_t n;

    if (buf == NULL || out == NULL || len <= FDB_DIRENT_SEQ_ENC_FIXED ||
        len > FDB_DIRENT_SEQ_ENC_MAX) {
        return false;
    }
    if (r_u8(&r) != FDB_DIRENT_SEQ_VERSION) {
        return false;
    }
    v.child_fileid = r_u64(&r);
    v.type = r_u8(&r);
    n = len - FDB_DIRENT_SEQ_ENC_FIXED;
    if (!r_bytes(&r, v.name, n) || !r_done(&r) || !type_ok(v.type) ||
        memchr(v.name, '\0', n) != NULL) {
        return false;
    }
    v.name[n] = '\0';
    *out = v;
    return true;
}

/* -----------------------------------------------------------------------
 * Stripe map rows
 * ----------------------------------------------------------------------- */

static bool stripe_hdr_ok(const struct fdb_stripe_hdr_val *v)
{
    return v->stripe_count >= 1 && v->stripe_count <= MDS_MAX_STRIPES &&
           v->mirror_count >= 1 && v->mirror_count <= MDS_MAX_MIRRORS &&
           v->stripe_unit != 0;
}

bool fdb_stripe_hdr_encode(const struct fdb_stripe_hdr_val *v, uint8_t *buf, size_t cap,
                           size_t *len)
{
    struct wcur w = { buf, cap, 0, false };

    if (v == NULL || buf == NULL || len == NULL || !stripe_hdr_ok(v)) {
        return false;
    }
    w_u8(&w, (uint8_t)FDB_STRIPE_HDR_VERSION);
    w_u32(&w, v->stripe_count);
    w_u32(&w, v->stripe_unit);
    w_u32(&w, v->mirror_count);
    if (w.fail) {
        return false;
    }
    *len = w.pos;
    return true;
}

bool fdb_stripe_hdr_decode(const uint8_t *buf, size_t len, struct fdb_stripe_hdr_val *out)
{
    struct rcur r = { buf, len, 0, false };
    struct fdb_stripe_hdr_val v;

    if (buf == NULL || out == NULL || len != FDB_STRIPE_HDR_ENC_SIZE) {
        return false;
    }
    if (r_u8(&r) != FDB_STRIPE_HDR_VERSION) {
        return false;
    }
    v.stripe_count = r_u32(&r);
    v.stripe_unit = r_u32(&r);
    v.mirror_count = r_u32(&r);
    if (!r_done(&r) || !stripe_hdr_ok(&v)) {
        return false;
    }
    *out = v;
    return true;
}

bool fdb_stripe_ent_encode(const struct mds_ds_map_entry *e, uint8_t *buf, size_t cap,
                           size_t *len)
{
    struct wcur w = { buf, cap, 0, false };

    if (e == NULL || buf == NULL || len == NULL || e->nfs_fh_len > MDS_NFS_FH_MAX) {
        return false;
    }
    w_u8(&w, (uint8_t)FDB_STRIPE_ENT_VERSION);
    w_u32(&w, e->ds_id);
    w_u32(&w, e->synth_suid);
    w_u32(&w, e->synth_sgid);
    w_u32(&w, e->nfs_fh_len);
    w_bytes(&w, e->nfs_fh, e->nfs_fh_len);
    if (w.fail) {
        return false;
    }
    *len = w.pos;
    return true;
}

bool fdb_stripe_ent_decode(const uint8_t *buf, size_t len, struct mds_ds_map_entry *out)
{
    struct rcur r = { buf, len, 0, false };
    struct mds_ds_map_entry e;

    if (buf == NULL || out == NULL || len < FDB_STRIPE_ENT_ENC_FIXED ||
        len > FDB_STRIPE_ENT_ENC_MAX) {
        return false;
    }
    memset(&e, 0, sizeof(e));
    if (r_u8(&r) != FDB_STRIPE_ENT_VERSION) {
        return false;
    }
    e.ds_id = r_u32(&r);
    e.synth_suid = r_u32(&r);
    e.synth_sgid = r_u32(&r);
    e.nfs_fh_len = r_u32(&r);
    if (r.fail || e.nfs_fh_len > MDS_NFS_FH_MAX) {
        return false;
    }
    if (!r_bytes(&r, e.nfs_fh, e.nfs_fh_len) || !r_done(&r)) {
        return false;
    }
    *out = e;
    return true;
}

/* -----------------------------------------------------------------------
 * GC queue row
 * ----------------------------------------------------------------------- */

bool fdb_gc_encode(const struct mds_gc_entry *e, uint8_t *buf, size_t cap, size_t *len)
{
    struct wcur w = { buf, cap, 0, false };

    if (e == NULL || buf == NULL || len == NULL || e->nfs_fh_len > MDS_NFS_FH_MAX) {
        return false;
    }
    w_u8(&w, (uint8_t)FDB_GC_VERSION);
    w_u64(&w, e->fileid);
    w_u32(&w, e->ds_id);
    w_u32(&w, e->owner_mds_id);
    w_u32(&w, e->sweep_hint);
    w_u32(&w, e->nfs_fh_len);
    w_bytes(&w, e->nfs_fh, e->nfs_fh_len);
    if (w.fail) {
        return false;
    }
    *len = w.pos;
    return true;
}

bool fdb_gc_decode(const uint8_t *buf, size_t len, struct mds_gc_entry *out)
{
    struct rcur r = { buf, len, 0, false };
    struct mds_gc_entry e;

    if (buf == NULL || out == NULL || len < FDB_GC_ENC_FIXED || len > FDB_GC_ENC_MAX) {
        return false;
    }
    memset(&e, 0, sizeof(e));
    if (r_u8(&r) != FDB_GC_VERSION) {
        return false;
    }
    e.fileid = r_u64(&r);
    e.ds_id = r_u32(&r);
    e.owner_mds_id = r_u32(&r);
    e.sweep_hint = r_u32(&r);
    e.nfs_fh_len = r_u32(&r);
    if (r.fail || e.nfs_fh_len > MDS_NFS_FH_MAX) {
        return false;
    }
    if (!r_bytes(&r, e.nfs_fh, e.nfs_fh_len) || !r_done(&r)) {
        return false;
    }
    *out = e;
    return true;
}
