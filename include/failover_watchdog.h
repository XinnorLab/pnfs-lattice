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

#include <stdbool.h>
#include <stdint.h>

#include "pnfs_mds.h"

struct failover_ctx;
struct mds_catalogue;
struct failover_watchdog;

/* -----------------------------------------------------------------------
 * Heartbeat clock domain
 *
 * Registry heartbeat timestamps are the writer's CLOCK_REALTIME in
 * nanoseconds (mds_cluster.h) and the watchdog compares them against
 * its own CLOCK_REALTIME, so the stale threshold must exceed the
 * deployment's NTP skew bound.  Before that contract the RonDB writers
 * stamped CLOCK_MONOTONIC -- nanoseconds since the writer's host
 * booted -- and such a value cannot reach FAILOVER_HB_REALTIME_FLOOR_NS
 * (2020-01-01T00:00:00Z) unless the host has been up for fifty years.
 * A row below the floor was therefore written by a not-yet-upgraded
 * primary during a rolling upgrade.  Consumers treat it as
 * INDETERMINATE: the watchdog skips the tick for that partner and
 * never declares it stale, because promoting against a live primary
 * that merely runs the old clock domain would be split-brain.  The
 * mds_list consumers (cluster_membership_populate) do not interpret
 * the timestamp at all.
 * ----------------------------------------------------------------------- */

/** 2020-01-01T00:00:00Z as CLOCK_REALTIME nanoseconds. */
#define FAILOVER_HB_REALTIME_FLOOR_NS 1577836800000000000ULL

/**
 * True when @p last_heartbeat_ns can be a CLOCK_REALTIME stamp, i.e. it
 * is at or above FAILOVER_HB_REALTIME_FLOOR_NS.  False means the row
 * is indeterminate (see above), never that it is stale.
 */
bool failover_heartbeat_plausible(uint64_t last_heartbeat_ns);

/* -----------------------------------------------------------------------
 * Stale threshold and the startup bound derived from it
 *
 * The default stale threshold is three heartbeat intervals (main.c
 * heartbeats every 5 s).  A node that has registered (fresh row) must
 * start its heartbeat thread within CLUSTER_STARTUP_DEADLINE_MS or exit
 * (main.c): otherwise its row goes stale while it is still
 * initialising, the standby promotes and takes its subtrees, and the
 * node then begins serving them too.  Two heartbeat intervals leave
 * one interval of margin below the threshold for clock skew and the
 * watchdog's 2 s poll cadence.
 * ----------------------------------------------------------------------- */

/** Default stale threshold (failover_watchdog_cfg.stale_timeout_ms == 0). */
#define FAILOVER_WATCHDOG_STALE_TIMEOUT_MS_DEFAULT 15000u

/** Budget from a successful node_register to the heartbeat thread start. */
#define CLUSTER_STARTUP_DEADLINE_MS 10000u

_Static_assert(CLUSTER_STARTUP_DEADLINE_MS <
	       FAILOVER_WATCHDOG_STALE_TIMEOUT_MS_DEFAULT,
	       "startup deadline must leave margin below the stale threshold");

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
 * A partner row whose heartbeat is below FAILOVER_HB_REALTIME_FLOOR_NS
 * is indeterminate: the tick is skipped (logged once per transition)
 * and the partner is never declared stale on its account.
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
