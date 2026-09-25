# Placement modes (`rr` / `fill` / `smart`)

XinnorLab addition to pnfs-lattice (branch `xinnor/placement-modes`, base
upstream `6b4dcde`).  Design and requirements: XinnorLab/pNFS
`docs/superpowers/specs/2026-09-23-placement-modes-design.md`.

One operator-facing key, `placement_mode`, selects how a **new** backing
object picks its data server.  Nothing else about a file changes: stripe
and mirror geometry, stripe unit, inline policy and device info are the
same in every mode.  Without the key the daemon behaves exactly as
upstream (`placement_policy`, `placement_policy_enabled`,
`placement_capacity_weighting`, `ds_weight.<id>`).

| Mode | Candidate set | Weight | Connector |
|---|---|---|---|
| `rr` | registered DS that are `DS_ONLINE` and pass the native NFS/transport/io-limit filters | none — cyclic order over the candidate list, one position per DS | not consulted |
| `fill` | the `rr` set **and** a fresh capacity observation for the DS's capacity domain with `available > placement_min_free_bytes` | `domain_weight / N`, `domain_weight = max(1, floor(100 × available / total))` | not consulted |
| `smart` | the `fill` set **and** a fresh `VALID` / `allowed` / `multiplier_ppm > 0` assessment from the DS connector | `domain_weight / N × multiplier_ppm / 1 000 000` | required |

`fill` and `smart` are **weighted random** selection, not a strict
rotation.  `capacity` stays a legacy `placement_policy` value (strict
maximum) and is not one of the modes.

**Status:** `rr`, `fill` and `smart` are implemented (Stages A and B).
`smart` needs a binary built with `ENABLE_DS_CONNECTOR=ON` (refused with
`PLACEMENT_MODE_UNSUPPORTED_BUILD` otherwise) and a running per-MDS
`lattice-ds-connector`; the `lattice-placement` helper and the
acceptance rows are Stage C.

## Keys

| Key | Default | Range / values | Applies to |
|---|---|---|---|
| `placement_mode` | absent = legacy | `rr` \| `fill` \| `smart` | all |
| `ds_capacity_poll_ms` | 60000 (existing) | fill/smart: `> 0` | fill, smart |
| `placement_capacity_max_age_ms` | 120000 | `> ds_capacity_poll_ms`, ≤ 86400000 | fill, smart |
| `placement_min_free_bytes` | 0 | uint64; a domain is a candidate only when `available > value` | fill, smart |
| `ds_capacity_domain.<ds_id>` | unset = the DS is its own domain | non-empty string ≤ 127 bytes | fill (required for shared-filesystem aliases), smart (must agree with the connector) |
| `placement_stripe_shrink` | `allow` | `allow` \| `strict` | all |
| `placement_allow_manual_base_weights` | false | bool | smart |
| `placement_domain_weight.<domain>` | unset | 1..10000, only with the flag above | smart |

## Validation errors (startup refuses)

| Code | Cause |
|---|---|
| `PLACEMENT_MODE_CONFLICT` | `placement_policy`, `placement_policy_enabled` or `placement_capacity_weighting` present next to `placement_mode`; a `workload_profile` that sets a placement policy (`hpc`, `ai_training`, `genomics`, `media`); `ds_weight.<id>` in `fill` |
| `PLACEMENT_MODE_UNSUPPORTED_BUILD` | `smart` on a binary without `ENABLE_DS_CONNECTOR` |
| `RANGE` | `ds_capacity_poll_ms = 0` in fill/smart; `placement_capacity_max_age_ms` not above the poll interval or out of range; bad `placement_mode` / `placement_stripe_shrink` value; bad `ds_capacity_domain.<id>` key or empty domain |
| `DOMAIN_WEIGHT_FORBIDDEN` | `placement_domain_weight.*` without `smart` + `placement_allow_manual_base_weights = true` |
| `MIRROR_COUNT_UNSUPPORTED` | `smart` with `default_mirror_count > 1` |

The gate also refuses to start `fill`/`smart` on a binary that carries the
community `wrr` stub (kernel id 0) or without the DS cache.

## Capacity domains and aliases

Two exports of one filesystem are two DS but one capacity domain.
Declare it: `ds_capacity_domain.0 = xinas-01/fs-uuid-01` and the same
for `.1`.  The domain's weight is shared `1/N` across its DS (N counts
every registered DS of the domain, offline included; a denied alias does
not hand its share to the others).  The `statvfs` `f_fsid` of each
back-mount checks the declaration:

| Observation | Result |
|---|---|
| one declared domain, same host string, different `f_fsid` | `DOMAIN_MAP_CONTRADICTION` — both DS excluded |
| same host string and same `f_fsid`, no shared declared domain | `SHARED_FS_ALIAS_UNMAPPED` for the **undeclared** side (a proven alias must be declared); a DS whose domain the operator or, in `smart`, the connector declared keeps its place, so a sibling that is not bound yet cannot take a healthy DS out. Two *different* declared domains on one filesystem are a `DOMAIN_MAP_CONTRADICTION` for both |
| same `f_fsid` behind different host strings, no shared domain | `ALIAS_SUSPECTED` — a rate-limited WARN and a counter; not excluded (cannot be proven from NFS) |

The domain's observation is the fresh one of the lowest DS id; a fresh
sibling that disagrees by more than 1 % of `total` pulls `available`
down to the smaller value.

Every registry-level fact (N, the alias grades, the canonical
observation) is taken from the published registry view of **all**
registered DS, not from the subset a caller happened to pass — an
offline or io-limit-filtered sibling still counts toward N and still
proves an alias.  The registry view is republished after every capacity
sweep and after every DS admin change (add/remove/set-state), and the
create boundary additionally reads the DS admin state live.  A capacity
probe of a mount path that is not a mount point (the empty directory an
unmounted DS leaves behind) counts as a failed probe, never as an
observation of the DS.

## Reasons

A DS that is not a candidate carries one reason; the first failing check
in this order: `DS_OFFLINE` → `DOMAIN_MAP_CONTRADICTION` /
`SHARED_FS_ALIAS_UNMAPPED` → `CAPACITY_UNKNOWN` (never observed) /
`CAPACITY_STALE` (older than `placement_capacity_max_age_ms`) →
`CAPACITY_FULL` (`available ≤ placement_min_free_bytes`) →
`WEIGHT_OVERFLOW` (defensive).  A refused placement is `NO_ELIGIBLE_DS`
(nothing eligible) or `INSUFFICIENT_ELIGIBLE_DS` (strict shrink, or fewer
eligible DS than mirrors); `smart` without its assessment source is
`MODE_NOT_READY`.  Clients see the existing `NFS4ERR_NOSPC`.

## Where the gate runs

Every selection of a DS for a new object goes through
`placement_select_gated()` — LAYOUTGET (both paths), inline promotion,
prealloc pop/peek/batch (stub and module).  Every *creation* of a DS file
goes through `mds_proxy_create_ds_file*()`, which needs an admission token
from `placement_gate_admit_create()` minted moments before; the
`mds_proxy_ensure_*` helpers look the object up first and only create
when it is absent **and** admitted.  An existing object is never blocked
(lookup is not gated); a missing object on a DS the gate rejects is not
re-created — LAYOUTGET keeps answering `NFS4ERR_DELAY` for that stripe
and a proxy write on it fails instead of creating elsewhere.

## `smart`: the connector client

Each MDS polls **its own** `lattice-ds-connector` over the Unix socket
(`ds_connector_socket`, every `ds_connector_poll_ms`, deadline
`ds_connector_request_deadline_ms`).  Placement never touches the socket:
the poll thread validates the batch and publishes an immutable assessment
view the gate reads.  Keys: see `config-keys.md` ("Connector client
keys"); the connector is a prerequisite of the mode, so
`ds_connector_enabled` is derived and an explicit contradiction is a
config error.

A batch is accepted whole or dropped whole: `contract_version` must be
`<major>.<…>` with the major equal to
`ds_connector_expected_contract_major` (`1x`, `01.0` and a bare `1` are
not versions); per connector instance the `(epoch, sequence)` line must
not go backwards — an **equal** sequence is the connector re-serving its
current snapshot until the next collection and is accepted (the rows are
re-timed from the receive instant), a **lower** one is a replay that
drops the batch; a changed `runtime_epoch` (connector restart) resets
every line and every binding pin; a batch that would need more sequence
lines than the 64 the MDS keeps is refused rather than run unprotected;
`generated_at` must not go backwards (a backwards wall-clock step on the
connector host therefore drops batches — `OLD_GENERATED_AT`, named in
`placement_connector_last_detail` — until the clock passes the last
accepted instant or the connector restarts with a new `runtime_epoch`;
`smart` fails closed meanwhile); `config_digest` must equal
`ds_connector_expected_config_digest` when that pin is set.  The body is
at most 4 MiB, nested at most 64 levels (checked in one linear pass before
parsing — jsmn is quadratic on depth), and strings are decoded with the
standard JSON escapes including `\uXXXX` surrogate pairs, so a
`json.dumps` connector may name a share in any script.  Inside
an accepted batch every assessment is bound before it is trusted: `scope`
= `NEW_ALLOCATION`, `access_scope_id` = `ds_connector_access_scope`,
`endpoint.server` = the registry host, `endpoint.port` = the registry
port, and the registered export path equals the endpoint's or lies under
it (`192.168.64.51:/mnt/data/pnfs-ds` inside the share `/mnt/data`);
when profile pins are set via `ds_connector_expected_profiles`, each record's profile id must be in the pin map with a matching digest; records with unpinned ids or mismatched digests are rejected. When pins are unset, any profile is accepted. The batch is dropped if one profile id carries two digests or if there are more than 8 distinct profile ids (a trailing `/` on the
endpoint path names the same share).  The first accepted tuple
`(instance, binding_generation, datastore_id, target_id,
target_incarnation, access scope)` is pinned per DS; a later record must
repeat it or carry a higher `binding_generation` (rebind: the DS is
UNKNOWN for that batch); anything else is `BINDING_MISMATCH` and that DS
has no record.  A `quality = UNKNOWN` record may carry
`target_incarnation: null` — the connector could not read its source and
knows the configured binding but not the share's incarnation; it is
compared on the rest of the tuple, accepted as UNKNOWN
(`ASSESSMENT_UNKNOWN` with the connector's reason codes, e.g.
`SOURCE_UNAVAILABLE`), and it neither creates nor changes a pin (a higher
generation clears the pin; the next VALID record pins).  A VALID record
with a null incarnation is a shape error.  The profile digest is
deliberately **not** part of the per-DS binding tuple: it is checked on every batch for id consistency and per-id binding,
but a connector profile reload must not strand every DS in
`BINDING_MISMATCH` until a process restart.  Records for unknown DS ids
are ignored; a DS id that appears more than once in a batch has no
trusted record and pins nothing.

Candidate rule in `smart` (after the capacity gate): a fresh record
(`received + remaining_ttl_ms` on the MDS clock) with `quality = VALID`,
`allowed = true` and `multiplier_ppm > 0`; otherwise `NO_BINDING`,
`ASSESSMENT_UNKNOWN`, `ASSESSMENT_STALE`, `CONNECTOR_DENIED` or
`ZERO_MULTIPLIER`.  Weight = `domain_weight × multiplier_ppm / 10⁶ / N`,
where the domain is the connector's `capacity_domain_id` (an operator
`ds_capacity_domain.<id>` that disagrees is `DOMAIN_MAP_MISMATCH`) and
`domain_weight` is the fill level, or a manual
`placement_domain_weight.<domain>` when
`placement_allow_manual_base_weights = true`.  Without any assessment
view every DS is `MODE_NOT_READY`; a lost connector never falls back to
`rr`/`fill` — rows expire on their TTL and the MDS refuses new
placements (`NFS4ERR_NOSPC`).

Readiness is four facts, reported by `config show` as
`placement_readiness = mode_active=… connector_config_valid=…
connector_reachable=… last_batch_valid=… coverage=full|partial|none
registered_ds=… covered_ds=… eligible_ds=…` (reachable = an accepted
batch within three intervals — one rule for every failure kind, whether
the socket is absent (`CONNECT`), the request timed out or the response
was malformed (`TIMEOUT`), the status was unexpected (`HTTP`), the
connector answered 503 (`UNAVAILABLE`) or the batch was dropped (`DROP`);
the `pnfs_mds_connector_reachable` gauge follows the same rule; coverage
counts DS with a fresh VALID record).  Partial coverage is a degraded, correct state: the MDS keeps
placing on the covered DS.  Run `lattice-ds-connector preflight
--expect-ds 0,1` on the MDS before switching to see the same facts from
the connector's side.

## Observability

- Startup line: `placement_mode=<mode> generation=<sha256[:12]>
  kernel=<id> shrink=<allow|strict> max_age_ms=<n> min_free=<n>` (only
  with an explicit mode).
- `mds-admin config show --mds-host <cluster_bind_addr> --mds-port <grpc_port>`
  (the transport listens on the bind address, not loopback):
  `placement_mode`, `placement_mode_effective`,
  `placement_config_generation`, `placement_kernel_id`,
  `placement_build = wrr=<0|1> connector=<0|1> prealloc=<0|1>` (the build
  facts `verify` compares across MDS), the thresholds,
  `ds_capacity_domain.<id>`, in `smart` `placement_readiness`,
  `placement_connector_config_digest`, `placement_connector_profiles`,
  `placement_connector_last_detail`, and one `placement_ds.<id> = domain=…
  state=… capacity_age_ms=… avail=… total=… [assessment_age_ms=… quality=…
  allowed=… ppm=… ttl_ms=…] weight=… reason=…` row per registered DS
  (`config show placement_ds.<id>` for one row).
- Metrics: `pnfs_mds_placement_mode{mode}`,
  `pnfs_mds_placement_eligible_ds`,
  `pnfs_mds_placement_rejections_total{reason}`,
  `pnfs_mds_placement_admit_seconds` (histogram, 100 µs … +Inf with
  `_sum`/`_count`: time in the gate per selection or create admission,
  observed in every mode — legacy included — so a legacy-vs-smart
  comparison measures the gate alone),
  `pnfs_mds_placement_alias_suspected_total`, plus the upstream
  `pnfs_mds_placement_degraded_total` when a layout shrank.
- Connector: `pnfs_mds_connector_batches_accepted_total`,
  `pnfs_mds_connector_batches_dropped_total{reason}`,
  `pnfs_mds_connector_poll_errors_total{code}`,
  `pnfs_mds_connector_covered_ds`, `pnfs_mds_connector_reachable`,
  `pnfs_mds_connector_last_success_mono_ms`.
- `pnfs_mds_placement_rejections_total{reason}` counts every DS the gate
  rejected at every placement decision (a DS that stays full for an hour
  keeps counting), `pnfs_mds_placement_alias_suspected_total` counts
  warned episodes (one per 60 s).  Refusals are logged once per reason
  per 10 s; the two alias grades that need an operator get an ERROR line
  once per 60 s.

## Changing the mode

The mode is read at startup.  Edit `mds.conf` on every MDS, keep the
`placement_config_generation` identical, restart the daemons one at a
time and confirm the generation with `config show` — the
`lattice-placement` helper (Stage C) automates the diff, the backup and
the verification.  Switching never touches existing layouts.
