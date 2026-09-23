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

**Status:** `rr` and `fill` are implemented (Stage A).  `smart` parses but
is refused at startup with `PLACEMENT_MODE_UNSUPPORTED_BUILD` until the
connector client lands (Stage B) — it is `NOT_READY`.

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
| same host string and same `f_fsid`, no shared declared domain | `SHARED_FS_ALIAS_UNMAPPED` — both DS excluded (a proven alias must be declared) |
| same `f_fsid` behind different host strings, no shared domain | `ALIAS_SUSPECTED` — a rate-limited WARN and a counter; not excluded (cannot be proven from NFS) |

The domain's observation is the fresh one of the lowest DS id; a fresh
sibling that disagrees by more than 1 % of `total` pulls `available`
down to the smaller value.

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

## Observability

- Startup line: `placement_mode=<mode> generation=<sha256[:12]>
  kernel=<id> shrink=<allow|strict> max_age_ms=<n> min_free=<n>` (only
  with an explicit mode).
- `mds-admin config show [--mds-port <grpc_port>]`: `placement_mode`,
  `placement_mode_effective`, `placement_config_generation`,
  `placement_kernel_id`, the thresholds, `ds_capacity_domain.<id>` and one
  `placement_ds.<id> = domain=… state=… capacity_age_ms=… avail=… total=…
  weight=… reason=…` row per registered DS (`config show placement_ds.<id>`
  for one row).
- Metrics: `pnfs_mds_placement_mode{mode}`,
  `pnfs_mds_placement_eligible_ds`,
  `pnfs_mds_placement_rejections_total{reason}`,
  `pnfs_mds_placement_alias_suspected_total`, plus the upstream
  `pnfs_mds_placement_degraded_total` when a layout shrank.
- Refusals are logged once per reason per 10 s.

## Changing the mode

The mode is read at startup.  Edit `mds.conf` on every MDS, keep the
`placement_config_generation` identical, restart the daemons one at a
time and confirm the generation with `config show` — the
`lattice-placement` helper (Stage C) automates the diff, the backup and
the verification.  Switching never touches existing layouts.
