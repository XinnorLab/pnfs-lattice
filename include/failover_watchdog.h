/*
 * Copyright (c) 2026 PeakAIO
 * SPDX-License-Identifier: MIT
 *
 * failover_watchdog.h -- Partner-liveness watchdog.
 *
 * Starts a background thread on the standby MDS that polls the
 * catalogue's node registry for a stale partner heartbeat
 * (mds_cluster_node_scan_stale) and triggers failover_promote() when
 * the partner misses enough heartbeats.  Backend-neutral: it needs
 * only the node_scan_stale cluster slot and refuses to start without
 * it.  See src/cluster/failover_watchdog.c for the full design note.
 */

#ifndef FAILOVER_WATCHDOG_H
#define FAILOVER_WATCHDOG_H

#include <stdint.h>

#include "pnfs_mds.h"

struct failover_ctx;
struct mds_catalogue;
struct failover_watchdog;

struct failover_watchdog_cfg {
	struct failover_ctx  *fo;            /**< Existing failover context. */
	struct mds_catalogue *cat;           /**< Catalogue with node_scan_stale. */
	uint32_t              partner_id;    /**< MDS ID to watch. */
	uint32_t              poll_interval_ms;
	uint32_t              stale_timeout_ms;
	uint32_t              min_observe_ms;
};

/**
 * Start the partner-liveness watchdog.
 *
 * @param cfg Config (see struct).  poll_interval_ms,
 *            stale_timeout_ms, and min_observe_ms are optional
 *            (0 = compile-time defaults).
 * @param out Receives the watchdog handle (NULL on any failure).
 * @return MDS_OK on success; MDS_ERR_INVAL for a NULL cfg/out/fo/cat
 *         or partner_id 0; MDS_ERR_NOSUPPORT when the catalogue's
 *         backend has no node_scan_stale cluster slot
 *         (mds_cluster_stale_scan_supported() is false) -- nothing is
 *         started in that case; MDS_ERR_NOMEM when the thread cannot
 *         be created.
 */
enum mds_status failover_watchdog_start(
	const struct failover_watchdog_cfg *cfg,
	struct failover_watchdog **out);

/**
 * Stop the watchdog.  Signals the thread, joins, frees.  NULL-safe.
 */
void failover_watchdog_stop(struct failover_watchdog *wd);

#endif /* FAILOVER_WATCHDOG_H */
