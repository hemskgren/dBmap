# Ear I2S read-path verification

Status of the investigation into zero `sample_count` on Ears after a stall-path change that introduced `vTaskDelay(pdMS_TO_TICKS(5))`.

**Deployment rule:** do not fleet-OTA until **EAR-003** passes the targeted test below. Removal of the 5 ms delay is a **hygiene / scheduling** change, not a proven root-cause fix.

## Evidence classes

### Confirmed by source code (ESP-IDF v5.4.4 + this tree)

- `auto_clear` / `auto_clear_after_cb` is documented and implemented for **TX** DMA buffers only (`driver/i2s_common.h`; memset in the TX DMA callback path in `i2s_common.c`).
- `i2s_new_channel` copies `auto_clear_*` only onto `tx_chan`. This Ear image opens **RX only** (`tx_handle = NULL`), so `chan_cfg.auto_clear` has no effect on capture.
- RX overflow does **not** wipe the channel: when the RX `msg_queue` is full, the ISR drops the **oldest** queued buffer pointer and enqueues the newest (`i2s_dma_rx_callback`).
- `i2s_channel_read` blocks on that queue up to `timeout_ms` (`xQueueReceive`); a later `vTaskDelay` after a failed wait does not clear the RX queue.
- Default DMA pacing at 16 kHz with `dma_frame_num = 240` is about **15 ms per DMA buffer** (~90 ms for `dma_desc_num = 6`). A 5 ms sleep after a miss is shorter than one buffer period.
- In firmware, `sample_count` is assigned only from `pcm_count`, and `pcm_count` increments only inside the loop that extracts whole 32-bit slot words from `bytes_read`. Timeout / empty / hard-error paths do not increment it by themselves.
- Telemetry publishes **separate** counters: `timeout_reads`, `empty_reads`, `read_errors` (plus `counters_capped`). They are not merged into one reported metric.

### Confirmed by runtime telemetry (observed so far)

- At least one Ear (EAR-003) previously reported non-zero windows around **`sample_count ≈ 256`** (one filled `EAR_MIC_READ_FRAMES` read) with no mic attached.
- After builds that included the stall `vTaskDelay(5)` (and other diagnostic changes in the same period), Hub-reported Ear audio showed **`sample_count = 0`** across devices in the comparison table.
- Without a mic, window status is expected to be **`error`** (starved / insufficient fill) once fill-ratio rules are applied — that classification alone is not proof of a driver regression.

These observations establish **correlation in time**, not a controlled A/B on a single variable.

### Inferred but not yet verified

- That `vTaskDelay(5)` on the stall path was the **cause** of losing all samples. IDF RX semantics argue against that mechanism; the earlier “`auto_clear` discards RX DMA while sleeping” explanation is **incorrect**.
- That removing the delay (current tree: timeout waits only inside `i2s_channel_read`; empty path uses `taskYIELD()` once) will restore EAR-003’s intermittent ~256-sample windows.
- That all five devices share the same failure mode (vs mixed hardware / image / reporting timing).

### Requires physical hardware testing

- EAR-003-only OTA of the current build, then compare to the prior ~256-sample baseline via `scripts/ear_audio_compare.py watch` (and Hub reported state).
- Confirm I2S clocks (BCLK/WS) and DIN activity on a scope or logic analyzer.
- Confirm pin map and INMP441 L/R strap (`docs/device-wiring-and-pinout.md` still marks several items unverified).
- Optional A/B: same binary family with vs without an artificial post-stall delay, on one device, to test causality without relying on the disproven `auto_clear` story.
- Meaningful RMS/peak only after a real microphone is connected; not required for this I2S path check.

## Targeted EAR-003 acceptance (before any other device)

| Check | Pass hint (no mic) |
|-------|--------------------|
| I2S init | Not stuck in init `error` / `unavailable` from failed `i2s_new_channel` / enable |
| Data when driver has data | Some windows with `sample_count > 0` **or** clean high `timeout_reads` with `read_errors = 0` (driver alive, queue empty) |
| `sample_count` integrity | Increases only with processed frames; consistent with successful reads, not with error/empty alone |
| Counter separation | `timeout_reads`, `empty_reads`, `read_errors` independently populated as appropriate |
| Scheduling | MQTT hello/health remain stable; no capture-task disconnect storm |
| Status | Mic/audio status correctly reflects insufficient samples (`error` when fill ≪ expected) |

**Fleet OTA:** only after EAR-003 passes. Do not present delay removal as a proven root-cause fix unless a controlled test shows it.

## EAR-003 post-OTA: `timeout_reads=10000` with `sample_count≈16320`

**Cause (confirmed in source):** `i2s_channel_read(..., timeout_ms)` expects **milliseconds** and calls `pdMS_TO_TICKS(timeout_ms)` internally. The capture loop passed `pdMS_TO_TICKS(50)`. With default `CONFIG_FREERTOS_HZ=100`, that becomes `timeout_ms=5`, then `pdMS_TO_TICKS(5)=0` → non-blocking queue waits. Between DMA buffers (~15 ms), the 1 s window spins immediate `ESP_ERR_TIMEOUT` (often with `bytes_read==0`) until the 10 000 cap; real samples still arrive whenever a descriptor is queued (`sample_count` tracks extracted frames only). Counters are **per-window** (`memset` at start of `measure_window`); `counters_capped=true` means the display cap was hit (runaway/spin), not a meaningful stall rate.

**Fix:** pass `50` (ms), not `pdMS_TO_TICKS(50)`. Same mistake exists in `net_i2s.c` (Output path); left untouched here unless that image is revised separately.

**Fleet:** still EAR-003 only until timeouts look sane after this fix.

## False `signal_present` without a mic (EAR-001 investigation)

### Confirmed by source

- `dbmap_inmp441_extract_sample`: `(int32_t)slot_word >> 8` (left-justified 24-bit assumption, arithmetic sign-extend). Alignment still **unverified** on hardware.
- Peak uses absolute magnitude; `peak_dbfs == 0` means at least one `|sample| >= 2^23`.
- `microphone_status` today is **only** “transport usable AND peak &gt; −120 dBFS” → any non-all-zero window becomes `signal_present`.
- Measure loop processes only `bytes_read / 4` words from `i2s_channel_read` (not stale DMA beyond that).
- Same firmware on EAR-002…005 with all-zero extracted PCM correctly yields `disconnected_suspected`.

### Confirmed by telemetry (EAR-001 vs peers)

- EAR-001: transport healthy, `signal_present`, peak **0 dBFS**, RMS ≈ −35 dBFS, no timeouts — **no physical mic**.
- EAR-002…005: identical expected fill, `disconnected_suspected`, RMS/peak −120 (all-zero PCM).

### Root cause (classification)

Working I2S + floating/noisy DIN on EAR-001 produces **nonzero extracted PCM** (often including full-scale codes when MSBs float). The presence heuristic treats that as a microphone. Peers with quiet/zero DIN do not. This is **not** proof of a connected INMP441.

### Classification rules (`dbmap_audio_classify_microphone`)

Ordered checks (transport remains independent):

1. Transport not `ok`/`degraded` → `unavailable` (`transport_unusable`)
2. `sample_count == 0` → `unavailable` (`no_samples`)
3. Peak at silence floor or `zero_ratio ≥ 0.995` → `disconnected_suspected` (`digital_silence`)
4. Floating/noise patterns → `noise_suspected`:
   - `zero_ratio ≥ 0.85` and `full_scale_sample_count ≥ 1` (`mostly_zero_with_rail_hits`) — **EAR-001**
   - `zero_ratio ≥ 0.85` and `unique < 64` (`mostly_zero_low_unique`)
   - `identical_ratio ≥ 0.25` and `unique < 128` (`long_identical_runs`)
   - `full_scale_ratio ≥ 0.05` (`rail_heavy`)
   - peak ≈ 0 dBFS and `zero_ratio ≥ 0.50` (`peak_rail_sparse_energy`)
5. Ambient-like → `signal_present` (`ambient_like_energy`): unique rich (≥64 or capped), `zero_ratio < 0.85`, `fs_ratio < 0.02`, `identical_ratio < 0.25`, peak above silence
6. Else → `unknown` (`ambiguous_window`)

Also publishes `classification_reason`. Multi-window hysteresis is not required for EAR-001’s pattern; can be added later if quiet-mic vs noise flickers.

### INMP441 alignment

Left unchanged: `(int32_t)slot_word >> 8`. EAR-001 `raw_min = -8388608` (−2²³) is consistent with that model when the slot MSB is set. No I2S/format change without scope confirmation.

## Transient nonzero Peak/RMS after reboot

### Startup sequence (source)

1. `dbmap_ear_mic_start` → `init_i2s_rx`: `i2s_new_channel` → `i2s_channel_init_std_mode` → **`i2s_channel_enable`** (DMA RX starts).
2. IDF allocates RX DMA bufs with **`heap_caps_aligned_calloc`** (zeroed); then hardware overwrites them with DIN samples.
3. `xTaskCreate(ear_mic_task)` → first `measure_window` runs **immediately** (no warmup / no discard).
4. I2S is not stopped/restarted later; Wi‑Fi/USB recovery runs **before** mic start in `app_main`.

### What RMS/Peak use

Only `bytes_read / 4` words from each `i2s_channel_read` (including partial `ESP_ERR_TIMEOUT` fills). App `s_slot_buf` words past `bytes_read` are never included. RMS/Peak formulas unchanged.

### Working hypothesis (pre–window_seq capture)

Multi-device Peak **0** / RMS ≈ −28…−40 for up to ~1 minute after boot, then EAR-004/005 return to −120 while EAR-001 stays nonzero, fits **floating DIN electrical settle after clocks start**, not app-buffer reuse:

| Candidate | Evidence |
|-----------|----------|
| Uninitialized DMA | Unlikely — IDF `calloc`s DMA bufs |
| Stale app buffer | Unlikely — only `bytes_read` frames counted |
| Partial-read bug | Unlikely if `timeout_reads`/`partial_reads` stay 0 with full `sample_count` |
| Startup/DMA + floating noise | Likely — first windows sample unsettled DIN; some boards quiet down |

### Instrumentation (telemetry + first-30-window log)

`window_seq`, `ms_since_i2s_enable`, `read_calls`, `bytes_received`, `complete_frames`, `partial_reads`, plus existing zero/FS/RMS/Peak.

### Minimal proposed fix (after EAR-001/004 reboot capture confirms)

If early windows show Peak 0 with `partial_reads=0` and later windows go all-zero: **discard or suppress level reporting for the first N windows / M ms after `i2s_channel_enable`** (transport still `ok`). Do not change extract/RMS math or add blind delays without that evidence.

## EAR-001 vs EAR-004 (same image, different outcomes)

### A. EAR-001 — persistent sparse rail PCM

**Observed:** steady `noise_suspected` (`peak_rail_sparse_energy`), Peak 0, RMS ≈ −36…−38, full windows, no timeouts.

**Extraction check (source + host test):**

- `dbmap_inmp441_extract_sample(slot) = (int32_t)slot >> 8`
- `0x80000000u >> 8` → **−8388608** (−2²³) = exact 24-bit full-scale → Peak 0 dBFS
- Low 8 bits alone (`0x000000FF`) → **0** after `>> 8` — cannot invent FS rails
- Therefore Peak 0 with `raw_min = -8388608` means DMA delivered slot words with **bit 31 set**, not a post-shift artifact

**Cause ranking:** (4) floating/noisy DIN on EAR-001 (genuine DMA samples), not (3) extraction bug. Continues indefinitely → not (1) startup-only.

**Smallest software response:** keep structure classifier (`noise_suspected`). Hardware: check DIN wiring / add defined idle level. Do not change I2S format without scope proof.

**New diags:** `raw_slot_min`/`max` (+ hex), `slot_zero_count`, `slot_msb_set_count` — expect `slot_msb_set_count ≥ 1` when Peak is 0.

### B. EAR-004 — temporary thin window then silence

**Observed:** briefly missing/thin telemetry → `sample_count = 2560` (`degraded`) → full windows at −120 (`disconnected_suspected` / `digital_silence`).

**Source evidence:**

- `2560 = 10 × 256` (`EAR_MIC_READ_FRAMES`) — only ~10 full read buffers in a 1 s window (~16% of 16000 expected)
- Transport `degraded` when `sample_count < expected/2` (`dbmap_audio_window_status`) — correct for thin fill
- Mic start is **after** Wi‑Fi; first `measure_window` runs immediately after `i2s_channel_enable` with no DMA warmup
- Peers that stay silent show extraction of zeros is fine; EAR-004’s recovery to −120 shows levels path is OK once filled

**Cause ranking:** (1) I2S/DMA startup / early window before steady RX fill, and/or (2) measurement window accumulating little data while queue was empty (timeouts) — confirm with `window_seq`, `timeout_reads`, `partial_reads`, `ms_since_i2s_enable` on the thin window. Not an EAR-001-style persistent rail problem.

**Smallest justified fix (after diag confirm):** do not treat the first incomplete post-enable window(s) as steady state — e.g. skip publishing levels until `sample_count ≥ expected/2` or `window_seq > N` (transport may still report degraded/ok honestly). No format change.

## Read-result metrics (reported)

| Field | Meaning |
|-------|---------|
| `sample_count` | Whole PCM frames extracted and processed in the window |
| `expected_sample_count` | `rate_hz * window_ms / 1000` |
| `timeout_reads` | `i2s_channel_read` returned `ESP_ERR_TIMEOUT` |
| `empty_reads` | `ESP_OK` (or equivalent success) with no whole frames |
| `read_errors` | Any other `i2s_channel_read` error |
| `counters_capped` | True if a per-window counter hit the 10 000 display cap |
