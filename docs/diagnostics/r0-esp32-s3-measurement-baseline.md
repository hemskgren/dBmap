# R0 — ESP32-S3 measurement baseline

**Branch:** `firmware/r0-esp32-s3-measurement-baseline`  
**Baseline commit:** `e271c60` (PR #54) + R0 diagnostic instrumentation only  
**Status:** Source verification and operator procedure ready. **Device-measured Map rows are empty until operator soak.**

This document corrects audit resource figures against source, defines measurement
methods/units, maps backpressure counters, and gives EAR-001 / EAR-002 soak steps.
It does **not** claim verified operating limits.

Value classes follow [`docs/firmware-map/schema.md`](../firmware-map/schema.md).

---

## 1. Corrected PCM allocation (source-verified)

### Why not “16-bit PCM @ 16 kHz × 1 s”?

Analysis PCM is **`int32_t` samples** (4 bytes), not 16-bit. Capacity includes
**+1% slack + 16 samples**, and storage is a **SPIRAM double-buffer per retained
channel**.

### Formula (verified)

| Step | Expression | @ defaults |
| --- | --- | ---: |
| Sample rate | `CONFIG_SP_MIC_SAMPLE_RATE_HZ` | 16000 Hz |
| Window | `CONFIG_SP_MIC_WINDOW_MS` | 1000 ms |
| Expected samples/ch | `sp_audio_expected_samples(rate, window_ms)` = `rate * window_ms / 1000` | **16000** |
| Cap samples/ch | `expected + expected/100 + 16` | **16176** |
| Bytes per buffer | `cap * sizeof(int32_t)` | **64704** |
| Channels retained | `s_live_mic_count` (`s_pcm_retained_channels`) | 1 or 2 |
| Double buffers | 2 per channel | — |
| **Total SPIRAM** | `cap * 4 * 2 * channels` = `sp_mic_pcm_bytes_total()` | see below |

**Sources:** `firmware/platform/mic/ear_mic.c` (`s_pcm_cap` alloc loop),
`sp_mic_pcm_bytes_total()`, `sp_audio_expected_samples()` in `audio_metrics.c`,
host test `test_stereo_unpack.c` / `test_audio_metrics.c`.

### Totals

| Live mics | `pcm_bytes` | Class |
| --- | ---: | --- |
| 1 | `16176 × 4 × 2 × 1 = ` **129408** | source-verified; also **measured** on EAR-001 2026-11-02 (`pcm_bytes=129408`) |
| 2 | `16176 × 4 × 2 × 2 = ` **258816** | source-verified formula (2× mono); confirm on EAR-001 boot log `bytes=` |

**129408 is the total double-buffer allocation for one channel**, not a single
1-second 16-bit buffer. Duration represented by the **nominal** window is still
1000 ms; the extra 176 samples are slack for slight over-fill vs nominal length.

Alloc capability: `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`.

I2S DMA (`dma_desc_num` / `dma_frame_num`) is **separate** from this application
window PCM (see §5).

---

## 2. Heap / PSRAM figures (source-verified interpretation)

### Historical EAR-001 line (2026-11-02)

```text
ear_mic: L0 ready pcm_cap=16176 bytes=129408 free_heap=8458520 free_spiram=8251648
```

| Field | API | Meaning |
| --- | --- | --- |
| `free_heap` | `esp_get_free_heap_size()` | **Combined** free heap. With `CONFIG_SPIRAM_USE_MALLOC=y`, this includes SPIRAM-eligible free memory — **not** “internal only”. |
| `free_spiram` | `heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` | Free SPIRAM only |
| Implied internal (approx) | `free_heap − free_spiram` | ~206872 B on that log — **estimate**, not logged then |

**8458520 ≈ 8.06 MiB** and **8251648 ≈ 7.87 MiB** are consistent with an **8 MB
PSRAM** board after PCM alloc: most free heap *is* free SPIRAM; they are not two
independent 8 MB pools.

### R0 logging (instrumentation)

`sp_diag_log_resources` now prints (tag `sp`):

| Field | API |
| --- | --- |
| `free_heap` / `min_free_heap` | `esp_get_free_heap_size` / `esp_get_minimum_free_heap_size` (combined) |
| `free_internal` / `min_free_internal` / `largest_internal` | `MALLOC_CAP_INTERNAL` free / min / largest block |
| `free_spiram` / `min_free_spiram` / `largest_spiram` | `MALLOC_CAP_SPIRAM` free / min / largest block |
| `cpu_mhz` / `configured_mhz` | `esp_clk_cpu_freq()` / `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` |
| `stack_hwm_bytes` | current task, then registered `sp_mic` / `ear_obs` |

**Never** label `free_heap` as free SPIRAM.

PCM init snapshot remains `free_heap_after_init` / `free_spiram_after_init` via
`sp_mic_pcm_*` (same APIs as above at alloc time).

---

## 3. CPU frequency

| Item | Finding | Class |
| --- | --- | --- |
| Product `sdkconfig.defaults` (current) | **240 MHz** / FreeRTOS **100 Hz** | configured product default |
| R0 reference overlay | 160 MHz / 100 Hz via `sdkconfig.defaults.r0_ref` | configured for A/C soaks |
| Local gitignored `sdkconfig` | May lag defaults until `rm sdkconfig` + reconfigure | operator pitfall |
| Runtime record | `sp_diag_log_resources`: `cpu_mhz=` / `configured_mhz=` / `tick_hz=` | measured / compile-time |

**Operator must record both `cpu_mhz` and `configured_mhz` from the `sp: res` line
for every soak.** If they differ, note power-management or boot path. For R0 A/C
cells, build with `r0_ref` so `configured_mhz=160`.

---

## 4. Stack configuration and units

| Task | Configured stack arg | Priority | Create site |
| --- | ---: | ---: | --- |
| `sp_mic` | **6144** | 5 | `ear_mic.c` `EAR_MIC_TASK_STACK` |
| `ear_obs` | **6144** | 4 | `ear_observation.c` |

On ESP-IDF 5.4.4 Xtensa, `xTaskCreate` stack size and
`uxTaskGetStackHighWaterMark` are in **bytes** (comment in `ear_observation.c`;
R0 registers both tasks with `sp_diag_register_task`).

**Margin** = `stack_hwm_bytes` remaining (high-water mark = unused stack).  
Configured size is the allocation; HWM is remaining free stack at deepest use.

---

## 5. I2S DMA vs application PCM

EAR uses `I2S_CHANNEL_DEFAULT_CONFIG` without overriding DMA fields
(`ear_mic.c` `init_one_i2s_rx`). Effective defaults from installed IDF
`i2s_common.h`:

- `dma_desc_num = 6`
- `dma_frame_num = 240`

Runtime stores and reports these in health/capture diag (`dma_desc_num`,
`dma_frame_num`, derived `dma_buffer_bytes`). This is the **hardware DMA ring**,
not the SPIRAM analysis window (`pcm_cap` / `pcm_bytes`).

---

## 6. Timing instrumentation definitions

All stage times use **`esp_timer_get_time()` wall-clock microseconds**, not
CPU-cycle counters and not busy-time excluding preemption.

| Metric | What it measures | Where |
| --- | --- | --- |
| Acoustic / AE `analyze_us` | Wall time of `sp_audio_analyze_channels` for **all active mics** (serial L1 workspace) | `ear_acoustic_runtime.c`; also AE `analyze_*` |
| L0 `stage_*` | Last L0 channel’s internal stages only | L0 library stages |
| L1 `analyze_us_*` | Updated only when L2 **off**; when L2 on, fused time is L2 `combined_*` | |
| L2 `combined_us_*` | Same wall pass as multi-mic fused analyze when L2 on | **Same pass as L0 analyze_us when L2 enabled** — do not sum L0+L2 |
| AE `post_dsp_us_*` | Align + signature build + `ae_publish_signature` (**includes sync CE listeners**) | `ae_device.c` |
| `promo_mgr` | `ear_promotion_policy_poll` + `ear_obs_manager_poll` + `mgr_feed_evidence` | `ear_observation.c` |
| `publish_json_mqtt` | JSON build + `sp_mqtt_publish_json` | `dbmap_ear_observation_publish_ex` |

Printed aggregates: **n, last, avg (=sum/n), max**. No percentile histogram in R0
(would add RAM/CPU). Enable with:

- `CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG` — L0/L1/L2 perf, R0 stage lines, `sp: res`
- `CONFIG_DBMAP_EAR_AE_DIAG` — AE diag + AE timing line

Build tip (existing):  
`idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.serial_diag" …`

---

## 7. Microphone identity and sync guarantees

Use **`mic0` / `mic1`** logical indexes. Do **not** claim left/right until Test A
(physical L/R strap) is documented complete.

| Guarantee | Status |
| --- | --- |
| Shared `window_seq` + `measured_at_us` / `window_end_mono_us` for channels in one measure window | **Verified** |
| Coherent hand-off via `sp_mic_take_window_job` | **Verified** |
| Common analysis-window boundaries (same job) | **Verified** |
| Sample-level inter-channel alignment / phase coherence / TDOA readiness | **Not verified** — not claimed |

---

## 8. Backpressure / drop counter map

| Counter | Capacity | Increment path | How to read |
| --- | ---: | --- | --- |
| `sp_mic_pcm_pending_overwrite` / L0 `overwrite` / AE `pcm_overwrite_events` | 1 pending PCM job | Prior job not taken when new window published (`ear_mic.c`) | L0 perf / AE diag |
| Manager `evq_dropped` | `EAR_OBS_MGR_EVENT_Q` = **8** | `ear_obs_manager_on_promotion` when `evq_push` fails (**R0 now counts**) | `obs_mgr diag … evq_drop=` |
| CE `create_dropped_full` | `CE_MAX_CANDIDATES` = **8** | Create when no free/evictable slot | `ce diag` / health enrich |
| Critical `drop_busy` | active Critical = **1** | Second START while busy | Critical diag / tests |
| Manager `reserve_full` | joint held+pending ≤ 2 | Reserve admission | `obs_mgr diag` |
| AE history | 32 | Ring overwrite (silent aging) | capacity only |
| L0/L1/L2 obs rings | 64 | Ring overwrite; may set `history_overflow` | interval flags |

---

## 9. Historical / unqualified timings (do not merge)

| Source | Claim | Identity gaps | Class |
| --- | --- | --- | --- |
| `ear-l0-firmware-integration-notes.md` EAR-001 2026-11-02 | L0 `us_last≈160–169 ms`, overwrite=0, pcm 129408, free_heap/spiram as above | Pre multi-mic Stage 1; L1/L2/AE/CE stack not current; CPU MHz not on that table | **historical / unqualified** for current main |
| `ear-l2-acoustic-dynamics-contract.md` user-reported | L0 ~62/67 ms, L1 ~40/45 ms @ 160 MHz | Missing git_sha, firmware_version, duration, analyze_n on Map identity | **historical / unqualified** |
| Host x86 L0/L1 µs in perf docs | Faster than S3 | Wrong target | **host_only** — never device |

---

## 10. Operator procedure (EAR-002 mono, EAR-001 dual)

**Do not flash/OTA from the agent.** Operator builds/flashes an image that includes
this R0 branch (or equivalent) with serial diag enabled.

### Image requirements

- Git SHA recorded (`git rev-parse HEAD`)
- IDF v5.4.4
- `SERIAL_DIAG` + `AE_DIAG` on for measurement image
- Note NVS `microphone_count`: **1** on EAR-002, **2** on EAR-001
- Desired OTA quiet / pinned so the image is not replaced mid-soak

### Warm-up

1. Boot; wait until Wi-Fi/MQTT connected (or explicitly run **offline** and note it).
2. Wait **60 s** after `analysis PCM ready` / first `L0 perf` with `analyze_n` advancing (boot settle + ring fill).

### Idle soak (required)

- Duration: **15 minutes** wall clock after warm-up.
- Workload: ambient room; no forced tones; MQTT may stay connected.
- Why 15 min: ≈900 one-second windows → enough for last/avg/max stability and
  overwrite detection without claiming long-term soak / `verified_operating`.

### Loaded soak (required)

- Duration: **10 minutes**.
- Workload: continuous broadband stimulus or speech/music near both mics (EAR-001)
  or the single mic (EAR-002) so activity may START; allow CE/Obs if they promote.
- Do **not** run Hub Desired OTA during the soak.

### Values to capture (copy into Map templates)

From UART:

1. Boot: `analysis PCM ready pcm_cap=… ch=… bytes=… free_heap=… free_spiram=…`
2. I2S: `dma_desc_num` / `dma_frame_num` (log or health)
3. Periodic `sp: res …` (cpu_mhz, configured_mhz, heap/SPIRAM/internal, stack lines for `sp_mic` / `ear_obs`)
4. `ear_obs: L0/L1/L2 perf …` (n, last, avg, max, overwrite, active_mics)
5. `ae: timing …` analyze / post_dsp
6. `ear_obs: R0 stage …` promo_mgr / publish
7. End-of-soak: `overwrite`, `evq_drop`, `ce … create_dropped`, Critical `drop_busy` if exercised

### Expected counter behavior (baseline acceptance hints — not pass/fail until filled)

- Idle: `overwrite=0`, `evq_drop=0`, `create_dropped_full` typically 0
- Loaded: overwrite should stay 0 if analyze keeps up; if >0, record as bottleneck

### After soak

Fill [`docs/firmware-map/records/R0-TEMPLATE-ear-002-mono-mic.md`](../firmware-map/records/R0-TEMPLATE-ear-002-mono-mic.md)
or [`R0-TEMPLATE-ear-001-dual-mic.md`](../firmware-map/records/R0-TEMPLATE-ear-001-dual-mic.md)
with **measured** values only. Leave unused cells `n/a`.

---

## 11. Decisions for review

1. Product CPU is **240 MHz** / 100 Hz in `sdkconfig.defaults`; R0 160 MHz remains overlay-only (`r0_ref`).
2. Accept 15 min idle + 10 min loaded as sufficient for first `measured` records (not `verified_operating`)?
3. When L2 fused, treat L0 `analyze_us` and L2 `combined_us` as the **same** pass (do not add)?
4. Schedule Test A (mic0/mic1 physical L/R) before trusting pair Δ as left/right?
