# Multi-microphone Platform consistency (up to four)

**Branch:** `firmware/multi-mic-consistent-platform`  
**Target physical check:** EAR-001 with **two** microphones (do not flash/OTA from this doc alone).

## 1. Hardware feasibility (four mics)

| Layer | Status |
| --- | --- |
| ESP32-S3 I2S | One STD RX port used today (`I2S_NUM_0`). A second DIN pair would need a second RX channel (or future TDM) sharing BCLK/WS — **not implemented**. |
| Wiring plan | Up to 4× INMP441: DIN0 L/R = indexes 0–1; DIN1 L/R = indexes 2–3 (`docs/device-wiring-and-pinout.md`). |
| Firmware acquisition | **`SP_MIC_ACQUIRE_MAX = 2`** — only DIN0 mono/stereo is opened. |
| Software config | **`SP_MIC_COUNT_MAX = 4`** — NVS/USB may store 1–4; live I2S count = `sp_mic_acquire_count(configured)`. |
| Physically verified 4-mic | **No** (out of scope for this change). |

Do not treat a software test with four rings as proof of four-channel I2S acquisition.

## 2. Mic-0-only assumptions (after this change)

| Location | Classification |
| --- | --- |
| Platform measure / finalize / enrich / background | **Generalized** for every **acquired** live index |
| `sp_mic_get_levels` | **By design** primary = index 0 (Observation / activity) |
| Analysis PCM / L0–L2 / Rel END | **Product mic-0** — unchanged; needs product decision for multi-channel acoustics |
| Observation `mic_index` | Hard-coded `0` — **product** |

## 3. What supports 1 / 2 / 4

| Capability | 1 | 2 | 4 (software) | 4 (physical I2S) |
| --- | --- | --- | --- | --- |
| NVS / USB count | yes | yes | yes (config) | n/a |
| I2S acquire | yes | yes | clamped to 2 | **not implemented** |
| Per-ch RMS/peak/samples | yes | yes | for acquired only | — |
| Per-ch background hist + enrich | yes | yes | rings sized for 4; live attach for acquired | — |
| Observation / L0 PCM | mic 0 | mic 0 | mic 0 | — |

## 4. Implementation summary

- `sp_mic_count.h` / `mic_count.c`: max 4; `sp_mic_acquire_count()`.
- `ear_mic.c`: per-index background rings; enrich all live mics; report `live_microphone_count`, `acquisition_max`, `acquisition_limited`, per-mic `din`.
- Slot naming: even=left, odd=right (DIN-pair contract).
- Kconfig / wiring docs aligned with behaviour.
- Host tests: mic_count 1–4 + acquire clamp; four independent bg histories; unacquired indexes omit background; channel slot names for indexes 0–3.

### Background RAM (static, when `CONFIG_DBMAP_EAR_MIC_BACKGROUND`)

Per index: `EAR_BG_RMS_CAP` samples where  
`EAR_BG_RMS_CAP = BACKGROUND_WINDOW_MS / WINDOW_MS + 8`  
(defaults: 180000/1000+8 = **188**).

Total sample storage: `SP_MIC_COUNT_MAX × EAR_BG_RMS_CAP × sizeof(sp_bg_rms_sample_t)`  
≈ `4 × 188 × 12` ≈ **9 KiB** plus `4 × sizeof(sp_bg_rms_hist_t)` metadata. Rings are allocated for all four indexes; only `s_live_mic_count` (≤2 today) is pushed/enriched.

### Configured vs acquired (Reported State)

| Field | Meaning |
| --- | --- |
| `configured_microphone_count` | NVS/USB installation value (1–4) |
| `live_microphone_count` | Channels opened on I2S (`sp_mic_acquire_count`) |
| `acquisition_max` | `SP_MIC_ACQUIRE_MAX` (2) |
| `acquisition_limited` | true when configured > live |

Hub **Firmware → Browser flashing → Configure device over USB** may write NVS `microphone_count` **1–4**. Values 3–4 are stored as configuration; current firmware still opens at most two I2S channels (`acquisition_limited`).

### Verification status

| Check | Result |
| --- | --- |
| Host `test_mic_count` | Pass |
| Host `test_background_rms` (independent + unacquired) | Pass |
| Host `test_ear_mic_channel_api` | Pass |
| ESP-IDF **v5.4.4** `esp-ear` build | Pass (host IDF); series base `0.8.0` in sdkconfig.defaults |
| Physical EAR-001 | **Not done** (operator procedure in §5) |

## 5. EAR-001 two-mic physical procedure (operator)

**Do not flash from the agent.** After you OTA/flash a build from this branch:

1. NVS `microphone_count=2` (or USB provision). Confirm boot log: `live_mic_count=2` → `stereo_lr`, no unexpected continuous spam (capture diag still opt-in).
2. Both mics wired on DIN0 (L/R straps). Hub `ear_audio_compare watch` (or list --json): Mic 0 and Mic 1 show RMS, Peak, Samples, and **BgMin/Mean/Max** when background is warm (after settle + enough windows).
3. Silence or disconnect **only** mic 1 (right): mic 0 levels/background should remain coherent; mic 1 presence/status should change without copying mic 0 RMS into mic 1.
4. Repeat silencing mic 0; mic 1 should stay independent.
5. Confirm L/R identity against straps (software contract: even=left). Treat absolute pin map as unverified until scoped.
6. With Wi-Fi: hello/health payload includes `audio.microphones[0].background` and `[1].background` when present; `configured_microphone_count` / `live_microphone_count` match expectation.

## 6. Recommended next step after physical verify

If two-mic background and independence look good: optional DIN1 / second I2S RX acquisition design (hardware + pin freeze) before claiming four-channel physical support. Separately, product decision on whether Ear observation/L0 should consume more than mic 0.
