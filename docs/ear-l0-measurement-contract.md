# Observation protocol v1 — L0 measurements (hierarchical)

**Status:** Hub, simulator, and Ear serializer share this contract.  
**Protocol version:** **`1`** hierarchical envelope only (`identity` / `context` / `acoustic` / `spatial`). Flat Observation payloads are rejected.

L0 lives at `acoustic.measurements` with `schema: "l0"`.

## Envelope (summary)

See [protocol.md](./protocol.md) for the full hierarchical Observation shape.

| Path | Notes |
| --- | --- |
| `identity.observation_id` | Stable episode UUID (shared across START/PERIODIC/END) |
| `identity.sequence_number` | Global per-Ear report sequence; Hub dedup with `node_id` |
| `context.emission_kind` | `start` \| `periodic` \| `end` \| `complete` |
| `context.termination_reason` | Required on `end` / `complete` |
| `context.interval_duration_ms` | This report’s analysis interval |
| `context.episode_duration_ms` | Episode start → this report end |
| `acoustic.signal_level_dbfs` | Latest valid RMS or JSON null |
| `acoustic.measurements` | L0 combined-scene aggregate (required) |

Observation interval association: `start < window_end ≤ end`.

## `measurements` (schema `l0`)

| Field | Type | Nullability / validity |
| --- | --- | --- |
| `schema` | `"l0"` | Always |
| `version` | int | `DBMAP_L0_MEASUREMENT_VERSION` (1) |
| `band_plan_version` | int | 1 |
| `crest_threshold_version` | int | 1 |
| `crest_threshold_db` | number | 12.0 |
| `mic_index` | int | 0 today |
| `sample_rate_hz` | int | e.g. 16000 |
| `analysis_window_ms` | int | e.g. 1000 |
| `window_count` | int | Usable associated windows (`0` allowed for empty finals) |
| `clipped_window_count` | int | Among associated |
| `n_crest` | int | Usable **unclipped** (T3 denominator) |
| `rms.mean_dbfs` / `min_dbfs` / `max_dbfs` | number \| null | Null if `window_count=0` |
| `bands.mean_dbfs[4]` | number \| null each | Linear mean of \(E_k\) then \(10\log_{10}\); nulls if no windows |
| `bands.fraction[4]` | number \| null each | \(\sum E_k / \sum_{all} E\); all null if below energy floor |
| `hl.mean_db` | number \| null | From aggregated linear energies |
| `t1` / `t2` / `t3` / `t6` | nested | Null fields when insufficient windows / overflow |

Combined mic scene only — not per-source. Unknown keys inside `measurements` are rejected (`extra=forbid`).

## Cutover

1. Stop flat Observation publishers.  
2. Clear development `observations` if the table still uses `observation_id` as PK (Hub migrates by dropping incompatible rows).  
3. Deploy Hub (hierarchical + `(node_id, sequence_number)` dedup).  
4. Deploy Ear / simulator hierarchical publishers.

## Ring capacity (ESP32)

Unchanged: `DBMAP_EAR_OBS_L0_MAX_WINDOWS` (default 64), shared combined-scene ring, ~8 KiB class static with hist arrays.
