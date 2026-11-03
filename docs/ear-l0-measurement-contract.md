# Observation protocol v1 — optional L0 measurements contract

**Status:** Hub, simulator, and Ear serializer share this extension.  
**Protocol version:** remains **`1`**. Do not bump to v2 for L0.

Physical Ear MQTT omits the extension unless `CONFIG_DBMAP_EAR_OBS_PUBLISH_L0=y`
(see Ear `sdkconfig.defaults` for the image default).

## Envelope

| Field | Type | Notes |
| --- | --- | --- |
| `protocol_version` | `1` | Required (unchanged) |
| `message_type` | `"observation"` | Required |
| `observation_id` | UUID string | Dedup key |
| `node_id` | string | |
| `sequence_number` | uint | |
| `event_time_utc` | ISO-8601 TZ | Start of Observation interval `[t, t+duration)` |
| `capture_timestamp_monotonic_us` | int | Interval end (mono) |
| `timing_quality` | enum | synchronized \| estimated \| unsynchronized |
| `clock_offset_ms` | null \| number | |
| `timestamp_uncertainty_ms` | number ≥ 0 | |
| `classification` | null \| object | Physical Ear: null |
| `bearing` | null \| object | Physical Ear: null |
| `signal_level_dbfs` | number ≤ 0 | **Detector closing-window RMS** (digital FS, not SPL) |
| `duration_ms` | number ≥ 0 | `(interval_end - interval_start) / 1000` |
| `emission_kind` | `"start"` \| `"periodic"` \| `"end"` | **Optional extension** — required together with `measurements` |
| `measurements` | object | **Optional extension** — L0 combined-scene aggregate |

Observation time span remains `[interval_start, interval_end)`.  
L0 window membership: `start < window_end ≤ end` (Checkpoint A).

### Compatibility

| Payload | Hub behavior |
| --- | --- |
| Legacy v1 (neither new field) | Accept as before |
| Extended v1 (both new fields, valid) | Validate and accept |
| `measurements` malformed / unknown nested keys | Reject |
| `emission_kind` invalid | Reject |
| Only one of the two new fields present | Reject |

Older senders are **not** required to include L0. When
`CONFIG_DBMAP_EAR_OBS_PUBLISH_L0` is enabled on Ear, every MQTT Observation
must include both fields; missing L0 / history overflow refuses publish.

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
| `window_count` | int | Usable associated windows |
| `clipped_window_count` | int | Among associated |
| `n_crest` | int | Usable **unclipped** (T3 denominator) |
| `rms.mean_dbfs` / `min_dbfs` / `max_dbfs` | number \| null | Null if `window_count=0` |
| `bands.mean_dbfs[4]` | number \| null each | Linear mean of \(E_k\) then \(10\log_{10}\); nulls if no windows |
| `bands.fraction[4]` | number \| null each | \(\sum E_k / \sum_{all} E\); all null if below energy floor |
| `hl.mean_db` | number \| null | \(10\log_{10}((\Sigma B2+\Sigma B3)/(\Sigma B0+\Sigma B1))\) from **aggregated linear** energies; null if invalid/insufficient energy |
| `t1.rms_stdev_db` / `rms_range_db` | number \| null | Null if \(N<2\) |
| `t2.rms_slope_db_per_s` | number \| null | Null if \(N<3\) for OLS |
| `t3.crest_max_db` / `high_crest_count` / `high_crest_rate` | number \| null | Null if `n_crest=0` |
| `t6.hl_slope_db_per_s` | number \| null | Null if &lt;3 valid per-window HL points |

Combined mic scene only — not per-source. Unknown keys inside `measurements`
(and nested objects) are rejected by Hub (`extra=forbid`).

## Ear configuration

| Config | Behavior |
| --- | --- |
| `PUBLISH_L0=n` (default) | MQTT legacy v1; L0 still computed for serial diag when MIC/OBS L0 on |
| `PUBLISH_L0=y` | Protocol still v1; every Observation includes `emission_kind` + `measurements`. Missing L0 / overflow / capacity fail → **refuse publish** |
| Capacity | `L0_MAX_WINDOWS` (default 64) must cover `INTERVAL_MS/WINDOW_MS + 2` or start fails when publishing L0 |

## Ring-buffer capacity (ESP32)

| Symbol | Default / formula |
| --- | --- |
| `DBMAP_EAR_OBS_L0_MAX_WINDOWS` | 64 (Kconfig range 8–128) |
| `DBMAP_EAR_OBS_INTERVAL_MS` | 5000 (range 1000–300000) |
| `DBMAP_EAR_MIC_WINDOW_MS` | 1000 |
| Windows needed | `INTERVAL_MS / WINDOW_MS + 2` (exclusive-start inclusive-end edges) |
| Supported interval @ default 64 | ≤ **62 s** (`(64-2)×1 s`) |
| Supported interval @ max 128 | ≤ **126 s** |

If configured interval needs more windows than `L0_MAX_WINDOWS`:

- With `PUBLISH_L0=y`: `dbmap_ear_observation_start` returns `ESP_ERR_INVALID_SIZE` (no silent truncation).
- With `PUBLISH_L0=n`: logs error; L0 interval path remains degraded / overflow-flagged.

If association exceeds hist capacity at runtime: `history_overflow=true`, temporal
features T1/T2/T3/T6 are cleared (never silently incomplete), and MQTT publish
is refused when `PUBLISH_L0` is on.

Measured on host/`esp32` ABI packing for cap=64:

| Block | Size |
| --- | --- |
| `sizeof(dbmap_l0_window_t)` | 104 B |
| Ring entry (window + usable) | 112 B |
| `s_l0_ring[64]` | 7168 B |
| Hist arrays (rms/hl/crest/t_end/flags) | 1408 B |
| **Total static ≈** | **8576 B (~8.4 KiB)** |

Prefer SPIRAM for PCM double-buffers; this ring stays in internal RAM for
determinism. Raise `L0_MAX_WINDOWS` only after confirming free internal RAM
(128 → ~17 KiB static).

## Cutover (physical Ear)

1. Deploy Hub that accepts the optional extension (this Checkpoint).  
2. Keep physical Ear on `PUBLISH_L0=n` until ready.  
3. Operator enables `CONFIG_DBMAP_EAR_OBS_PUBLISH_L0=y`, rebuilds, flashes/OTAs.  
4. Confirm Hub stores `emission_kind` + `measurements`; reconcile sequences (see protocol.md).
