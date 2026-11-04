# Step 4 — L2 Acoustic Scene Features: Investigation

**Status:** Investigation / design only. No firmware, protocol, or schema implementation in this document.  
**Hardware baseline (user-reported, EAR-001 @ 160 MHz):** L0 ≈ 62 ms last / 67 ms max; L1 ≈ 40 ms last / 45 ms max; no ring overwrites or invalid windows in the reported test.  
**Audio defaults:** 16 kHz mono, 1000 ms analysis window, FFT 1024 (L1), combined-mic scene only.

This report proposes a **non-classifying** L2 layer: compact acoustic *dynamics / structure* features that add information L0 and L1 do not already publish. It deliberately conflicts with the historical naming where “L2” meant `acoustic.classification` (`source_family` / confidence). That conflict is addressed in §6.

---

## 1. Existing architecture and reusable data

### 1.1 Layer roles today

| Layer | Code | What it is | What it is not |
| --- | --- | --- | --- |
| **L0** | `firmware/ear_components/ear_mic/acoustic_l0.c` | Combined-scene level, 4 IIR bands, crest, interval temporal T1/T2/T3/T6 | No FFT, no spectrum buffer, not signature |
| **L1** | `firmware/ear_components/ear_mic/acoustic_l1.c` | Interval mean/stdev of centroid, bandwidth, dominant, flatness from averaged 1024-pt Hann FFT | Not classification; discards per-frame spectra after averaging |
| **Observation** | `ear_observation` + `observation_payload.c` | Hierarchical protocol v1 JSON; interval association `start < window_end ≤ end` | No episode-level acoustic rollup |
| **Reserved “L2” slot** | Hub `ObservationClassification` / Ear always `classification: null` | Source-family hypothesis schema | **Not** scene-feature measurements |

Contracts: [`../ear-l0-measurement-contract.md`](../ear-l0-measurement-contract.md), [`../ear-l1-signature-contract.md`](../ear-l1-signature-contract.md), [`../protocol.md`](../protocol.md).

### 1.2 PCM lifecycle (constraints for L2)

1. I2S fills SPIRAM double-buffer `s_l0_pcm[fill_idx]` for one 1 s window (~16 000 int32 samples, 24-bit FS).
2. On completion, a pending analysis job points at that buffer; `fill_idx` flips. If the prior job was still pending → **overwrite** (window lost for analysis).
3. Observation task `dbmap_ear_mic_l0_poll()` runs **L0 then L1 on the same buffer**, then releases it for the next fill.
4. Only **scalar** `dbmap_l0_window_t` / `dbmap_l1_window_t` enter the observation rings (default capacity 64). PCM and L1 `power_acc[513]` are **not** retained after analyze returns.

Implication: any L2 feature that needs raw PCM or the per-frame / mean power spectrum must be computed **inside the same poll**, before the buffer is reused—preferably by extending the L1 FFT loop so the spectrum is not recomputed.

### 1.3 What L0 already publishes (do not duplicate)

**Per window:** `rms_dbfs`, `peak_dbfs`, `crest_db`, `band_energy_linear[4]` / `band_dbfs[4]`, `hl_db`, clip/validity.

**Per Observation interval:** RMS mean/min/max; band means & fractions; HL mean; plus:

| Feature | Meaning | Overlap with candidate “temporal L2” |
| --- | --- | --- |
| **T1** `rms_stdev_db`, `rms_range_db` | Cross-window level variability | Covers *inter*-window AM strength, not *intra*-window modulation rate |
| **T2** `rms_slope_db_per_s` | Level trend | Not periodicity |
| **T3** crest max / high-crest rate | Impulsiveness (unclipped) | Not repetition rate or envelope regularity |
| **T6** `hl_slope_db_per_s` | Band-balance trend | Not spectral flux |

L0 uses **IIR bandpass energy**, not FFT. There is no reusable spectrum from L0.

### 1.4 What L1 already publishes (do not duplicate)

**Per window (internal, then aggregated):** `centroid_hz`, `bandwidth_hz`, `dominant_hz`, `flatness` from the **mean** power spectrum over `⌊16000/1024⌋ = 15` non-overlapping frames (576 samples unused at the end of each second).

**Per interval:** mean + population stdev (`n ≥ 2`) of those four features; `invalid_window_count` for associated clipped/invalid windows.

| Existing L1 signal | What it does **not** capture |
| --- | --- |
| Flatness mean/stdev | Explicit tonality / HNR / harmonic series |
| Dominant mean/stdev | Directed frequency movement; within-second FM; frame-level peak hopping |
| Centroid/bandwidth stdev | Frame-to-frame spectral change while the mean spectrum looks stable |

Critical implementation detail (`dbmap_l1_analyze_window`): frame powers are **accumulated and averaged first**; features are extracted only from the average. Intra-window spectral dynamics are destroyed unless computed during the frame loop.

### 1.5 Diagnostics / performance instrumentation

- L0/L1 wall times: `dbmap_ear_mic_get_l0_perf` / `get_l1_perf` + L0 stage timers (`reset` / `sample_loop` / `post`), printed under `DBMAP_EAR_OBS_SERIAL_DIAG`.
- `dbmap_diag` counters cover MQTT/Wi-Fi/rejects—not acoustic analyze cost.
- Host L0/L1 µs are **not** S3 evidence; this report uses the **measured EAR-001** baselines above for budget discussion.

### 1.6 Host / golden test infrastructure

| Suite | Role for L2 design |
| --- | --- |
| `ear_mic/test_host/test_acoustic_l0.c` (+ parity) | Deterministic tones, silence, impulse, clip, interval association, T*, overflow |
| `ear_mic/test_host/test_acoustic_l1.c` | Tone / two-tone / noise / silence / clip; frame_count=15; mean/stdev |
| `ear_observation/test_host/test_observation_payload.c` | JSON shape; today asserts `classification: null` |
| Hub `test_observation_l0.py` | Schema `extra=forbid` ingest |

L2 should follow the same host-first pattern: pure C analyze + accumulators, synthetic PCM, then optional serialization behind a Kconfig gate.

### 1.7 Data reusable by L2 **without** another PCM pass

| Source | Reusable now? | Notes |
| --- | --- | --- |
| Ring / latest `dbmap_l0_window_t` scalars | Yes | Enough for cross-window deltas of RMS/crest/HL/bands |
| Ring / latest `dbmap_l1_window_t` scalars | Yes | Enough for cross-window spectral deltas / “stability” |
| Finalized L0/L1 interval structs | Yes | Already aggregated; weak for new physics |
| L1 `power_acc` after analyze | **No** (destroyed) | Would need handoff or in-loop compute |
| Per-frame power during L1 FFT | **No** today | Ideal hook for spectral flux / short-time tonality |
| Raw PCM after poll completes | **No** | Buffer may flip / overwrite |

**Episode registry** tracks identity, phases, and timing only—no acoustic state. Preserve START/PERIODIC/END reset semantics: L2 should be **interval-scoped** like L1, not a new episode rollup, unless a later phase explicitly adds episode accumulators with clear reset rules.

---

## 2. Candidate feature comparison

Legend for cost: **estimate** on ESP32-S3 @ 160 MHz unless marked measured. Scale from current L1 ≈ 40 ms for 15×1024 FFT + features.

### A. Temporal characteristics

| Candidate | Measures | L0/L1 enough? | PCM required? | Cost (est.) | Usefulness | Noise / placement sensitivity |
| --- | --- | --- | --- | --- | --- | --- |
| **Amplitude modulation depth** | Relative envelope swing inside 1 s | Partial: T1 is *across* windows | Yes, or subframe RMS during L0/L1 | Low if piggybacked on existing sample/frame loop (~1–3 ms); medium if separate PCM pass | High for engines, alarms, speech-like pulsing vs steady hum | High: SNR and distance compress modulation depth |
| **Periodicity / repetition rate** | Dominant envelope period (e.g. autocorr peak) | No | Yes (envelope or PCM) | **High** (O(N log N) or O(N·L) autocorr on 16 k samples) | High when stable (fans, idling), brittle otherwise | High; multipath and mic orientation smear peaks |
| **RMS envelope variation** | Stdev/CV of short-block RMS | Partial vs T1 | Subframe RMS (align to 1024-sample frames = free with L1) | **Low** (~1 ms) | Strong separator steady vs fluctuating sources | Medium; absolute RMS depends on gain/placement, CV less so |
| **Temporal regularity** | Envelope peak interval CV / entropy | No | Envelope track | Medium | Useful for “machine-like” vs irregular | High false irregularity in noise |
| **Repetition rate** | Same family as periodicity, often pulse trains | No | Envelope | Medium–high | Good for beeps / indicators | Thresholding fragile outdoors |

### B. Spectral characteristics over time

| Candidate | Measures | L0/L1 enough? | PCM / FFT required? | Cost (est.) | Usefulness | Sensitivity |
| --- | --- | --- | --- | --- | --- | --- |
| **Spectral stability** | How little spectrum changes across windows | Partial: L1 feature stdevs | No PCM if defined on L1 scalars; better on spectra | **Very low** from scalars; low from mean-spectrum L2 state | Good for stationary plant vs passing events | Placement affects absolute spectrum more than *stability* |
| **Spectral variation (consecutive windows)** | ‖Pₙ − Pₙ₋₁‖ or |Δdominant|, |Δcentroid| | Partial: stdev ≠ directed / pairwise | Prefer L1 ring scalars first | **Very low** | Complements stdev; captures step changes | Medium |
| **Dominant frequency movement** | Signed/abs Δf or path length of dominant | Partial: dominant stdev | Scalars OK; within-window needs per-frame peaks | Low (cross-window); medium (per-frame during L1) | High for sirens / Doppler / chirps | Bin quantization 15.625 Hz; noise peak-hopping |
| **Spectral flux** | Mean frame-to-frame spectral distance **inside** 1 s | **No** — L1 averages it away | Reuse L1 FFT frames; **no second PCM pass** | **Low–medium** (~2–8 ms + 513 floats prev-frame) | **Highest new information density** vs cost | Absolute flux rises with noise; normalize by energy |

### C. Tonal characteristics

| Candidate | Measures | L0/L1 enough? | Required input | Cost (est.) | Usefulness | Sensitivity / reliability @ 1024 / 16 kHz |
| --- | --- | --- | --- | --- | --- | --- |
| **Tonality index** | Peakiness of mean spectrum (e.g. spectral crest, or 1−flatness) | Partial: flatness already inverse-ish | Mean `power_acc` at end of L1 | **Very low** | Mild; largely redundant with flatness unless definition differs | Same as flatness |
| **Harmonic structure** | Integer-ratio peak sets | No | Mean or per-frame spectrum + peak pick | Medium–high | High *if* reliable | **Weak** at 15.6 Hz bins; missing fundamentals; outdoor SNR |
| **HNR** | Harmonic vs residual energy | No | Harmonic model | Medium–high | Speech/tonal vs noise | **Not recommended** at this resolution without careful gating |

### Recommendation filter (what *not* to ship first)

- **Autocorr periodicity / repetition rate:** expensive, brittle outdoors, overlaps detector cadence questions.
- **HNR / full harmonic model:** deferred in L1 contract for good reasons; keep deferred.
- **Tonality as 1−flatness alone:** little new information; skip or define a *different* spectral-crest metric only if flux+envelope are insufficient after field trials.
- **Anything that needs a second full PCM pass** while L0+L1 already ~100 ms: avoid.

---

## 3. Temporal scope and state management

### 3.1 Three scopes

| Scope | Cadence | State | Fits current lifecycle? |
| --- | --- | --- | --- |
| **Per 1 s analysis window** | Every successful L0/L1 poll | Ephemeral workspace (+ optional ring of window feature structs) | Yes — primary compute site |
| **Across consecutive windows** | Needs previous valid window features | 1-deep (or short) previous-feature cache, cleared on episode boundaries if desired | Yes — small static state |
| **Observation interval** | START/PERIODIC/END finalize | Accumulator like L1 (`sum`, `sum_sq`, counts) | **Yes — preferred publish scope** |
| **Entire episode** | Multi-interval | Extra registry fields + reset on END | **Defer** — changes semantics and memory; not required for meaningful L2 |

### 3.2 Trade-offs

- **Window-only publish:** too chatty / inconsistent with L0/L1 interval JSON.
- **Interval aggregates only:** matches protocol; loses ordering of events inside a 5 s PERIODIC unless stdev/deltas retained.
- **Episode aggregates:** useful later for long events, but conflicts with “each emission finalizes its interval” and needs overflow policy like L0 `history_overflow`.

### 3.3 Proposed state (preserve lifecycle)

1. Compute **per-window L2 scalars** during L1 analyze (and optionally one cheap envelope metric from the same frame loop).
2. Push `dbmap_l2_window_t` into a ring parallel to L0/L1 (same capacity / association rule).
3. On emission, `dbmap_l2_interval_accum_*` → interval object (means/stdevs + optional pairwise continuity metrics).
4. On episode END / registry clear: drop accumulators the same way L1 does—**no new reset semantics**.
5. Optional 1-deep `prev_dominant_hz` / `prev_flatness` for consecutive-window deltas; invalidate when `window_end` gap ≠ ~1 s or after clip/invalid.

---

## 4. Computational and memory cost estimates

### 4.1 Measured baseline (device)

| Stage | last | max | Notes |
| ---: | ---: | ---: | --- |
| L0 | ~62 ms | ~67 ms | User-reported EAR-001 @ 160 MHz |
| L1 | ~40 ms | ~45 ms | User-reported |
| **Sum** | **~102 ms** | **~112 ms** | Still ≪ 1000 ms window; overwrite risk is **task latency**, not average CPU alone |

Remaining soft budget before analysis becomes risky under load (Wi-Fi, MQTT, diag): treat **≤ 30 ms** additional analyze time as a design target; **≤ 15 ms** preferred for a first cut.

### 4.2 Estimated L2 incremental costs (not measured)

Assumptions: float math, reuse L1 frame loop, analysis band 80–7600 Hz (~485 bins), 15 frames/window.

| Work | RAM | Time estimate @ 160 MHz | Confidence |
| --- | --- | --- | --- |
| Frame RMS + envelope CV/modulation | ~20 floats hist | **+0.5–2 ms** (arith in existing PCM read) | Medium |
| Spectral flux (L1-normalized) vs previous frame power | +513 floats (~2 KiB) in L1 workspace | **+2–8 ms** | Medium–low (depends on band-limited loop quality) |
| Spectral crest / tonality from mean `power_acc` | 0 extra | **+0.2–1 ms** | Higher |
| Cross-window \|Δdominant\|, \|Δcentroid\| from ring | ~32 B | **≪1 ms** | High |
| Interval mean/stdev finalize | same class as L1 accum | **≪1 ms** | High |
| **Autocorr periodicity (full)** | multi‑KiB | **tens of ms** | Reject for v1 |
| **Second PCM pass for envelope** | 0 | **+5–15 ms** | Avoid if frame RMS exists |

**Reuse checklist**

| Goal | Approach |
| --- | --- |
| Reuse L1 FFT results | Compute flux (and optional per-frame dominant) **inside** the existing frame loop before `power_acc` average |
| Reuse spectral measurements | Cross-window stability from L1 window ring—no FFT |
| Avoid additional PCM passes | Derive envelope stats from per-frame time-domain energy while filling FFT input |
| Bound memory | No retained PCM; +~2 KiB workspace; +one `dbmap_l2_window_t` ring entry per hist slot (~32–48 B × 64) |
| Avoid unnecessary SPIRAM | Keep new workspace in internal BSS next to `dbmap_l1_workspace_t`; do not allocate SPIRAM spectra |

CPU frequency / PM: **unchanged** (160 MHz); L2 must fit without a clock bump.

---

## 5. Recommended minimal L2 feature set

### 5.1 Design principle

Add features that are **orthogonal** to published L0/L1:

1. **Intra-window non-stationarity** (L1 currently averages it away).
2. **Intra-window envelope dynamics** (L0 T1 is inter-window only).
3. **Cheap inter-window continuity** (directed change, not only stdev).

### 5.2 Minimal set (v1 proposal)

| ID | Feature | Scope | Definition (normative sketch) | Why it is new |
| --- | --- | --- | --- | --- |
| **F1** | `spectral_flux` | Per window → interval mean/stdev | For frames \(i=1..F-1\), band-limited \(d_i = \sum_k \|P_i[k]-P_{i-1}[k]\| / \sum_k P_i[k]\) (FS-relative power); window value = mean \(d_i\). Invalid if &lt;2 frames or energy floor. | L1 never exposes frame-to-frame change |
| **F2** | `envelope_cv` | Per window → interval mean/stdev | Per FFT-aligned frame, \(r_j = \mathrm{RMS}(\mathrm{frame}_j)\); \( \mathrm{cv} = \mathrm{stdev}(r)/\mathrm{mean}(r) \) with mean&gt;ε. | Not equal to crest or T1 |
| **F3** | `dominant_delta_hz` | Per window pair → interval mean abs | \( \|f^\mathrm{dom}_n - f^\mathrm{dom}_{n-1}\| \) for consecutive **valid unclipped** windows with expected 1 s spacing; else skip. | Stdev ≠ pairwise motion; cheap from L1 ring |

**Explicitly out of minimal set:** periodicity/autocorr, HNR, harmonic grids, classification labels, episode rollups, tonality-as-1−flatness.

### 5.3 Optional phase-2 additions (only after device timing)

- Per-frame dominant path length inside the window (`dominant_wander_hz`).
- Spectral crest of mean spectrum (if field data show flatness insufficient).
- Band-limited flux (e.g. 80–1000 vs 1000–7600) using L0-like edges on the FFT bins.

---

## 6. Data contract proposal (not implemented)

### 6.1 Naming collision with `acoustic.classification`

Today:

- Protocol / Hub: `acoustic.classification` with `schema: "l2"`, `source_family`, `confidence`, `hints`.
- Ear firmware: always serializes `classification: null`.
- L1 header comments: “Not classification (L2)”.

**This investigation’s L2 is not classification.** Putting scene features into `classification` would overload a field whose meaning is already “source hypothesis” and would pressure firmware toward labels (forbidden here).

### 6.2 Additive JSON placement (protocol v1–compatible strategy)

Recommended shape: new **nullable sibling**, same pattern as `signature`:

```text
acoustic.measurements   → schema "l0"   (required)
acoustic.signature      → schema "l1" | null
acoustic.dynamics       → schema "l2" | null   ← NEW scene-feature object
acoustic.classification → schema "l2_class" | null  ← keep null; rename schema later if needed
```

**Consumer compatibility**

| Consumer | Effect |
| --- | --- |
| Ear builds that omit `dynamics` | Unchanged |
| Hub with `extra=forbid` today | **Must** add optional `dynamics` field before accepting payloads that include it (additive Hub change; `protocol_version` stays **1**) |
| Readers that only use L0/L1 | Ignore `dynamics` when absent/null; no L0/L1 field renames |
| Existing `classification: null` | Remains valid; no labels required |

Alternative (rejected for v1 features): reuse `classification` with a different inner schema—**breaks** the documented meaning of `source_family` / simulator hints.

Until Hub accepts `dynamics`, firmware must keep emitting without it (or only behind a flag that matches Hub deploy). **This document does not change schemas.**

### 6.3 Candidate `acoustic.dynamics` object

Independently versioned; units explicit; nulls for invalid; no class labels.

```json
{
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
  "spectral_flux": { "mean": 0.042, "stdev": 0.011 },
  "envelope_cv": { "mean": 0.18, "stdev": 0.05 },
  "dominant_delta_hz": { "mean": 31.2, "stdev": null }
}
```

| Field | Type | Unit / range | Validity |
| --- | --- | --- | --- |
| `schema` | `"l2"` | — | Always when object present |
| `version` | int ≥ 1 | semantics package | Always |
| `sample_rate_hz` / `analysis_window_ms` / `fft_size` | int | mirror L1 | Always |
| `freq_min_hz` / `freq_max_hz` | float | Hz, analysis band | Always |
| `frame_hop_samples` | int | samples (1024) | Documents flux/envelope alignment |
| `window_count` | int | valid unclipped contributors | Always |
| `invalid_window_count` | int | associated rejected | Always |
| `spectral_flux.mean` | float \| null | dimensionless ≥ 0 (energy-normalized) | null if `window_count=0` |
| `spectral_flux.stdev` | float \| null | same | null if `window_count<2` |
| `envelope_cv.mean` | float \| null | dimensionless ≥ 0 | null if insufficient |
| `envelope_cv.stdev` | float \| null | same | null if `window_count<2` |
| `dominant_delta_hz.mean` | float \| null | Hz ≥ 0 | null if &lt;1 consecutive pair |
| `dominant_delta_hz.stdev` | float \| null | Hz | null if &lt;2 pairs |

**Determinism:** same association rule as L0/L1; fixed hop; documented normalization; population stdev; no RNG; clip/energy floors aligned with L1 (`−0.05 dBFS`, `1e-12`).

**Non-goals in schema:** `source_family`, confidence labels, embeddings, MFCC, raw spectra.

---

## 7. Testing strategy (host-side, pre-implementation)

Mirror `test_acoustic_l1.c`: 16 kHz, 24-bit FS int32, 1 s windows, tolerances as gates not soft targets.

### 7.1 Synthetic signals → expected qualitative behavior

| Signal | spectral_flux | envelope_cv | dominant_delta (multi-window) |
| --- | --- | --- | --- |
| Silence / below energy floor | invalid | invalid | no pairs |
| White / broadband stationary noise | low–moderate stable | low | ~0 (peak may hop → allow wider tol) |
| Pure tone 1 kHz steady | **low** | **low** | ~0 |
| Two steady tones | low | low | ~0 |
| Linear FM chirp across 1 s | **high** | low–moderate | large if chirp spans windows |
| AM tone (e.g. 1 kHz × 5 Hz depth 0.5) | low–moderate | **high** | ~0 |
| Periodic pulse train | moderate–high | **high** | ~0 if spectrum stable |
| Tone + lower-level noise | flux ↑ vs pure tone | slight ↑ | small |
| Hard level step mid-window | flux spike | cv spike | n/a within single window |

### 7.2 Numerical tolerance guidance (initial)

| Metric | Steady tone | AM tone | Chirp |
| --- | --- | --- | --- |
| `spectral_flux` | &lt; ε_tone (calibrate on golden) | ≥ ε_tone | ≫ ε_tone |
| `envelope_cv` | &lt; 0.05 (order-of) | ≫ steady | mid |
| `dominant_hz` pairing | \|Δ\| ≤ 1 bin (15.625 Hz) | ≤ 1–2 bins | large |

Exact ε values should be frozen from the first golden vectors (like L1’s flatness &lt; 0.35 for tone / &gt; 0.4 for noise).

### 7.3 Edge cases

- `count < 2048` → flux undefined (&lt;2 frames); envelope may still be defined for 1 frame → **specify** invalid until ≥2 frames for both F1/F2 consistency.
- Clipped window: excluded from interval means; increments `invalid_window_count`.
- Non-monotonic / gap in `window_end`: do not form `dominant_delta` pair.
- History overflow: follow L0 philosophy—either clear continuity features or set an explicit `history_overflow` (prefer **clear pairwise deltas**, keep per-window flux/cv if hist holds them).
- Interval with 0 windows: schema-valid object with null features (L1 empty pattern).

### 7.4 Integration tests (later phases)

- Payload host test: `dynamics` null when Kconfig off; object when on.
- Hub schema test: accept null; accept object; reject unknown keys / out-of-range.
- Device: extend serial diag with `L2 perf us_last/max`; require overwrite=0 over ≥500 windows.

---

## 8. Risks and limitations

1. **Naming debt:** “L2” already means classification in Hub/protocol comments—needs conscious rename (`dynamics`) before implementation approval.
2. **Flux normalization:** absolute flux tracks SNR; energy-normalized flux is more comparable but can explode near silence → hard energy floor required.
3. **Dominant hopping in noise:** `dominant_delta` can look “active” for broadband scenes; gate on flatness or tonal confidence proxies without emitting class labels (e.g. only pair when flatness &lt; threshold)—document if used.
4. **Bin resolution 15.625 Hz:** weak for fine harmonic / HNR work; do not oversell tonal detail.
5. **Combined latency:** L0+L1 ≈ 100 ms measured; L2 must not push poll past the real-time margin under MQTT spikes.
6. **SPIRAM PCM latency:** frame loop already touches SPIRAM; extra per-bin flux work increases that pressure—measure, don’t assume host ratios.
7. **No source identity:** scene features help *downstream* correlation but must not be treated as classification on the Ear.
8. **Mic placement / calibration:** envelope CV and flux magnitudes are not absolute outdoor dB metrics; use relative / comparative interpretation.

---

## 9. Recommended implementation phases

| Phase | Work | Exit criteria |
| --- | --- | --- |
| **0 — Contract approval** | Agree `acoustic.dynamics` + feature formulas/ranges; Hub additive schema sketch | Written contract accepted (separate doc, like L0/L1) |
| **1 — Host L2 core** | `dbmap_acoustic_l2.*`: window analyze hooked from shared FFT/envelope path **or** pure functions fed by test doubles; interval accum; golden PCM tests | Host tests green; no device/MQTT yet |
| **2 — Firmware wire-up** | Compute inside `l0_poll` after/with L1; ring + Kconfig `DBMAP_EAR_OBS_L2`; diag timers | Device L2 us_max within budget; overwrite=0 |
| **3 — Serialization + Hub** | Emit `dynamics`; Hub optional field; simulator fixtures | Protocol v1 consumers still accept L0-only paths; extras forbid honored |
| **4 — Field validation** | Compare flux/cv/delta across known scenes on EAR-001 | Confirm features separate scenes L0/L1 stdev cannot; tune floors |
| **5 — Optional expansions** | Band-split flux, dominant wander; still no classifier | Only if Phase 4 shows gaps |

No L0/L1 formula changes; no CPU frequency changes; no classification logic in these phases.

---

## 10. Final recommendation

**Smallest technically meaningful L2 set:**

1. **`spectral_flux`** (intra-window, from L1 FFT frames),  
2. **`envelope_cv`** (intra-window, FFT-frame RMS),  
3. **`dominant_delta_hz`** (inter-window, from existing L1 dominant scalars).

Publish as interval mean/stdev under a new nullable **`acoustic.dynamics`** object with `schema: "l2"`, independently versioned—**not** under `acoustic.classification`, and **without** source labels.

This set:

- adds information L0 (T1–T6) and L1 (mean/stdev of averaged spectrum features) do not provide,
- reuses the L1 FFT path and avoids a second PCM pass,
- keeps memory to roughly +2 KiB workspace plus small ring entries,
- targets roughly **≲ 10 ms** incremental analyze time on S3 (estimate—must be measured),
- preserves Observation START/PERIODIC/END interval semantics.

**Do not implement** until the L2/`dynamics` contract is approved and Hub additive acceptance is planned. Periodicity, HNR, and classification remain explicitly out of scope for this layer.
