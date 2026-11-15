# R0 device record — EAR-002 (1 microphone) — TEMPLATE

Copy to `YYYYMMDD-r0-ear-002.md` and fill **only measured** cells.  
Procedure: [`docs/diagnostics/r0-esp32-s3-measurement-baseline.md`](../../diagnostics/r0-esp32-s3-measurement-baseline.md) §10.

## Identity

| Field | Value |
| --- | --- |
| `record_id` | |
| `date_utc` | |
| `product` | dbmap-ear |
| `hardware_profile` | esp32-s3-n16r8 |
| `builder_target` | ear-esp32-s3 |
| `idf_version` | v5.4.4 |
| `firmware_version` | |
| `platform_version` | |
| `git_sha` | |
| `sdkconfig_identity` | serial_diag + ae_diag image; note defaults file |
| `node_id` | EAR-002 |
| `microphone_count` (NVS/live) | 1 |
| Value class for filled metrics | `measured` |

## Scenario

| Field | Value |
| --- | --- |
| Idle duration_s | 900 (15 min) after 60 s warm-up |
| Loaded duration_s | 600 (10 min) |
| instrumentation | SERIAL_DIAG, AE_DIAG, `sp_diag_log_resources` |

## CPU

| Metric | Idle | Loaded | Method |
| --- | --- | --- | --- |
| configured_mhz | | | `sp: res configured_mhz` |
| runtime cpu_mhz | | | `sp: res cpu_mhz` |

## Memory (end of idle / end of loaded)

| Metric | Idle | Loaded | Method |
| --- | --- | --- | --- |
| pcm_cap / pcm_bytes / ch | | | boot `analysis PCM ready` |
| free_heap / min_free_heap | | | `sp: res` |
| free_internal / min / largest | | | `sp: res` |
| free_spiram / min / largest | | | `sp: res` |
| free_heap_init / free_spiram_init | | | L0 perf or boot line |

## Stack (bytes remaining HWM)

| Task | Configured | Idle HWM | Loaded HWM |
| --- | ---: | --- | --- |
| sp_mic | 6144 | | |
| ear_obs | 6144 | | |

## Analysis timing (wall µs)

| Metric | n | last | avg | max | Notes |
| --- | --- | --- | --- | --- | --- |
| L0/analyze pass | | | | | If L2 on, same pass as L2 combined — do not sum |
| L1 (if L2 off) | | | | | |
| L2 combined | | | | | |
| AE analyze | | | | | |
| AE post_dsp | | | | | includes CE listeners |
| promo_mgr | | | | | |
| publish_json_mqtt | | | | | |

## Backpressure (end of soak)

| Counter | Idle | Loaded |
| --- | --- | --- |
| PCM overwrite | | |
| obs_mgr evq_drop | | |
| ce create_dropped_full | | |
| critical drop_busy | | |

## Limitations

Device results pending operator fill. No `verified_operating` claim from this template alone.
