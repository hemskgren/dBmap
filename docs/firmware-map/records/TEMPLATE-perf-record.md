# Performance record template

Copy to `YYYYMMDD-<short-id>.md` in this directory. Fill only what was measured.
Use `n/a` or omit metrics that were not collected. Value classes:
[schema.md](../schema.md).

Do **not** promote estimates or host-only timings to device `measured` /
`verified_operating` without saying so.

---

## Identity

| Field | Value |
| --- | --- |
| `record_id` | |
| `date_utc` | |
| `product` | e.g. `dbmap-ear` |
| `hardware_profile` | e.g. `esp32-s3-n16r8` |
| `builder_target` | e.g. `ear-esp32-s3` |
| `idf_version` | e.g. `v5.4.4` |
| `firmware_version` | catalog / reported image version |
| `platform_version` | from `firmware/platform/VERSION` in that build |
| `git_sha` | full or short SHA of the tree built/flashed |
| `sdkconfig_identity` | builder effective-config digest, log path, or note |
| `node_id` | optional (e.g. `EAR-001`) |

## Scenario

| Field | Value |
| --- | --- |
| `scenario` | e.g. quiet_office / tone / dual_obs / soak |
| `duration_s` | |
| `workload_notes` | candidates open, Observations open, MQTT load, diag on/off |
| `instrumentation` | e.g. `CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG`, `sp_diag_log_resources`, L0/L1/L2 perf lines |

## Concurrent load observed

| Metric | Value | Method / source | Value class | Notes |
| --- | --- | --- | --- | --- |
| Max simultaneous CE candidates | | UART / test harness | | |
| Max open normal Observations | | | | |
| Critical Observations | | | | Use only if implemented |

## CPU

| Metric | Value | Method / source | Value class | Notes |
| --- | --- | --- | --- | --- |
| Configured MHz | | `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` / diag `configured_mhz` | | |
| Runtime MHz | | `esp_clk_cpu_freq` via `sp_diag_log_resources` / ear_obs cpu line | | |
| CPU load / utilization | | method if any; else `n/a` | | |

## Memory

| Metric | Value | Method / source | Value class | Notes |
| --- | --- | --- | --- | --- |
| Free heap (end or sample) | | `esp_get_free_heap_size` / diag | | |
| Min free heap | | `esp_get_minimum_free_heap_size` / diag | | |
| Free SPIRAM | | `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` / diag | | |
| Task stack HWM | | `sp_diag_log_resources` `stack_hwm_bytes` | | |

## Analysis timing

| Metric | Value | Method / source | Value class | Notes |
| --- | --- | --- | --- | --- |
| L0 us_last / us_max | | `ear_obs: L0 perf` / `dbmap_ear_mic_get_l0_perf` | | |
| L1 us_last / us_max | | `ear_obs: L1 perf` / `dbmap_ear_mic_get_l1_perf` | | |
| L2 us_last / us_max | | `ear_obs: L2 perf` / `dbmap_ear_mic_get_l2_perf` | | |
| Analysis rate | | windows/s or `analyze_n` over duration | | |
| Other function latency | | name the function | | |

Host (x86) timings, if any, go in a **separate** table labeled `host_only` and
must not be mixed into device rows.

## Acquisition stability

| Metric | Value | Method / source | Value class | Notes |
| --- | --- | --- | --- | --- |
| PCM overwrite count | | L0 perf `overwrite` | | |
| Incomplete windows | | `sp_mic` win / capture diag | | |
| I2S timeout / empty / err | | capture_diag / mic counters if present | | |
| Stereo status | | AE diag `stereo=` / pair flags | | Not DoA |

## Reporting

| Metric | Value | Method / source | Value class | Notes |
| --- | --- | --- | --- | --- |
| Observation interval configured | | Kconfig / Manager | | |
| Publish failures | | logs / counters | | |
| Sequence gaps | | Ear seq vs Hub rows | | |
| Skipped disconnected | | diag counters | | |

## Stability

| Metric | Value | Method / source | Value class | Notes |
| --- | --- | --- | --- | --- |
| Duration completed | | wall clock | | |
| Resets / panics | | | | |
| Overall result class | `measured` or `verified_operating` | criteria | | |

## Sources and artifacts

- UART log path / attachment id:
- Hub export / observation query notes:
- Related diagnostics doc (if any):

## Limitations

List missing fields, single-run caveats, and anything that prevents
`verified_operating`.
