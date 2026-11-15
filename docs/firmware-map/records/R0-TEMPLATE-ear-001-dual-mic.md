# R0 device record — EAR-001 (2 microphones) — TEMPLATE

Copy to `YYYYMMDD-r0-ear-001.md` and fill **only measured** cells.  
Procedure: [`docs/diagnostics/r0-esp32-s3-measurement-baseline.md`](../../diagnostics/r0-esp32-s3-measurement-baseline.md) §10.

Use logical **mic0** / **mic1** only. Do not label L/R unless Test A is complete.

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
| `sdkconfig_identity` | serial_diag + ae_diag image |
| `node_id` | EAR-001 |
| `microphone_count` (NVS/live) | 2 |
| Value class for filled metrics | `measured` |

## Scenario

| Field | Value |
| --- | --- |
| Idle duration_s | 900 after 60 s warm-up |
| Loaded duration_s | 600 |
| workload_notes | Stimulus audible to both mics on loaded soak |
| instrumentation | SERIAL_DIAG, AE_DIAG |

## CPU

| Metric | Idle | Loaded | Method |
| --- | --- | --- | --- |
| configured_mhz | | | `sp: res` |
| runtime cpu_mhz | | | `sp: res` |

## Memory

| Metric | Idle | Loaded | Method |
| --- | --- | --- | --- |
| pcm_cap / ch / bytes | | | expect ch=2, bytes=**258816** if formula holds |
| free_heap / min_free_heap | | | combined |
| free_internal / min / largest | | | |
| free_spiram / min / largest | | | |

## Stack HWM (bytes remaining)

| Task | Configured | Idle | Loaded |
| --- | ---: | --- | --- |
| sp_mic | 6144 | | |
| ear_obs | 6144 | | |

## Analysis timing (wall µs)

| Metric | n | last | avg | max | Notes |
| --- | --- | --- | --- | --- | --- |
| Full multi-mic analyze | | | | | `active_mics=2`; L0 us == L2 combined when fused |
| AE analyze | | | | | |
| AE post_dsp | | | | | |
| promo_mgr | | | | | |
| publish_json_mqtt | | | | | |

## Sync / identity notes

| Item | Observed |
| --- | --- |
| Shared window_seq on pair | |
| AE stereo= / pair flags | |
| Physical L/R Test A | not claimed |

## Backpressure

| Counter | Idle | Loaded |
| --- | --- | --- |
| PCM overwrite | | |
| evq_drop | | |
| create_dropped_full | | |
| drop_busy | | |

## Limitations

Pending operator fill. Dual-mic analyze cost is expected higher than EAR-002; compare only with matching identity fields.
