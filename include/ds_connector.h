/*
 * SPDX-License-Identifier: MIT
 *
 * ds_connector.h -- the placement DS connector client (placement_mode =
 * smart), design section 7.
 *
 * Pure half: ds_connector_apply_batch() parses one batch of the
 * lattice-ds-connector contract (connector-batch.schema.json, contract
 * 1.0), validates the envelope, every instance's sequence line and every
 * assessment's binding against the DS registry, and builds an immutable
 * placement_assessment_view with TTLs on the MDS monotonic clock.  I/O
 * half (Task B3): the Unix-socket HTTP client and the poll thread that
 * publishes the view into the placement gate.
 */

#ifndef DS_CONNECTOR_H
#define DS_CONNECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "pnfs_mds.h"
#include "placement_modes.h"
#include "placement_gate.h"

#define DC_NAME_MAX      128
#define DC_INSTANCES_MAX 64
#define DC_BATCH_MAX     (4u * 1024u * 1024u)
#define DC_TTL_MAX_MS    3600000u

struct ds_connector_registry_ds {
    uint32_t ds_id;
    char     host[MDS_DS_HOST_MAX];
    char     export_path[MDS_DS_EXPORT_MAX];
    uint16_t tcp_port;
};

struct ds_connector_registry {
    uint32_t count;
    struct ds_connector_registry_ds ds[MDS_MAX_DS_NODES];
};

struct ds_connector_cfg {
    uint32_t contract_major;
    uint32_t max_ds;
    char     access_scope[PM_SCOPE_MAX];
    uint32_t expected_profile_count;                       /* 0 = not pinned */
    struct pm_profile_pin expected_profiles[PM_PROFILES_MAX];
    char     expected_config_digest[PM_DIGEST_MAX];    /* "" = not pinned */
};

/* First accepted binding tuple per ds_id (design section 7, review finding 2). */
struct ds_connector_pin {
    bool     pinned;
    char     instance[DC_NAME_MAX];
    uint32_t binding_generation;
    char     datastore_id[DC_NAME_MAX];
    char     target_id[DC_NAME_MAX];
    char     target_incarnation[DC_NAME_MAX];
    char     access_scope[PM_SCOPE_MAX];
};

struct ds_connector_instance_seq {
    bool     used;
    char     id[DC_NAME_MAX];
    char     epoch[DC_NAME_MAX];
    uint64_t sequence;
};

struct ds_connector_state {
    struct ds_connector_cfg cfg;
    char     runtime_epoch[DC_NAME_MAX];
    struct ds_connector_instance_seq inst[DC_INSTANCES_MAX];
    struct ds_connector_pin pins[MDS_MAX_DS_NODES];
    uint64_t last_generated_at_ms;   /* 0 = none yet */
};

/* enum ds_connector_drop and ds_connector_drop_name() live in placement_modes.h. */

struct ds_connector_report {
    enum ds_connector_drop drop;
    uint32_t accepted;
    uint32_t rejected_binding;
    uint32_t rejected_shape;
    uint32_t unknown_ds;
    uint32_t rebound;
    char     detail[160];
};

void ds_connector_state_init(struct ds_connector_state *st,
                             const struct ds_connector_cfg *cfg);

/*
 * Parse and validate one batch.  DC_OK: `out` holds one row per registry
 * DS (present=false where no record was accepted) and the state advanced
 * (sequence lines, pins, generated_at).  Any other drop leaves the state
 * untouched and `out` empty with batch_valid=false.
 */
enum ds_connector_drop ds_connector_apply_batch(struct ds_connector_state *st,
                                                const char *text, size_t len,
                                                const struct ds_connector_registry *reg,
                                                uint64_t now_mono_ms,
                                                struct placement_assessment_view *out,
                                                struct ds_connector_report *rep);

/* "YYYY-MM-DDTHH:MM:SS[.fff]Z" -> milliseconds since the epoch; 0 on error. */
uint64_t ds_connector_iso8601_ms(const char *s, size_t len);

/* Endpoint rule (endpoint ds_path design §4): server == host,
 * port == tcp_port when both set; without ds_path the endpoint path equals
 * the registry path; with ds_path, ds_path equals the registry path and the
 * endpoint path is ds_path or a component-wise ancestor of it, "/" never a
 * parent.  Trailing '/' is ignored.  ds_path NULL or "" = absent. */
bool ds_connector_endpoint_matches(const struct ds_connector_registry_ds *ds,
                                   const char *server, const char *export_path,
                                   const char *ds_path, uint32_t port);

/* -----------------------------------------------------------------------
 * I/O half
 * ----------------------------------------------------------------------- */

struct ds_cache;

/*
 * One HTTP/1.1 GET over the Unix socket.  MDS_OK with a malloc'd body on
 * 200; MDS_ERR_DELAY on 503 (connector not ready); MDS_ERR_INVAL on any
 * other status; MDS_ERR_IO on connect failure, timeout, oversize or an
 * unparsable response.  The deadline covers connect + the full read.
 */
enum mds_status ds_connector_http_get(const char *socket_path, const char *path,
                                      uint32_t deadline_ms, char **body, size_t *len,
                                      int *http_status);

/* Configure without a thread (tests); start = configure + thread. */
int  ds_connector_configure(const struct mds_config *cfg, struct ds_cache *cache);
int  ds_connector_start(const struct mds_config *cfg, struct ds_cache *cache);
void ds_connector_stop(void);
/* One synchronous poll: GET, validate, publish, update the facts. */
enum mds_status ds_connector_poll_once(void);
void ds_connector_facts(struct placement_connector_facts *out);

#endif /* DS_CONNECTOR_H */
