# Dual-mic data flow, logging ownership, and reporting inventory

**Branch:** `firmware/multi-mic-consistent-platform` (moved from investigate PR #47)  
**Scope:** Source inspection snapshot (post PR #46 / Sensor Platform Step 1).  
**Follow-up implementation:** `docs/diagnostics/multi-mic-platform.md` (per-mic background + config max 4 / acquire max 2).  
**Original investigate constraints:** no renames, no engines, no flash/OTA in that inventory pass.

Verified facts cite paths and symbols. Recommendations are labelled separately.

---

## 1. Dual-mic data path

### Acquisition (Platform `mic`)

| Step | Location | Behaviour |
| --- | --- | --- |
| Live count | `sp_mic_start` in `firmware/platform/mic/ear_mic.c` | `s_live_mic_count` from NVS/`sp_mic_count_*` (1 or 2). |
| I2S RX | `init_i2s_rx()` | Stereo when `s_live_mic_count >= 2`: Philips slot stereo; mono otherwise. Shared BCLK/WS/DIN0. |
| DMA / read | `measure_window()` | `i2s_channel_read` into `s_slot_buf`; stereo words are `[L,R]` pairs. |
| Channel split | `measure_window()` stereo loop | Even slot → mic 0 (left), odd → mic 1 (right); `ch_accum_*` per channel. |
| Per-channel metrics | `finalize_channel()` for each live index | Independent RMS, peak, clip, presence, sample counts. Shared transport counters copied to both. |
| Latest store | `ear_mic_task` | Writes `s_latest[0..]` under critical section. |
| APIs | `sp_mic_get_levels` / `get_channel` / `get_stream` | `get_levels` **always returns mic 0**. Channel/stream expose both when live. |

Comment in `measure_window` (verified): analysis PCM stores **mic0/left only**. Stereo loop writes PCM only from `left` when `CONFIG_DBMAP_EAR_MIC_L0` is on.

### Reported State (hello/health/keepalive)

`enrich_health()` → `audio.microphones[]` for each live mic via `enrich_mic_object()` (RMS/peak/samples/status/etc.).

Background attachment is **not** symmetric (see §2).

### Product acoustic / observation

| Path | Input | Channel |
| --- | --- | --- |
| Activity / Relative END | `sp_mic_get_levels()` | Mic 0 only |
| L0/L1/L2 PCM | `sp_mic` PCM ring (left samples only) | Mic 0 only |
| L0 interval `mic_index` | `dbmap_l0_interval_accum_init(..., 0, ...)` in `ear_observation.c` | Hard-coded `0` |

---

## 2. Mic 1 background fields — exact reason absent

**Verified cause (not baseline_freeze, not init race):** deliberate Iteration-1 limit in Platform enricher.

In `firmware/platform/mic/ear_mic.c` `enrich_health()`:

```c
#if CONFIG_DBMAP_EAR_MIC_BACKGROUND
        if (i == 0) {
            /* Background RMS remains mic-0 only (Iteration 1). */
            sp_bg_rms_profile(...);
            // attach mic.background only when bg.present
        }
#endif
```

Matching producer path in `ear_mic_task`:

- Candidate built **only from `snaps[0]`**.
- `sp_bg_rms_push(&s_bg_hist, snaps[0].rms_dbfs, ...)` — **single** history ring `s_bg_hist`.

So Mic 1 can have RMS/Peak/Samples (same measure path) while `background` is **omitted from JSON** for index ≠ 0.  
`ear_audio_compare` then prints `—` for Bg\* because `_background_block(mic)` is empty (`scripts/ear_audio_compare.py`).

**Note:** Kconfig help for `DBMAP_EAR_MIC_BACKGROUND` says `audio.microphones[i].background` (all `i`), which **does not match** the `i == 0` gate. That is a docs/Kconfig wording drift, not a second runtime path.

---

## 3. Observation JSON vs `ear_audio_compare`

### `ear_audio_compare` (Hub Reported State)

- Source: Hub `GET /api/v1/nodes` → `reported.payload.audio` (`fetch_nodes` / `_audio_block`).
- That payload is built on-device by Platform `enrich_health` (mic enricher), published on hello/health/keepalive via `sp_health_*` + `sp_mqtt_publish_json`.
- Both mics appear when `live_mic_count==2`; script iterates `audio.microphones` independently.
- Missing Bg fields = missing `background` object on that mic object (deliberate omission), **not** an unavailable RMS measurement.

### MQTT Observation messages (`ear_obs`)

- Built by product `dbmap_ear_observation_publish` → `dbmap_observation_build_json` → `sp_mqtt_publish_json(cfg->topics.observation, ...)`.
- Acoustic measurements use L0/L1/L2 from **mic-0 PCM / mic-0 levels**; `measurements.mic_index` is set from accumulator init with **`0`**.
- Observation JSON is **not** a dual-mic Hub compare dump; it is episode lifecycle + product acoustic schemas.
- Log line `ear_obs: published observation_id=...` is product publish success after Platform MQTT helper returns OK.

---

## 4. Existing audio functionality (inventory)

| Capability | Where | Classification |
| --- | --- | --- |
| I2S RX, stereo unpack, DMA counters | `ear_mic.c`, `i2s_stereo.c` | (1) Generic HW/acquisition |
| Per-channel RMS/peak/clip/presence | `measure_window` / `finalize_channel` | (2) Generic sensor measurement |
| Rolling background RMS hist | `background_rms.c` + single ring in `ear_mic.c` | (2) Generic measurement; **wired mic-0 only** |
| Reported `audio.microphones[]` enricher | `enrich_health` | (2) Platform reporting of sensor values |
| PCM window hand-off | `sp_mic_pcm*` | (1)/(2) Platform acquisition for product |
| L0/L1/L2 analysis | `ear_acoustic/*` | (3) Ear-specific acoustic |
| Activity / Rel END / episode lifecycle | `ear_observation/*`, `activity_detector.c` | (4) Ear observation/reporting |
| Observation MQTT JSON | `observation_payload.c`, `dbmap_ear_observation_publish` | (4) Ear-specific; uses Platform MQTT + Observation Foundation helpers |
| Sequence / episode registry / emit gate | `firmware/platform/observation/*` | Platform foundation (generic), consumed by Ear |

No separate “Audio Engine” or “Reporting Engine” module exists by those names; the behaviours above already cover much of the intended split.

---

## 5. Log-tag ownership

| Tag | File | Owns |
| --- | --- | --- |
| `sp_mic` | `firmware/platform/mic/ear_mic.c` | Platform mic task/I2S/window logs (file still named `ear_mic.c`) |
| `ear_acoustic` | `ear_acoustic_runtime.c` | Ear L0/L1/L2 runtime |
| `ear_obs` | `ear_observation.c` | Ear observation lifecycle + **product** publish |
| `ear_act` | `activity_detector.c` | Ear activity / Rel END detector |
| `sp_mqtt`, `sp_health`, … | `node_common/*` | Platform transport / health |

**`ear_obs: published observation...`:** Ear product responsibility. It constructs Ear Observation JSON and calls Platform `sp_mqtt_publish_json`. That is **not** already a generic Platform “Reporting Engine”; promoting publish orchestration to a shared Reporting Engine would be an **architectural change**, not a rename of `ear_obs` → `sp_obs`.

**Intended naming vs today:** Platform acquisition already logs as `sp_mic`. Residual product naming is mostly filenames (`ear_mic.c`) and Ear tags (`ear_*`), which matches ownership for observation/acoustic.

---

## 6. Architecture assessment

| Capability | Assessment |
| --- | --- |
| Dual-channel I2S measure + Reported RMS/peak | Implemented in correct Platform layer |
| Dual-channel background hist | Partially implemented: algorithm generic, **ownership/wiring mic-0 only** |
| Observation / activity / L0–L2 on mic 0 | Implemented as Ear product on Platform primary API — correct for current contract; dual-mic observation **not implemented** |
| Observation publish | Ear-owned + Platform MQTT helper — clear; Reporting Engine would be new boundary |
| Log tags | Mostly aligned (`sp_mic` / `ear_obs`); filename `ear_mic.c` is cosmetic mismatch |
| Kconfig background help text | Misleading vs `i == 0` gate |

**Genuinely missing (for dual-mic parity):** per-index background rings + enrich attach; optional product decision to feed observation from a selected/aggregated channel (not present).

---

## 7. Recommended next smallest step

**Extend Platform Iteration-1 background to per-live-mic histories** (one ring per index, push/profile/attach for each `i < live_count`), and fix Kconfig help to match.

- Small, evidence-based, confined to `ear_mic.c` (+ maybe static RAM sizing) and tests of `background_rms` / enrich.
- Directly fixes the `ear_audio_compare` Mic 1 Bg\* asymmetry.
- Does **not** require an Audio/Reporting Engine, observation schema change, or log-tag rename.

Defer dual-mic observation/L0 until product policy chooses which channel(s) drive episodes.

---

## Source index (primary)

- `firmware/platform/mic/ear_mic.c` — `measure_window`, `ear_mic_task`, `enrich_health`, `sp_mic_get_levels`
- `firmware/platform/mic/background_rms.c` / `include/sp_background_rms.h`
- `firmware/platform/mic/Kconfig` — `DBMAP_EAR_MIC_BACKGROUND*`
- `firmware/products/dbmap/ear/ear_observation/ear_observation.c` — publish + `sp_mic_get_levels` + L0 `mic_index` 0
- `firmware/products/dbmap/ear/ear_acoustic/ear_acoustic_runtime.c` — PCM → L0
- `scripts/ear_audio_compare.py` — Hub reported `audio.microphones` formatting
