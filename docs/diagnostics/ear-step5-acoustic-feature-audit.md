# Step 5 — L0/L1/L2 Acoustic Feature and Observation Audit

**Status:** Investigation only (no code changes).  
**Scope:** What is implemented and preserved from EAR firmware through Hub storage/API.  
**Hardware context (deployed):** ESP32-S3-N16R8, INMP441, firmware reported as 0.7.11.  
**Repo defaults checked:** `firmware/esp-ear/sdkconfig.defaults` still lists `CONFIG_DBMAP_FIRMWARE_VERSION="0.7.0"` — **deployed 0.7.11 build metadata is not verified from this tree alone**.

**Primary sources:**  
`firmware/ear_components/ear_mic/{acoustic_l0,l1,l2}.c`, headers;  
`firmware/ear_components/ear_observation/{activity_detector,ear_observation,observation_payload,episode_registry}.c`;  
`hub/bootstrap/app/{schemas,service,models}.py`;  
contracts under `docs/ear-l0-measurement-contract.md`, `docs/ear-l1-signature-contract.md`, `docs/diagnostics/ear-l2-acoustic-dynamics-contract.md`.

---

## 1. Executive summary

The EAR implements a **combined-scene** (not per-source) pipeline:

1. **1 s PCM windows** @ 16 kHz (INMP441 / I2S).
2. **L0** time/band energy features per window → interval aggregates on Observation reports.
3. **L1** 1024-pt Hann FFT spectral signature (centroid, bandwidth, dominant, flatness).
4. **L2 Acoustic Dynamics** (spectral flux, envelope CV, dominant Δf) fused into the L1 FFT path — **not** a classifier.
5. **RMS activity detector** drives START / PERIODIC / END (Relative END v1 available).
6. Hub **stores the full hierarchical JSON blob** (`observations.payload_json`) after Pydantic validation; API returns that payload plus `received_time_utc`.

**What works today:** interval-scoped L0/L1/(optional L2) features ride on every Observation when compile flags match `sdkconfig.defaults` (`PUBLISH_L0`, `L1`, `L2`+`L2_PUBLISH`). Hub accepts and round-trips those schemas strictly.

**Critical naming note:** Hub `acoustic.classification` schema is documented as “L2 source hypothesis” (`schemas.py` ~372), while firmware **L2** means **Acoustic Dynamics** (`acoustic.dynamics`, schema `"l2"`). Physical Ears always publish `classification: null`. Do not conflate them.

**Classifier readiness (short):** The stack provides a **measurable interval feature baseline** (level, coarse bands, spectral shape, short-term dynamics) sufficient for **offline exploratory labeling and simple explainable heuristics**. It is **not** yet a foundation for a robust drone classifier: no per-window uplink, no mel/MFCC/embedding, no periodicity/HNR, no multi-mic/spatial, no calibration to SPL, and PERIODIC aggregation discards fine time structure inside each 5 s report interval.

---

## 2. L0 feature matrix

**Defaults:** `CONFIG_DBMAP_EAR_MIC_SAMPLE_RATE_HZ=16000`, `CONFIG_DBMAP_EAR_MIC_WINDOW_MS=1000`, `CONFIG_DBMAP_EAR_OBS_L0_MAX_WINDOWS=64`  
**Analyze:** `dbmap_l0_analyze_window` — `acoustic_l0.c` ~240–344  
**Interval:** `dbmap_l0_interval_accum_{init,add,finalize}` — `acoustic_l0.c` ~346–640  
**Export gate:** `CONFIG_DBMAP_EAR_OBS_PUBLISH_L0` (defaults `y` in esp-ear)

| Feature | API / JSON field | Algorithm (code) | Units | Validity / clip | Window vs interval | Exported? |
| --- | --- | --- | --- | --- | --- | --- |
| RMS | `rms_dbfs` → `measurements.rms.{mean,min,max}_dbfs` | FS-normalized mean-square → `20*log10(rms)` via `amp_to_dbfs_f` (`acoustic_l0.c` ~312) | dBFS | Window always `valid` if samples>0; silence floor −120 | Both | Interval mean/min/max **yes**; per-window only serial diag / ring |
| Peak | `peak_dbfs` | Max \|sample\| / FS → dBFS (~313) | dBFS | — | Window only | **No** in MQTT; used for clip + crest |
| Crest | `crest_db` = peak−rms (~314) | dB | Undefined if !valid | Window; interval T3 | T3 aggregates **yes**; raw crest **no** |
| Clipping | `clipped`, `clipped_sample_count` | Sample near FS **or** peak ≥ −0.05 dBFS (`DBMAP_L0_CLIP_PEAK_DBFS`) (~315) | bool / count | Clipped windows still in means; **excluded from T3 crest** | Window → `clipped_window_count` | Count **yes**; `clipped_sample_count` **discarded** after window |
| Band energy B0–B3 | `band_energy_linear[4]`, `band_dbfs[4]` | Cascaded HP+LP Butterworth biquads per band; mean-square vs FS² (~298–322) | linear / dBFS | Bands: 20–250, 250–1k, 1k–4k, 4k–8k Hz (`dbmap_acoustic_l0.h` ~46–51) | Window → interval mean_dbfs + fraction | Interval **yes**; linear energies not in JSON |
| H/L ratio | `hl_db` | `10*log10((B2+B3)/(B0+B1))` if energy above floor (~330–339) | dB | `hl_valid` / `has_hl_mean` | Window + interval | Interval `hl.mean_db` **yes** |
| T1 | `t1.rms_stdev_db`, `rms_range_db` | **Sample** stdev (÷ n−1) of hist RMS (≥2); range max−min (`finalize` ~567–586) | dB | Cleared on `history_overflow` | Interval (≥2 windows) | **Yes** (null if unavailable). Differs from L1/L2 **population** stdev |
| T2 | `t2.rms_slope_db_per_s` | OLS slope vs mono time; ≥3 hist points (~588–595) | dB/s | Needs hist; overflow clears | Interval | **Yes** |
| T3 | `t3.crest_max_db`, `high_crest_count`, `high_crest_rate` | Max crest among unclipped; rate vs 12 dB threshold (`DBMAP_L0_CREST_THRESHOLD_DB`) (~597+) | dB / count / rate | Unclipped only (`n_crest`) | Interval | **Yes** |
| T6 | `t6.hl_slope_db_per_s` | OLS on hist HL where valid (~same finalize path) | dB/s | Overflow clears | Interval | **Yes** |
| `history_overflow` | internal `dbmap_l0_interval_t.history_overflow` | Hist cap exceeded while still counting means (`accum_add` ~424–428) | bool | Non-final publish **refused** (`ear_observation.c` ~531–534) | Interval | **Not in L0 JSON** (only L2 exports overflow) |

**Update interval:** one L0 analyze per completed mic window (~1 Hz). Observation reports aggregate windows with membership `start < window_end ≤ end` (`dbmap_l0_window_in_interval`).

**Invalid / missing windows:** unusable activity samples never credit detector quiet; L0 association skips `!valid`. Sequence gaps are handled in the activity detector hold logic, not by inventing L0 windows.

**Not implemented (do not assume):** T4/T5, per-band crest, calibrated dB SPL, source separation.

---

## 3. L1 feature matrix

**Analyze:** `dbmap_l1_analyze_window` / `_ex` — `acoustic_l1.c`  
**Features from power:** `features_from_power` — `acoustic_l1.c` ~152–235  
**Workspace:** `dbmap_l1_workspace_t` ≈ 22.5 KiB static (`dbmap_acoustic_l1.h` ~90–101)  
**Gate:** `CONFIG_DBMAP_EAR_OBS_L1` (defaults `y`) → `acoustic.signature` object; off → JSON `null`

| Feature | Field | Formula (code) | Window | Interval JSON | Notes |
| --- | --- | --- | --- | --- | --- |
| Spectral centroid | `centroid_hz` | Σ f·M / Σ M over 80–7600 Hz (~204) | Yes | `centroid_hz.{mean,stdev}` | Magnitude-weighted |
| Bandwidth | `bandwidth_hz` | √(Σ (f−c)² M / Σ M) (~215) | Yes | `bandwidth_hz.{mean,stdev}` | |
| Dominant frequency | `dominant_hz` | Bin of max power × bin_hz (~216) | Yes | `dominant_hz.{mean,stdev}` | Also feeds L2 Δf |
| Spectral flatness | `flatness` | geo(M)/arith(M) clipped [0,1] (~217–228) | Yes | `flatness.{mean,stdev}` | Bins with M>1e−30 |
| Frame count | `frame_count` | ⌊N/1024⌋ non-overlapping Hann frames | Yes | Not exported | ~15 frames @ 16 kHz / 1 s |
| Clip / valid | `clipped`, `valid` | Peak ≥ −0.05 dBFS; energy floor 1e−12 | Yes | Via `invalid_window_count` | Clipped excluded from means |

**Aggregation:** arithmetic mean + **population** stdev (`sample_stdev` uses `sum_sq/n − mean²`, `acoustic_l1.c` ~556–562) when `window_count≥2`. Contract text matches population; function name is misleading.

**Frame coverage:** Non-overlapping 1024-sample frames @ 16 kHz / 1 s → **15 frames**; trailing **576 samples discarded** (no zero-pad) — `acoustic_l1.c` frame loop.

**Dependencies:** Runs on same SPIRAM PCM as L0 after L0 in `ear_mic` poll path; does **not** use L0 feature values as inputs.

**History:** Interval accumulators are independent of L0 hist arrays; association rule identical. No multi-window L1 features beyond mean/stdev over the report interval.

**Planned / out of scope (contracts):** rolloff, HNR, MFCC, embeddings, classification — **not implemented**.

---

## 4. L2 feature matrix (Acoustic Dynamics)

**Contract:** `docs/diagnostics/ear-l2-acoustic-dynamics-contract.md`  
**Code:** intra metrics in `dbmap_l1_analyze_window_ex` (`acoustic_l1.c` ~430–509); continuity `dbmap_l2_continuity_push` (`acoustic_l2.c` ~14–53); interval `acoustic_l2.c` ~92–180  
**Compute gate:** `CONFIG_DBMAP_EAR_OBS_L2` (defaults `y`)  
**Publish gate:** `CONFIG_DBMAP_EAR_OBS_L2_PUBLISH` (defaults `y`) — if compute-only, dynamics pointer is NULL → JSON `null`

| Feature | Field | Algorithm | Temporal scope | Validity | Exported |
| --- | --- | --- | --- | --- | --- |
| Spectral flux | `spectral_flux` | Per consecutive FFT frames: Σ\|P−Pprev\|/ΣP over analysis band; mean over pairs (`acoustic_l1.c` ~467–492) | Intra-window (frames) | Needs ≥1 frame pair + energy floor | Interval mean/stdev + `valid_count` |
| Envelope CV | `envelope_cv` | Population σ/μ of per-frame RMS; frames with RMS > 1e−6 (`~445–505`) | Intra-window | Needs ≥2 RMS frames | Interval mean/stdev + `valid_count` |
| Dominant delta | `dominant_delta_hz` | \|dom_i − dom_{i−1}\| if consecutive windows within ±100 ms of nominal window (`acoustic_l2.c` ~35–43) | Inter-window | Both windows eligible (L1 valid, unclipped, frames≥2) | Interval mean/stdev + `valid_count` |
| Eligibility | `eligible` | L1 valid ∧ !clipped ∧ frame_count≥2 | Window | Ineligible → `invalid_window_count` | Not a JSON field |
| History overflow | `history_overflow` | Option A: clear all metrics (`mark_overflow`) | Interval | Hub enforces empty metrics | **Yes** in dynamics JSON |

**Relationship with L1:** Same FFT pass; L1 outputs unchanged when L2 intra is requested (`dbmap_acoustic_l1.h` ~125–128). Dominant Δf uses L1 `dominant_hz` via continuity cache (broken on episode release — `ear_observation.c` `l2_continuity_break`).

**Not implemented:** classifier, band-split flux, periodicity detectors, mel spectrograms (explicitly out of L2 investigation scope).

---

## 5. Observation lifecycle

### Detector state machine

Implemented in `activity_detector.c` / `dbmap_activity_detector.h`:

`IDLE → PENDING_ENTER → ACTIVE → PENDING_EXIT → IDLE`

| Transition | Trigger (verified) |
| --- | --- |
| IDLE → PENDING_ENTER | Usable RMS ≥ enter (−45 dBFS default) |
| PENDING_ENTER → ACTIVE + **START** | Hold ≥ `min_activity_ms` (2000) still above enter |
| ACTIVE → PERIODIC | Every `observation_interval_ms` (5000) while **ACTIVE** (also possible on the enter-PENDING_EXIT / cancel-to-ACTIVE call via `maybe_periodic`). **Not emitted while remaining in PENDING_EXIT.** |
| ACTIVE → PENDING_EXIT | Abs: RMS < exit (−55); Rel (if armed): RMS ≤ baseline+margin **or** abs |
| PENDING_EXIT → ACTIVE | Cancel (abs: RMS≥exit; rel: RMS > baseline+margin+hyst) |
| PENDING_EXIT → IDLE + **END** | Continuous exit-hold elapsed ≥ latch hold (abs 1000 ms / rel 2000 ms) |
| Forced END | `max_invalid_hold_ms` (30000) without usable samples → `termination_reason=invalid_hold` |

Relative END v1: freeze median IDLE baseline at START; arm on contrast; latch relative/absolute at PENDING_EXIT entry. See `docs/diagnostics/ear-relative-end-v1.md`.

Acoustic L0/L1/L2 **do not** drive enter/exit — only RMS activity does. Features are associated to emission interval bounds after the fact (`ear_observation.c` `handle_emission`, ring finalize).

### COMPLETE

| Path | Status |
| --- | --- |
| Payload + Hub schema allow `emission_kind=complete` | **Yes** (`observation_payload.c` ~15–19; `schemas.py` ~410) |
| `dbmap_episode_registry_deferred_decision` can return `"complete"` | **Yes** (`episode_registry.c` ~117–126) |
| Called from live RMS `ear_observation.c` path | **No** — only tests reference it |
| Phase 1 `TEST_PUBLISH_ONCE` publishes `"complete"` | **Yes** (`ear_observation.c` ~2080) |

**Verified conclusion:** On the production RMS activity path, short episodes still emit **START then END** (if START MQTT-accepted). **COMPLETE is schema-ready but not produced by the live detector path.**

### Accumulation during an episode

- Static rings capacity 64 (`EAR_OBS_L0_HIST_CAP`) of L0/L1/L2 window snapshots while active.
- On each emission, interval finalize walks ring entries with membership in `[interval_start, interval_end]`.
- After finalize, rings continue (circular). Full `64/64` is steady-state after ~64 s — **not** PCM overwrite.
- Episode registry: 2 slots (`DBMAP_OBS_EPISODE_SLOTS`); one RMS producer binding typical.

### Short vs long observations

- Short (< interval): START covers first ~min_activity windows; END covers remainder; may have few windows → many L0 T* / L1 stdev nulls.
- Long: PERIODIC every 5 s; each report is **only that slice**, not cumulative episode spectro-temporal series. Episode UUID stable; `episode_duration_ms` grows; `sequence_number` increments per report.

---

## 6. START / PERIODIC / END / COMPLETE payload matrix

All kinds share the same hierarchical envelope when publish succeeds (`dbmap_observation_build_json`, `observation_payload.c` ~308–436). Differences are context fields and which interval was finalized.

### Envelope (all kinds)

| Field | Type | Origin | Notes |
| --- | --- | --- | --- |
| `protocol_version` | 1 | const | |
| `message_type` | `"observation"` | const | |
| `identity.observation_id` | UUID string | episode registry | Stable across START/PERIODIC/END |
| `identity.node_id` | string | device | |
| `identity.hub_id` | string | may be `""` | |
| `identity.sequence_number` | uint | NVS-backed seq | Hub dedup key with node_id |
| `context.event_time_utc` | ISO-8601 | wall clock of **interval start** | Second resolution (`%Y-%m-%dT%H:%M:%SZ`); sub-second truncated (`ear_observation.c` ~367–405). Publish refused if clock unavailable |
| `context.capture_timestamp_monotonic_us` | int | mono of interval **end** | Interval start mono used only to derive wall `event_time_utc` |
| `context.timing_quality` | enum | sync state | |
| `context.clock_offset_ms` | number\|null | | Always null on physical Ear today |
| `context.timestamp_uncertainty_ms` | number | | |
| `context.interval_duration_ms` | number | emission duration | |
| `context.episode_duration_ms` | number | episode start→end | START: equals interval |
| `context.emission_kind` | start\|periodic\|end\|complete | | |
| `context.termination_reason` | natural\|invalid_hold\|ota_reboot | end/complete only | Omitted on start/periodic |
| `acoustic.signal_level_dbfs` | number\|null | detector emission RMS | **Not** the same as L0 `rms.mean_dbfs` |
| `acoustic.measurements` | L0 object | **required** | |
| `acoustic.signature` | L1 object\|null | L1 on/off | |
| `acoustic.dynamics` | L2 object\|null | L2 publish on/off | |
| `acoustic.classification` | **always null** on Ear | | |
| `spatial.bearing` / `position` | **always null** on Ear | | |

### L0 `measurements` (schema `l0`) — all published kinds

See §2. Concrete shape from `build_measurements_object` (`observation_payload.c` ~182–305).

### L1 `signature` (schema `l1`) when L1 enabled

`centroid_hz|bandwidth_hz|dominant_hz|flatness` each `{mean, stdev}` — `observation_payload.c` ~56–100.

### L2 `dynamics` (schema `l2`) when L2 publish enabled

`spectral_flux|envelope_cv|dominant_delta_hz` each `{valid_count, mean, stdev}` + `history_overflow` — ~127–179.

### Example (structure abbreviated; values illustrative)

```json
{
  "protocol_version": 1,
  "message_type": "observation",
  "identity": {
    "observation_id": "a5314310-0000-4000-8000-000000000001",
    "node_id": "EAR-001",
    "hub_id": "HUB-1",
    "sequence_number": 42
  },
  "context": {
    "event_time_utc": "2026-11-05T18:00:00.000Z",
    "capture_timestamp_monotonic_us": 1732000000,
    "timing_quality": "synchronized",
    "clock_offset_ms": 12.5,
    "timestamp_uncertainty_ms": 50.0,
    "interval_duration_ms": 5000.0,
    "episode_duration_ms": 270000.0,
    "emission_kind": "periodic"
  },
  "acoustic": {
    "signal_level_dbfs": -51.2,
    "measurements": {
      "schema": "l0",
      "version": 1,
      "band_plan_version": 1,
      "crest_threshold_version": 1,
      "crest_threshold_db": 12.0,
      "mic_index": 0,
      "sample_rate_hz": 16000,
      "analysis_window_ms": 1000,
      "window_count": 5,
      "clipped_window_count": 0,
      "n_crest": 5,
      "rms": {"mean_dbfs": -51.0, "min_dbfs": -52.1, "max_dbfs": -49.8},
      "bands": {
        "mean_dbfs": [-60.0, -55.0, -58.0, -70.0],
        "fraction": [0.2, 0.4, 0.3, 0.1]
      },
      "hl": {"mean_db": -2.5},
      "t1": {"rms_stdev_db": 0.8, "rms_range_db": 2.3},
      "t2": {"rms_slope_db_per_s": -0.01},
      "t3": {"crest_max_db": 9.0, "high_crest_count": 0, "high_crest_rate": 0.0},
      "t6": {"hl_slope_db_per_s": null}
    },
    "signature": {
      "schema": "l1",
      "version": 1,
      "sample_rate_hz": 16000,
      "analysis_window_ms": 1000,
      "fft_size": 1024,
      "freq_min_hz": 80.0,
      "freq_max_hz": 7600.0,
      "window_count": 5,
      "invalid_window_count": 0,
      "centroid_hz": {"mean": 1450.0, "stdev": 120.0},
      "bandwidth_hz": {"mean": 800.0, "stdev": 50.0},
      "dominant_hz": {"mean": 1100.0, "stdev": 200.0},
      "flatness": {"mean": 0.35, "stdev": 0.05}
    },
    "dynamics": {
      "schema": "l2",
      "version": 1,
      "sample_rate_hz": 16000,
      "analysis_window_ms": 1000,
      "fft_size": 1024,
      "freq_min_hz": 80.0,
      "freq_max_hz": 7600.0,
      "frame_hop_samples": 1024,
      "window_count": 5,
      "invalid_window_count": 0,
      "history_overflow": false,
      "spectral_flux": {"valid_count": 5, "mean": 0.12, "stdev": 0.03},
      "envelope_cv": {"valid_count": 5, "mean": 0.08, "stdev": 0.02},
      "dominant_delta_hz": {"valid_count": 4, "mean": 40.0, "stdev": 15.0}
    },
    "classification": null
  },
  "spatial": {"bearing": null, "position": null}
}
```

---

## 7. EAR → Hub data flow

```text
INMP441 I2S PCM (int32, ~24-bit FS)
        │
        ▼
ear_mic capture (SPIRAM double-buffer PCM)
        │
        ├─► RMS / mic health  ──► activity_detector (lifecycle)
        │
        └─► L0 analyze_window ──┐
            L1/L2 analyze_*   ──┼─► rings (cap 64)
                                │
                    emission bounds (START/PERIODIC/END)
                                │
                                ▼
                    interval finalize L0/L1/L2
                                │
                                ▼
              dbmap_observation_build_json (cJSON)
                                │
                                ▼
              MQTT topic esp-ear/local/<node>/observation
                                │
                                ▼
              hub mqtt_listener → ObservationBody (Pydantic)
                                │
                                ▼
              observations table:
                observation_id, node_id, sequence_number,
                event_time_utc, received_time_utc, payload_json
                                │
                                ▼
              GET /api/v1/observations → ObservationView
                (= payload + received_time_utc)
```

**Ingestion:** `ingest_observation` (`service.py` ~601–638) validates node, dedups `(node_id, sequence_number)`, stores `body.model_dump_json(by_alias=True)` as `payload_json`. **No feature recompute; no column-per-feature.**

**Post-ingest Hub quirk (verified):** `mqtt_listener.py` ~186 / ~192 references `body.observation_id` and `body.event_time_utc`, which do **not** exist on hierarchical `ObservationBody` (fields live under `identity` / `context`). Observation **persist still succeeds**; weather scheduling / duplicate log paths can raise `AttributeError` (caught or mis-logged). Does not strip stored acoustic fields.

**API:** `list_observations_page` reloads JSON into `ObservationView` (~724–729). Nested acoustic fields preserved if ingestion accepted them.

**Kconfig vs defaults:** Kconfig defaults `PUBLISH_L0` / `L2` / `L2_PUBLISH` to **n**; `firmware/esp-ear/sdkconfig.defaults` forces them **y**. Device behavior follows the built image, not bare Kconfig defaults.

---

## 8. Data retention and information-loss analysis

| Item | Fate |
| --- | --- |
| Raw PCM | Device-only; never MQTT |
| Per-window L0/L1/L2 scalars | Aggregated then **discarded** (except serial diag) |
| Peak dBFS, clipped_sample_count | **Lost** after window (clip count retained) |
| L0 linear band energies | Converted to mean_dbfs + fraction; raw Σ not stored |
| L0 `history_overflow` flag | Used to refuse publish; **not in measurements JSON** |
| L1 per-frame spectra / power_acc | Internal workspace only |
| L2 flux pair counts / env frame counts | Window-local; only means/stdevs uplink |
| Intra-interval timing finer than 1 s | **Lost** inside PERIODIC 5 s aggregates |
| Episode-long time series | **Not stored** as one object — only chained reports |
| Classification / bearing | Always null from Ear |
| MQTT drop while disconnected | Emission dropped; seq not consumed (`Kconfig` help; drop counter) |
| Rejected publish (overflow, clock, MQTT fail) | No Hub row |

**Transformations:** float → JSON number (cJSON double); Hub re-parses to Python float. No intentional quantization beyond IEEE binary64 JSON.

---

## 9. Data quality and performance limitations

### Quality (verified behavior)

- Clipped windows inflate RMS/band means but are excluded from L0 T3 and L1/L2 feature means.
- Invalid mic/transport windows: no quiet credit; can force `invalid_hold` END.
- L2 dominant Δf breaks across gaps >100 ms or episode boundaries.
- PERIODIC frequency 5 s ⇒ at most ~5 analysis windows of detail per report at 1 s windows.
- Ring cap 64 ⇒ max supported interval ≈ 62 s; default PERIODIC 5 s is safe. Longer custom intervals fail closed (refuse / overflow), not silent T* truncation.
- Relative END / abs thresholds are **provisional**, not calibrated SPL.

### Performance structure (code; device timings **not** re-measured this audit)

| Stage | Where | Cadence | Notes |
| --- | --- | --- | --- |
| L0 | `dbmap_l0_analyze_window` | every window | Stages: reset / sample_loop / post |
| L1 only | `dbmap_l1_analyze_window` | every window if L2 off | |
| L1+L2 fused | `analyze_window_ex` / `l2_analyze_window` | every window if L2 on | Combined timer in `ear_mic` L2 perf |
| Interval finalize | observation task on emit | on START/PERIODIC/END | Light vs FFT |
| Stack | `ear_obs` task 6144 bytes | — | HWM tracked when SERIAL_DIAG |
| L1 workspace | ~22.5 KiB | static/reuse | |
| PCM buffers | 2 × window in SPIRAM | — | `pending_overwrite` if analyze late |

Prior field notes (≈58 ms L0, ≈90 ms L1+L2 combined on one 0.7.x device) are **anecdotal** — do not treat as guaranteed for all paths/clocks.

### Potential risks (not fully verified in this pass)

- Numeric parity float vs double paths under very low energy.
- Hub `ObservationL2FeatureStats.mean` constrained `ge=0` — OK for flux/CV/\|Δf\|; would reject signed metrics if added later.
- `sdkconfig.defaults` firmware version string vs deployed 0.7.11 labeling.
- Detector-unusable windows: omitted from L0 counts; L1/L2 may still increment `invalid_window_count` (asymmetric association) — confirm per path when labeling.

### Stale documentation (do not treat as source of truth)

| Document | Issue |
| --- | --- |
| `docs/diagnostics/ear-l2-acoustic-dynamics-contract.md` header | Still says “Not implemented” — **stale**; compute+publish exist in firmware/Hub/`sdkconfig.defaults` |
| `docs/diagnostics/ear-l2-hub-compatibility-preparation.md` | Claims Hub has no `dynamics` — **stale** vs `schemas.py` |
| `docs/protocol.md` examples | Often omit `acoustic.dynamics`; incomplete vs current Ear serializer |
| `DBMAP_EAR_OBS_PUBLISH_L0` Kconfig help | Suggests optional L0 omission; builder still **requires** `measurements` |

---

## 10. Recommendations for the next development phase

1. **Treat current L0/L1/L2 interval vectors as the v1 feature contract** for offline datasets — do not redesign formulas until a labeled EAR corpus exists.
2. **Hub-side episode assembler** — implemented in Step 6 ([`ear-step6-episode-assembly.md`](./ear-step6-episode-assembly.md)); use it for labeling before on-device classifier work.
3. **Decide COMPLETE policy:** either wire deferred COMPLETE for sub-interval episodes or document that live path never emits it (today: latter).
4. **Export or explicitly document L0 `history_overflow`** asymmetry vs L2.
5. **Classifier prep experiments (Hub/offline first):** logistic/linear models on `[L0 rms+bands+hl+T*, L1 means, L2 means]` with explainability; measure drone vs ambient separability.
6. **Only if offline fails:** consider (a) shorter PERIODIC or partial per-window uplink, (b) periodicity/HNR features, (c) calibrated level — as separate RFCs, not silent firmware expansion.
7. **Keep Relative END / detector thresholds** out of the classifier feature discussion; they affect episode boundaries, not feature math.

---

## 11. Final assessment — drone acoustic classifier foundation

### Already available

- Deterministic, versioned L0/L1/L2 interval features on MQTT.
- Hub strict schema + full JSON retention + API round-trip.
- Episode identity + START/PERIODIC/END lifecycle with Relative END for ambient-above-exit cases.
- Host tests for L0/L1/L2 math and observation payload shape.

### Available but insufficient for a measurable, explainable drone classifier

- Features are **interval summaries**, not trajectories inside the event.
- Single combined scene; no spatial/multi-mic discrimination.
- No absolute acoustic calibration (dBFS only).
- No harmonic/periodicity descriptors typical for rotorcraft.
- Classification field unused; no labeled training loop in-product.

### Missing

- Per-window or spectrogram uplink (by design today).
- Mel/MFCC/embeddings, pitch/HNR, band-limited flux variants.
- Hub-side episode feature store / labeling tooling.
- COMPLETE on live RMS path.
- On-device or Hub classifier (`acoustic.classification` always null from Ear).

### Investigate / implement next

1. Offline episode join + labeled EAR-001 captures (drone flybys vs ambient).
2. Baseline explainable model on existing interval features; quantify confusion.
3. Gap analysis → feature RFC only where metrics fail.
4. Resolve COMPLETE / overflow reporting consistency for data hygiene.

**Verdict:** The current L0/L1/L2 + Observation pipeline is a **trustworthy measurement and transport baseline** for research and for building an **offline, explainable** classifier prototype. It is **not yet sufficient** as a production drone-classifier feature stack without episode assembly, labels, and almost certainly additional temporal/harmonic descriptors after the first baseline model is measured.

---

## Appendix — cross-check sources

Parallel source inventories (investigation-only) corroborated the matrices above and contributed the Hub listener quirk, PERIODIC-while-PENDING_EXIT rule, L1 trailing-sample discard, stdev convention split, and stale-doc list:

- [L0/L1/L2 feature inventory](82859664-2db4-4f09-b978-648d53ce1443)
- [Observation pipeline audit](0e082139-b441-4226-b56d-c3ed67a7d209)
- [EAR→Hub data-flow audit](57b822ad-b3d7-4237-aa6c-a65907acebb0)
