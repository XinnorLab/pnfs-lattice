/*
 * Copyright (c) 2026 PeakAIO
 * SPDX-License-Identifier: MIT
 *
 * catalogue_fdb_internal.h -- helpers shared by the FoundationDB
 * backend's translation units (catalogue_fdb_ns.c, catalogue_fdb_ext.c).
 *
 * Backend-private: nothing here is part of the library's public
 * headers (include/catalogue_fdb.h declares only what catalogue_fdb.c
 * needs to assemble the vtables).  Every helper is a transaction body
 * helper in the sense of fdb_txn.h: it works on the caller's
 * transaction, returns 0 or an fdb_error_t for the body to hand back
 * to the runner, and issues blind writes only -- the caller's body
 * owns every read and every decision.
 */

#ifndef CATALOGUE_FDB_INTERNAL_H
#define CATALOGUE_FDB_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "pnfs_mds.h"
#include "fdb_codec.h"
#include "fdb_keys.h"
#include "fdb_txn.h"

/* -----------------------------------------------------------------------
 * GC queue rows.  A row and its GC_BY_OWNER key (fdb_keys.h) are one
 * unit: written together by gc_enqueue and by the fused remove, cleared
 * together by gc_dequeue, never one without the other -- that is what
 * lets an owner-scoped peek trust the index without a repair pass.
 * ----------------------------------------------------------------------- */

/**
 * Blind-set the GC row @p e (key GC + e->gc_seq, value fdb_gc codec) and
 * its index key GC_BY_OWNER + e->owner_mds_id + e->gc_seq (empty value)
 * in @p tr.
 *
 * @param tr  The body's transaction.
 * @param p   The handle's key prefix.
 * @param e   Row to write; gc_seq and owner_mds_id must be final.
 * @return 0, or FDB_ERR_PLATFORM_ERROR when @p e cannot be encoded
 *         (nfs_fh_len above MDS_NFS_FH_MAX).
 *
 * Ownership: nothing is retained.  Thread safety: that of @p tr.
 */
static inline int fdb_gc_row_set(FDBTransaction *tr, const struct fdb_key_prefix *p,
                                 const struct mds_gc_entry *e)
{
    static const uint8_t empty = 0;
    uint8_t enc[FDB_GC_ENC_MAX];
    size_t len = 0;
    struct fdb_key k;

    if (!fdb_gc_encode(e, enc, sizeof(enc), &len)) {
        return FDB_ERR_PLATFORM_ERROR;
    }
    fdb_key_gc(&k, p, e->gc_seq);
    fdb_txn_set(tr, &k, enc, len);
    fdb_key_gc_by_owner(&k, p, e->owner_mds_id, e->gc_seq);
    fdb_txn_set(tr, &k, &empty, 0);
    return 0;
}

/**
 * Blind-clear the GC row @p gc_seq and its index key under
 * @p owner_mds_id (the owner stored in the row) in @p tr.
 *
 * Ownership: nothing is retained.  Thread safety: that of @p tr.
 */
static inline void fdb_gc_row_clear(FDBTransaction *tr, const struct fdb_key_prefix *p,
                                    uint64_t gc_seq, uint32_t owner_mds_id)
{
    struct fdb_key k;

    fdb_key_gc(&k, p, gc_seq);
    fdb_txn_clear(tr, &k);
    fdb_key_gc_by_owner(&k, p, owner_mds_id, gc_seq);
    fdb_txn_clear(tr, &k);
}

#endif /* CATALOGUE_FDB_INTERNAL_H */
