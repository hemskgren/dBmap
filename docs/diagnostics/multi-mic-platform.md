# Multi-microphone Platform (up to four)

**Branch:** `firmware/multi-mic-consistent-platform`  
**Physical check:** operator flash/OTA + EAR-001 (agent does not flash).

## 1. Hardware / interface arrangement

| Layer | Status |
| --- | --- |
| ESP32-S3 I2S | **`SOC_I2S_NUM = 2`**. STD mode has **one DIN per controller**. |
| Acquisition design | **Dual RX:** I2S0 **master** (DIN0) drives shared BCLK/WS; I2S1 **slave** (DIN1) takes the same clocks. |
| Wiring plan | Up to 4× INMP441: DIN0 L/R = indexes 0–1; DIN1 L/R = indexes 2–3 (`docs/device-wiring-and-pinout.md`). |
| Firmware acquisition | **`SP_MIC_ACQUIRE_MAX = 4`** — opens DIN0 always; opens DIN1 when live count ≥ 3. |
| Software config | **`SP_MIC_COUNT_MAX = 4`** — NVS/USB store 1–4; live = `sp_mic_acquire_count(configured)`. |
| Physically verified 4-mic board | **No** (pins remain placeholder / unverified). |

INMP441 is not a TDM mic: four channels require **two stereo DIN pairs**, not one controller with four slots. This firmware does **not** invent Mic 2/3 from duplicated DIN0 data.

**Physical requirement for Mic 2/3:** wire a second INMP441 pair to DIN1 (Kconfig default GPIO **7**), share BCLK/WS, strap L/R (GND/VDD). Without that wiring, firmware still opens I2S1 when `mic_count` ≥ 3; those channels will read floating/silent — not a silent fallback to two-channel acquisition.

## 2. Mic-0-only assumptions (product)

| Location | Classification |
| --- | --- |
| Platform measure / finalize / enrich / background | **Every acquired live index** |
| `sp_mic_get_levels` | Primary = index 0 (Observation / activity) |
| Analysis PCM / L0–L2 / Rel END | **Product mic-0** — unchanged |
| Observation `mic_index` | Hard-coded `0` — **product** |

## 3. What supports 1 / 2 / 3 / 4

| Capability | 1 | 2 | 3 | 4 |
| --- | --- | --- | --- | --- |
| NVS / USB count | yes | yes | yes | yes |
| I2S acquire | DIN0 mono | DIN0 stereo | DIN0 stereo + DIN1 mono L | DIN0+DIN1 stereo |
| Per-ch RMS/peak/samples | yes | yes | yes | yes |
| Per-ch background hist + enrich | yes | yes | yes | yes |
| Observation / L0 PCM | mic 0 | mic 0 | mic 0 | mic 0 |

## 4. Implementation summary

- Dual STD RX in `ear_mic.c` (`init_one_i2s_rx` / `accumulate_din_slots`).
- `sp_i2s_channels_on_din` / `sp_i2s_mic_index_for_din_slot` map DIN+slot → mic index.
- Per-DIN read counters; independent RMS/peak/clip/presence/background per index.
- Reported State: `live_microphone_count`, `acquisition_max` (4), `acquisition_limited`, per-mic `din`.
- Hub USB flashing UI writes NVS `microphone_count` **1–4** (same as firmware).

### Configured vs acquired (Reported State)

| Field | Meaning |
| --- | --- |
| `configured_microphone_count` | NVS/USB installation value (1–4) |
| `live_microphone_count` | Channels opened on I2S (`sp_mic_acquire_count`) |
| `acquisition_max` | `SP_MIC_ACQUIRE_MAX` (4) |
| `acquisition_limited` | true when configured > live (should be rare while MAX==4) |

### Verification status

| Check | Result |
| --- | --- |
| Host `test_mic_count` | Pass (`SP_MIC_ACQUIRE_MAX=4`) |
| Host `test_stereo_unpack` (dual-DIN map + unpack) | Pass |
| Host `test_background_rms` (4 independent + live=4) | Pass |
| Host `test_ear_mic_channel_api` / capture_diag format | Pass |
| ESP-IDF **v5.4.4** `esp-ear` build | Pass (series base `0.8.0`) |
| Physical EAR-001 four-mic | **Operator** (not done here) |

## 5. Background validity (disconnected / floating DIN)

Background is ambient learning for a **live** mic only:

- **Include** a window iff `microphone_status == signal_present` (plus existing transport/clip/settle rules). `unknown`, `disconnected_suspected`, and `noise_suspected` never seed the ring.
- **Publish** `audio.microphones[i].background` only when the **current** snapshot is still `signal_present` and the ring has in-window samples.
- On untrusted status, the per-mic ring is **cleared** so reconnect starts empty and UI shows `—` immediately.

## 6. EAR-001 four-mic physical procedure (operator)

**Do not flash from the agent.**

1. Wire Mic 0–1 on DIN0 and Mic 2–3 on DIN1 (shared BCLK/WS). Confirm free GPIOs vs placeholder map.
2. NVS `microphone_count=4`. Boot log: `live_mic_count=4` → `dual_din_stereo_lr`, `dins=2`.
3. `ear_audio_compare`: **EAR-001 Mic 0–3** each with independent RMS/Peak/Samples; Bg* only when that mic is `signal_present`.
4. With Mic 2/3 unwired: RMS/Peak −120, `disconnected_suspected`, **no** background object.
5. Silence or disconnect one wired mic at a time; other channels must not copy its levels.
6. Repeat with `mic_count=1` and `=2` to confirm regression.
