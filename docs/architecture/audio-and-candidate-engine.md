# Audio Engine and Candidate Engine — architecture design

**Status:** Proposed (design only — not implemented)  
**Branch intent:** `firmware/audio-candidate-engine-design`  
**ADR:** [0002](../adr/0002-audio-and-candidate-engine.md)  
**Baseline:** `main` after PR #48 (multi-mic Platform acquisition)  
**Reference IDF:** ESP-IDF 5.4.4 (no migration)

This document is the reviewable architecture for the next Ear firmware layer and for how that layer must stay compatible with a reusable, multi-engine Sensor Platform. It is grounded in the **current** codebase. Capabilities are labelled by horizon: **Platform today**, **AE/CE v1 uses**, or **shared Platform later** (also **reuse** / **extend** / **new** where useful).

---

## Concept glossary (do not mix)

| Concept | What it is | What it is not |
| --- | --- | --- |
| **Raw samples** | int32 PCM (or I2S slot words) from Platform capture | Features, signatures, candidates |
| **Audio features** | Scalars/vectors from one analysis window (RMS, bands, FFT scalars, …) | A tracked source identity |
| **Acoustic signature** | Quality-gated, versioned feature package for a window (or short chain), with timing + quality | A classifier label (ABC / drone / …) |
| **Candidate** | On-device tracked hypothesis of a continuing acoustic source/event, accumulating signatures | An Observation; Hub Event/Track |
| **Classification result** | Product meaning from a future module (ABC, Drone-ML, …) | Produced by Audio or Candidate Engine |
| **Observation** | Episodic MQTT report (START/PERIODIC/END) with platform envelope + product schemas | Continuous analysis output |
| **Platform contract** | Versioned, domain-neutral data/API surface produced by Platform (audio is first; sensor/Omnibus later) | Engine-private features, product labels, or an audio-only private bus |
| **Engine** | Independent peer consumer of one or more Platform contracts; owns domain interpretation | Platform acquisition; Hub Observation transport; another Engine’s internals |
| **Module** | Pluggable analyzer/classifier that an Engine or Module Runtime may invoke | Not a second acquisition path; not Platform transport |

---

## 0. Multi-engine Platform architecture (requirement)

### Long-term Platform goal

Sensor Platform must be designed and **progressively developed** as reusable, multi-engine infrastructure. Future Engines and Modules should consume shared generic contracts, transport, lifecycle, and services **without product-specific special paths** wherever reasonably possible.

Audio Engine / Candidate Engine are the **first** Engine pair on that foundation. Their v1 implementation must remain compatible with concurrent non-audio Engines; it must **not** expand into a full Engine Runtime, contract bus, or Module Runtime merely to prove the goal.

This section is an **architectural requirement**. Implementing the shared bus / registry / Engine Runtime is **explicitly deferred**.

### Architectural principles

| # | Principle | Requirement |
| --- | --- | --- |
| 1 | **Multiple independent Engines** | Platform must support **concurrent peer Engines** consuming **different** Platform contracts on one device. Example: Audio Engine Light on the audio contract alongside Monitor Engine on sensor + Omnibus contracts. Peers are not multiple audio-processing instances of one Engine. |
| 2 | **Generic contracts** | Platform contracts must not be *inherently* audio-only as an architecture. Audio is the first concrete contract type. Adding sensor/Omnibus (or other) producers must not require redesigning the Engine integration model. |
| 3 | **Shared transport (direction)** | Long-term on-device data plane must allow **multiple independent consumers/subscribers**. An audio-only transport or single-consumer mechanism must not be treated as the architectural foundation. Existing pull/notify may serve AE/CE v1 if interfaces can evolve into shared generic transport. |
| 4 | **Shared lifecycle and services** | Platform owns generic Engine/Module lifecycle foundations, health contribution, time/clock rules, config hooks, and (later) resource policy. Domain thresholds, features, and interpretation stay in Engines/Modules. |
| 5 | **Clear ownership** | **Platform:** generic acquisition, contracts, shared transport mechanisms, common services. **Engines:** domain processing and derived data. **Candidate Engine** in *this* design remains **audio-signature-specific** unless a separate justified generalization is proposed. Do not move product-specific processing into Platform. |

**Illustrative product mix (not implemented here):**

| Engine | Consumes | Owns (domain) |
| --- | --- | --- |
| **Audio Engine** (full or “Light”) | Platform **audio** contract | Acoustic features, signatures, history |
| **Candidate Engine** | AE signature updates (engine-to-engine) | Multi-candidate identity / lifecycle over acoustic signatures |
| **Monitor Engine** (future) | Platform **sensor** + **Omnibus** contracts | Monitoring interpretation / thresholds |

### 0.1 Target shape

```mermaid
flowchart TB
  subgraph platform [Sensor Platform - progressive shared foundation]
    ACQ[Generic acquisition producers]
    CTR[Versioned contracts: audio then sensor omnibus …]
    BUS[Shared notify or pull - multi-subscriber direction]
    LIFE[Lifecycle config time health resource foundations]
    ACQ --> CTR
    CTR --> BUS
    LIFE --> BUS
  end
  subgraph engines [Independent peer Engines]
    AEL[Audio Engine Light]
    MON[Monitor Engine future]
    CE[Candidate Engine audio-signature v1]
  end
  subgraph later [Later shared Platform capabilities]
    ER[Engine Runtime optional]
    MR[Module Runtime]
  end

  BUS --> AEL
  BUS --> MON
  AEL --> CE
  ER -.-> AEL
  ER -.-> MON
  MR -.-> CE
```

### 0.2 Three horizons (do not conflate)

| Horizon | Meaning | In this document |
| --- | --- | --- |
| **Platform today** | What the codebase already provides | §0.3 |
| **AE/CE v1 uses** | What Audio/Candidate Engine may rely on without building the full shared bus | §0.4 |
| **Shared Platform later** | Capabilities Platform must grow for reusable multi-engine products | §0.5 — design now, implement later |

### 0.3 Platform today (what already exists)

| Capability | Today | Notes |
| --- | --- | --- |
| Audio acquisition + channel metrics | `firmware/platform/mic` (`sp_mic_*`) | Concrete **audio contract seed** (pull APIs + health JSON) — not yet a versioned multi-contract catalog |
| Analysis PCM hand-off | `sp_mic_pcm.h` (`sp_mic_take_pcm_job`) | **Single-consumer** job queue — mic0 only; useful pattern, **not** the long-term multi-subscriber foundation |
| Health enricher table | `sp_health_add_enricher` / `sp_health_set_enricher` | Multi-component Hub Reported State — **not** the on-device Engine data plane |
| Device lifecycle / config / time | `node_common` (NVS, Desired, NTP/`sp_time`, OTA, MQTT) | Shared device services; not yet an Engine registry |
| Observation foundation | `platform/observation` | Episode/seq/gates — **Hub reporting**, not Engine contract transport |
| FreeRTOS / ESP-IDF events | Wi-Fi uses `esp_event`; mic uses task + pull | No unified Platform data-plane bus yet |

**Absent today:** general Engine registry, cross-contract multi-subscriber bus, named contract IDs beyond headers, Omnibus/sensor contract producers, Monitor Engine, Module Runtime, Engine Runtime, shared resource policy.

### 0.4 What AE/CE v1 will use (allowed without expanding Platform scope)

| AE/CE v1 may | Must not treat as final Platform architecture |
| --- | --- |
| Pull `sp_mic_*` / `sp_mic_take_pcm_job` for audio windows | Single-taker PCM queue as *the* multi-Engine transport |
| Prefer **listener list or small queue** for AE→CE signatures (engine-local) | An AE-owned global “audio bus” as Platform’s only data plane |
| Reuse health enrichers for optional AE/CE diag fields | Health MQTT as Engine-to-Engine delivery |
| Boot/start from product `app_main` / obs loop | Codify Ear-only init order as Platform Engine lifecycle |
| Run AE poll inside `ear_obs` initially (§10) | Assume AE is the only consumer of Platform CPU after mic |

**v1 scope rule:** do not implement contract catalog, multi-subscriber Platform bus, Engine registry, Module Runtime, or Monitor Engine in AE/CE v1. Do keep interfaces **migratable** (§0.6).

### 0.5 Shared Platform capabilities — design later, develop progressively

Recommended **future** Platform work (separate ADRs when scheduled). These are **Platform responsibilities**, not Audio Engine features:

| Later capability | Purpose |
| --- | --- |
| **Contract catalog** | Stable `contract_id` + version; payloads for audio window, sensor sample, Omnibus status/event, … New contract types plug in without new Engine wiring models. |
| **Shared data-plane notify/pull** | On-device multi-subscriber delivery (not MQTT). Producers publish small descriptors; consumers pull payloads. Evolves today’s PCM job **without** locking in single-taker semantics. |
| **Engine descriptor + lifecycle** | Register name, contracts consumed/produced, start/stop, deps, task handle; generic health string for enrichers. |
| **Module lifecycle hooks** | Load/enable analyzers against Engine-produced evidence without touching acquisition. |
| **Shared clock / timestamp rules** | Formalize mono_us + producer sequence (`window_seq` today) as contract-time fields for all producers. |
| **Resource policy** | CPU/RAM/queue budgets so concurrent Engines cannot starve acquisition (e.g. I2S). |

**Out of scope for AE/CE v1 implementation.** **In scope for this architecture:** AE↔Platform and AE↔CE shapes that can migrate onto the above without redesign.

### 0.6 Compatibility rules (AE/CE must not block generalization)

| Rule | Implication |
| --- | --- |
| AE consumes **Platform audio APIs**, not a private I2S path | Peer Engines remain free to consume other contracts |
| Do not put AE/CE/L0–L2 **product processing** inside `firmware/platform/mic` | Platform stays free of acoustic product logic |
| AE→CE is an **engine-to-engine** signature API | Not a substitute for the future Platform multi-contract bus; CE must not speak I2S |
| Prefer pull + small multi-listener notify over single global “audio-only” queues owned as Platform | Same pattern generalizes to sensor/Omnibus later |
| Do not hard-wire Engine integration to “one PCM taker” forever | If v1 uses `sp_mic_take_pcm_job`, wrap behind AE intake so Platform can later fan-out windows |
| Concurrent Engines | AE must tolerate other tasks; must not assume sole post-mic CPU owner |
| “Audio Engine Light” | Same audio contract, fewer features — still an Engine, not a Platform fork |
| Monitor Engine | Independent peer; shares Platform lifecycle/time/health/transport foundations only |
| Candidate Engine scope | **Audio-signature-specific in this design**; generalizing CE needs a **separate justified proposal** |

### 0.7 Ownership map

| Layer | Owns | Must not own |
| --- | --- | --- |
| **Platform** | Generic acquisition, contract producers, shared transport mechanisms (today’s seeds + later bus), common lifecycle/config/time/health foundations, Observation *foundation* APIs | Acoustic analysis, candidate FSM, monitoring thresholds, classifiers, product Observation policy |
| **Audio Engine** | Features, signatures, signature history, quality flags | I2S/DMA; Candidates; Hub Observations; non-audio contracts |
| **Candidate Engine** | Multi-candidate identity/lifecycle over **acoustic signatures** | Platform acquisition; MQTT Observation system; product classification |
| **Future Monitor Engine** | Sensor/Omnibus interpretation | Audio acquisition; AE internals |
| **Reporting / Observation product path** | Episodic Hub reports from promotions | Continuous Engine tick fan-out |

### 0.8 How AE/CE map onto the general model

| Component | Role |
| --- | --- |
| Platform `mic` | **Producer** of the first concrete Platform contract (audio) |
| Audio Engine | **Engine** consuming audio contract; **producer** of signature updates (engine-local) |
| Candidate Engine | **Engine** consuming signatures; **producer** of candidate state for promoters / future Module Runtime |
| Monitor Engine (future) | **Peer Engine** on sensor/Omnibus — parallel to AE, never behind AE |
| Observation / Reporting | Downstream of promotion — not the Platform Engine data plane |

### 0.9 Design choices that could constrain generalization (explicit)

These are accepted for AE/CE v1 **only if** wrapped so Platform can evolve:

| Choice | Constraint risk | Mitigations required by this design |
| --- | --- | --- |
| `sp_mic_take_pcm_job` single taker | Blocks a second on-device audio consumer | AE is the sole v1 taker; Platform later adds multi-subscriber window notify without AE rewriting I2S |
| AE→CE direct callback/queue | Looks like “the” bus if copied for every Engine | Keep AE→CE **engine-local**; Platform contracts stay pull/notify at Platform boundary |
| Engines under `products/dbmap/ear/` | Suggests product-private Platform | Obey contract rules; relocate under shared Engine tree only after Engine Runtime ADR |
| Health enrichers as multi-component hook | Easy to misuse as data plane | Enrichers = Hub telemetry only |
| Polling AE inside `ear_obs` | Couples Engine lifecycle to Observation task | Temporary; dedicated `ae_task` / registry start later |
| Audio-first contract APIs | Temptation to name Platform APIs `audio_*` globally | Prefer contract-neutral names when introducing catalog/bus (`sp_contract_*`) |

---

## 1. Current source-code assessment

### 1.1 What exists today

```mermaid
flowchart LR
  subgraph platform [Sensor Platform]
    I2S[I2S RX DIN0/DIN1]
    MW[measure_window ~1s]
    Health[health audio.microphones]
    PCM[SPIRAM PCM mic0 only]
  end
  subgraph product [dBmap product]
    L0L2[ear_acoustic L0/L1/L2]
    Act[activity_detector single stream]
    Obs[ear_observation MQTT]
  end
  I2S --> MW
  MW --> Health
  MW --> PCM
  PCM --> L0L2
  MW -->|sp_mic_get_levels mic0| Act
  L0L2 --> Obs
  Act --> Obs
```

| Layer | Location | Role today |
| --- | --- | --- |
| Mic acquisition | `firmware/platform/mic/ear_mic.c` | Dual STD RX; 1–4 live channels; per-ch RMS/peak/status; bg hist; health enrich |
| Mic public API | `sp_mic.h`, `sp_mic_pcm.h`, `sp_mic_channel_api.h` | `sp_mic_start`, `get_levels` (**mic0**), `get_channel`, `get_stream`, PCM job take |
| Count policy | `sp_mic_count.h` | `SP_MIC_COUNT_MAX=4`, `SP_MIC_ACQUIRE_MAX=4` |
| L0/L1/L2 math | `firmware/products/dbmap/ear/ear_acoustic/` | Host-testable libs + `dbmap_ear_mic_l0_poll` on mic0 PCM |
| Activity | `dbmap_activity_detector.*` | One aggregate RMS activity stream → START/PERIODIC/END |
| Observation | `ear_observation.c` | Episode registry + MQTT publish; `mic_index` hard-coded **0** |
| Platform obs foundation | `firmware/platform/observation/` | Sequence, episode registry, emit gate |

There is **no** Audio Engine or Candidate Engine component. The name “candidate” elsewhere means Rel END baseline samples, Hub firmware catalog candidates, or offline localization script candidates — different concepts.

### 1.2 Timing baseline (measured / configured facts)

| Item | Value | Source |
| --- | --- | --- |
| Capture / analysis window | Default **1000 ms** | `CONFIG_DBMAP_EAR_MIC_WINDOW_MS` |
| Measure wall time | ≈ window length (~1.00–1.01 s) | USB `measure_us` / health `measure_window_us` |
| Mic task | `"sp_mic"`, stack **6144**, delay 20 ms between windows | `ear_mic.c` |
| Observation interval | Default **5000 ms** | `CONFIG_DBMAP_EAR_OBS_INTERVAL_MS` |
| Obs task | `"ear_obs"`, stack **6144**, poll ~100 ms | `ear_observation.c` |
| L0 analyze (device logs) | typically **~55–90 ms** last/max on 1 s PCM | `ear_obs` L0 perf lines |
| L1+L2 fused | typically **~90–120 ms** combined | same |
| L1 workspace | static ~**22.5 KiB** | `dbmap_l1_workspace_t` |
| PCM | 2× SPIRAM buffers ≈ **129 KiB** for 16 kHz × 1 s | boot log `pcm_bytes` |
| CPU % | **Not measured** | Do not invent |

### 1.3 Stereo / multi-mic facts (do not over-claim)

- **DIN0 stereo (Mic 0 L / Mic 1 R):** same I2S controller, same `measure_window`, shared `window_seq` and `measured_at_us`. Samples are interleaved L/R per IDF Philips contract. This is **window-aligned stereo** suitable for energy comparison. Sample-clock identity within the pair follows one RX DMA stream — treat as **implementation-aligned on one DIN**, not independently verified with a scope on every board.
- **DIN1 (Mic 2/3):** second controller, slave clocks. Sample counts can differ slightly from DIN0 in the same wall window. **Not** a verified TDOA path.
- **Analysis PCM:** Platform retains **mic0 / DIN0 left only** (`accumulate_din_slots`). Product L0–L2 therefore see a mono scene today.
- Physical lab posture for this design: **continue with two mics (0/1)**. Four-channel acquisition must not block Audio Engine v1.

### 1.4 Known coupling / gaps

| Gap | Implication for design |
| --- | --- |
| Product paths mic0-centric | Audio Engine v1 should explicitly consume Mic0+Mic1 metrics; stereo PCM is an **extension** of Platform |
| Activity = single stream | Candidate Engine must not be a rename of `dbmap_activity_detector`; migrate carefully |
| Observation = episodic report | Candidate Engine must not publish MQTT Observations |
| Health JSON size | Multi-mic health already pressured Mosquitto packet size; AE/CE must not dump PCM on MQTT |

---

## 2. Proposed component responsibilities

### 2.1 Sensor Platform (ownership — progressive multi-engine foundation)

**Owns today (audio contract producer + shared services):** microphone hardware lifecycle, I2S/DMA, windowed acquisition, per-channel measurements/diagnostics, health `audio.*`, analysis-PCM hand-off API, device lifecycle/config/time/health foundations (`node_common`), Observation *foundation* APIs.

**Owns as Platform grows (later, not AE/CE v1):** versioned contract catalog, shared multi-subscriber on-device transport, Engine/Module lifecycle hooks, resource policy (§0.5).

**Must not:** know acoustic events, candidates, monitoring thresholds, classifiers, or product Observation policy. Domain processing stays in Engines/Modules.

Audio Engine **consumes** Platform via `sp_mic_*` / `sp_mic_pcm_*` (v1). It must not open I2S or duplicate `measure_window`. A future Monitor Engine consumes **other** Platform contracts the same way — peer Engines, not forks of AE.

### 2.2 Audio Engine (new — application / audio-processing)

**Owns:**

- Pulling completed Platform windows (PCM + channel snapshots).
- Windowing policy for analysis (default: align to Platform 1 s windows).
- Feature extraction (reuse L0/L1/L2 libs; add stereo pair features where justified).
- Building **acoustic signatures** and a bounded **signature history**.
- Data-quality / confidence flags for each signature.
- Publishing signature updates to Candidate Engine (small structs — no PCM copies).

**Must not:**

- Own I2S/DMA.
- Maintain Candidates.
- Emit Observations or Hub MQTT product reports.
- Apply ABC / Drone-ML / VMA labels.

### 2.3 Candidate Engine (new — application)

**Owns:**

- Multiple simultaneous Candidates.
- Create / update / expire / reject / promote (lifecycle).
- Evidence accumulation from **acoustic signatures** (this design — audio-signature-specific).
- Candidate identity and continuity heuristics (lightweight).
- Temporary loss (“coast”) and recovery.

**Must not:**

- Become a second Observation/reporting system.
- Own audio buffers or Platform acquisition.
- Make final product classification decisions (those belong to future modules / Decision).
- Be treated as a Platform generic “any-evidence” runtime in v1 — that requires a separate justified generalization proposal (§0 principle 5).

### 2.4 Existing product pieces after introduction

| Existing | Role going forward |
| --- | --- |
| `ear_acoustic` L0/L1/L2 **libs** | Reused as Audio Engine’s feature backends (preferred) or called from AE wrappers |
| `dbmap_ear_mic_l0_poll` glue | Eventually absorbed into Audio Engine runtime task/API |
| `dbmap_activity_detector` | Interim promotion/gating path; later a **Workflow/Decision** consumer of Candidates, or replaced by a thin promoter |
| `ear_observation` | Remains Observation publisher; should eventually bind to **promoted** Candidates / track ids, not raw AE spam |

---

## 3. Proposed source placement

Recommended tree (names indicative — not created in this task):

```text
firmware/platform/
  mic/                    # audio contract producer (unchanged ownership)
  observation/            # Observation foundation (reporting), not Engine data plane
  node_common/            # lifecycle, config, time, health enrichers, MQTT
  # later (not now): contract bus / engine registry — shared Platform capability

firmware/products/dbmap/ear/
  ear_acoustic/           # L0/L1/L2 math libs (reusable by Audio Engine)
  audio_engine/           # NEW: Audio Engine (full or Light)
  candidate_engine/       # NEW: Candidate Engine (v1 evidence = signatures)
  ear_observation/        # episodic MQTT; later consume promotions

# future industrial product (illustrative)
# firmware/products/<monitor>/monitor_engine/  # consumes sensor/omnibus contracts
```

**Why Engines stay outside Platform acquisition:** Platform must remain free of product interpretation in drivers/acquisition (§0 ownership). Audio Engine and Monitor Engine are peer Engines on shared Platform foundations; only **contracts and shared services** live in Platform.

**Why not a Platform-only “audio runtime”:** an audio-specific runtime/bus would force Monitor Engine into a second private path and violate the multi-engine Platform goal (§0).

**Progressive Platform tree (later, not now):** contract catalog / shared notify-pull / Engine registry belong under `firmware/platform/` as generic infrastructure — not under `audio_engine/`.

**Alternative considered:** place engines under `firmware/platform/engines/`. Deferred until a second Engine exists and a shared Engine Runtime ADR is accepted; v1 keeps AE/CE under the dBmap Ear product tree while obeying Platform contract and transport-evolution rules (§0.6, §0.9).

---

## 4. Audio data-flow design

### 4.1 End-to-end flow

```mermaid
flowchart TB
  subgraph plat [Platform mic]
    RX[I2S RX Mic0/Mic1]
    WIN[measure_window]
    CH[sp_mic_get_channel 0..1]
    PCM[sp_mic_take_pcm_job mic0]
    HL[health audio]
  end
  subgraph ae [Audio Engine]
    IN[Window intake]
    FE[Feature extract]
    SIG[Signature build]
    HIST[Signature history ring]
    OUT[Signature update pub]
  end
  subgraph ce [Candidate Engine]
    ASSOC[Associate / create]
    EV[Evidence update]
    LC[Lifecycle FSM]
  end
  subgraph later [Later - not this design's implementables]
    MOD[Module Runtime]
    WF[Workflow]
    REP[Reporting / Observation]
  end

  RX --> WIN
  WIN --> CH
  WIN --> PCM
  WIN --> HL
  CH --> IN
  PCM --> IN
  IN --> FE
  FE --> SIG
  SIG --> HIST
  SIG --> OUT
  OUT --> ASSOC
  ASSOC --> EV
  EV --> LC
  LC -.-> MOD
  LC -.-> WF
  LC -.-> REP
```

### 4.2 Stage contracts

| Stage | Input | Output | Owner | Timing | Buffering | Validity |
| --- | --- | --- | --- | --- | --- | --- |
| Acquisition | I2S DMA | Per-ch snapshot + optional PCM | Platform | Continuous RX; ~1 s windows | DMA + slot scratch; PCM double-buffer | transport/mic status, sample counts |
| Window intake | `sp_mic_get_stream` + channels + PCM job | `ae_window_view_t` | Audio Engine | Once per completed Platform window | No long PCM retain beyond analyze | Drop if transport error / missing PCM when required |
| Feature extract | PCM (+ ch metrics) | `ae_features_t` | Audio Engine | Same cadence as windows (~1 Hz) | L0 filters + L1 workspace (existing static) | Feature-level validity flags |
| Signature | Features + quality | `ae_signature_t` | Audio Engine | Per accepted window | None beyond hist | Omit/publish invalid |
| Signature history | Signatures | Ring query API | Audio Engine | Append ~1 Hz | Fixed capacity (see §6) | Aged drop |
| CE ingest | Signature updates | Candidate mutations | Candidate Engine | Event-driven on AE publish | Small queue of updates | Ignore invalid; coast on gaps |
| Observation | Promoted candidate / detector | MQTT JSON | `ear_observation` | Episodic / 5 s periodic | Existing rings | Unchanged contract |

### 4.3 Stereo and timing (explicit)

**Association of Mic0 and Mic1**

- Prefer **same `window_seq`** and shared Platform `measured_at_us` from one `measure_window` on DIN0 stereo.
- Timestamp for AE signature: **`window_end_mono_us`** from Platform PCM job when PCM used; else channel snapshot `measured_at_us`. Prefer acquisition-completed time, not “analysis finished” time, so CE timing tracks the acoustic world.
- Analysis duration is recorded separately (`analyze_us`) for diagnostics only.

**Alignment claim level**

- **Claim:** Mic0/Mic1 from DIN0 share one RX stream and one measure deadline → **window-coherent stereo pair**.
- **Do not claim:** verified inter-sample phase for TDOA, or DIN0↔DIN1 coherence.
- **Verification method (future):** capture_diag / host compare of `window_seq`, sample counts, and optional bench tone — not assumed done.

**Dropped / incomplete windows**

- Incomplete stereo trailing words already counted by Platform (`incomplete_stereo_frames`).
- AE: if `sp_mic_take_pcm_job` overwritten (`pending_overwrite`) → emit signature with `quality=stale_or_gap` or skip CE update for that seq; never invent samples.
- If mic1 `disconnected_suspected` while mic0 `signal_present` → stereo features marked degraded; mono primary path may continue.

**Continuity**

- Monotonic `window_seq` from Platform is the primary continuity key.
- AE detects gaps (`seq != last+1`) and flags them on the next signature.

### 4.4 Analysis windows

**Recommendation:** keep **~1 s** Platform-aligned windows as Audio Engine v1 cadence.

| Rationale | Detail |
| --- | --- |
| Matches existing PCM, L0–L2, and device timing budget | L0+L1+L2 already fit in ≪ 1 s wall on S3 (measured analyze_us) |
| Matches human-scale acoustic persistence | Stable enough for energy/spectral tracking without 10× CPU |
| Independent of Hub | Observation interval (5 s) aggregates; AE runs whether MQTT is up |

**Shorter internal windows:** optional later (e.g. 250 ms hop for flux) *inside* AE using retained PCM — **not** required for v1; would need Platform multi-job PCM or AE-owned copy (avoid).

**Overlap:** not for v1 (extra CPU, complex hist). Non-overlapping 1 s windows are enough for Candidate coast/recovery.

---

## 5. Feature and acoustic-signature design

### 5.1 Features for Audio Engine v1

Reuse first; add only what Candidate continuity needs.

| Feature | Source | Purpose | Cost | CE relevance |
| --- | --- | --- | --- | --- |
| RMS / peak / crest | L0 / channel metrics | Energy + clip | Low (existing) | Enter/exit, coast |
| 4-band energies + H/L | L0 | Coarse spectrum shape | Medium (biquads on PCM) | Similarity / change |
| Centroid, bandwidth, dominant Hz, flatness | L1 | Spectral identity cues | High (~FFT 1024) | Continuity / split |
| Spectral flux, envelope CV, dominant Δ | L2 | Change / dynamics | Medium–high (fused path) | Activity vs stable noise |
| Mic0 vs Mic1 ΔRMS / Δpeak | **New** from `sp_mic_get_channel` | Stereo asymmetry without TDOA | Very low | Multi-mic evidence; not DOA |
| Channel quality bits | Platform statuses | Gate learning | None | Reject bad evidence |

**Deferred (not v1):** full stereo FFT, cross-correlation / TDOA, beamforming, ML embeddings, DIN2/3 features as required inputs.

### 5.2 Acoustic signature definition

An **acoustic signature** is a **versioned, quality-gated package of features for one analysis window**, plus timing and continuity metadata. It describes *what the sensors heard in that window*, not *what object it is*.

```c
/* Conceptual — illustrative, not implemented */
typedef struct {
    uint32_t schema_version;       /* ae_signature schema */
    uint32_t window_seq;           /* from Platform */
    int64_t  window_end_mono_us;   /* acquisition-tied */
    uint32_t window_ms;
    uint8_t  mic_mask;             /* bit0=mic0, bit1=mic1, … */
    uint8_t  quality;              /* enum: ok / degraded / gap / invalid */
    uint8_t  flags;                /* clipped, stereo_degraded, … */

    /* Primary scene (typically mic0 PCM-derived) */
    float rms_dbfs, peak_dbfs, crest_db;
    float band_dbfs[4];
    float hl_db; bool hl_valid;
    float centroid_hz, bandwidth_hz, dominant_hz, flatness;
    float spectral_flux, envelope_cv;
    float dominant_delta_hz; bool dominant_delta_valid;

    /* Pair features (mic0/mic1) — optional if mic1 absent */
    float delta_rms_db;            /* mic0 - mic1 */
    float delta_peak_db;
    bool  pair_valid;
} ae_signature_t;
```

**Creation / update:** one new signature per accepted window (immutable snapshot). “Update” means append history + notify CE, not mutate an old signature in place.

**Similarity:** CE may compare consecutive signatures (RMS Δ, band cosine, dominant proximity). AE may expose a helper `ae_signature_distance()` later; v1 can keep distance logic inside CE to avoid circular deps.

**Uncertainty:** encode in `quality` + feature-level validity (e.g. `hl_valid`), not by inventing confidence from classifiers.

---

## 6. Signature-history design

| Property | Recommendation |
| --- | --- |
| Structure | Fixed ring of `ae_signature_t` (by value) |
| Capacity | **32–64** entries (~32–64 s at 1 Hz) — enough for Candidate association and short coast |
| RAM | ~128–256 bytes/signature estimate → **~8–16 KiB** static (refine at impl) |
| Query | Latest N; range by `window_seq` / mono_us |
| Ownership | Audio Engine only; CE receives copies/notifications, does not share mutable ring |
| Persistence | RAM only; no NVS |

---

## 7. Candidate data model

```c
/* Conceptual */
typedef enum {
    AE_CAND_NASCENT = 0,
    AE_CAND_ACTIVE,
    AE_CAND_COASTING,
    AE_CAND_PROMOTED,   /* handed to Observation/Workflow interest */
    AE_CAND_REJECTED,
    AE_CAND_EXPIRED,
} ae_cand_state_t;

typedef struct {
    uint64_t candidate_id;         /* device-local monotonic or UUID-derived */
    ae_cand_state_t state;
    int64_t created_mono_us;
    int64_t last_evidence_mono_us;
    int64_t last_update_mono_us;
    uint32_t evidence_count;
    uint32_t coast_windows;
    float confidence;              /* 0..1 heuristic — not classifier posterior */
    uint8_t quality_summary;
    /* Compact rolling evidence: last signature id/seq, EMA rms, band fingerprint */
    uint32_t last_window_seq;
    float ema_rms_dbfs;
    float band_ema[4];
    uint8_t mic_mask_seen;
} ae_candidate_t;
```

**Capacity:** start with **4–8** simultaneous candidates (static array). Excess → reject weakest / oldest nascent.

**Candidate ≠ Observation:** Observations remain episodic MQTT reports with `observation_id`. A Candidate may later *inspire* an Observation episode (promotion), optionally carrying `track_id` continuity — that binding is Workflow/Reporting work.

---

## 8. Candidate lifecycle and state transitions

```mermaid
stateDiagram-v2
  [*] --> NASCENT: new signature unmatched
  NASCENT --> ACTIVE: evidence_count >= N_enter AND quality ok
  NASCENT --> REJECTED: timeout OR poor quality streak
  ACTIVE --> ACTIVE: matching signature update
  ACTIVE --> COASTING: missing/invalid windows < T_coast
  COASTING --> ACTIVE: matching signature returns
  COASTING --> EXPIRED: coast exceeded (EXIT if promotion_entered)
  ACTIVE --> PROMOTED: promoter / workflow gate
  PROMOTED --> COASTING: missing/invalid windows < T_coast
  PROMOTED --> ACTIVE: still tracked locally optional
  ACTIVE --> REJECTED: explicit reject policy
  REJECTED --> [*]
  EXPIRED --> [*]
```

| Transition | Condition (v1 heuristic — tunable, not ML) |
| --- | --- |
| Create NASCENT | Signature `quality∈{ok,degraded}`, not associated to existing candidate (distance above threshold) |
| NASCENT→ACTIVE | ≥ `N_enter` consecutive matching windows (e.g. 2–3) with usable energy |
| Update ACTIVE | Signature distance below match threshold to candidate fingerprint |
| ACTIVE→COASTING | Gap flag, invalid window, or mic quality loss while candidate young evidence remains |
| COASTING→ACTIVE | Match returns within `T_coast` windows (e.g. 3–5 s) |
| →EXPIRED | Coast timeout or max age without promotion interest |
| →PROMOTED | External promoter (initially: adapt activity detector thresholds on candidate energy; later Workflow) |
| →REJECTED | Persistent noise fingerprint / policy blacklist (optional; keep minimal in v1) |

**Duplicate avoidance:** association before create; one signature updates at most one candidate per window (best match). Unmatched → new NASCENT (subject to capacity).

**Multiple simultaneous:** separate fingerprints (band EMA + EMA RMS + optional delta_rms). Do not force a single global activity bit inside CE.

**Classification vs lifecycle:** lifecycle answers “is there a continuing acoustic thing?” Classification answers “what is it?” — future modules only.

---

## 9. Audio Engine ↔ Candidate Engine interface

### 9.1 Principles

- AE publishes **signatures** (and optionally raw feature blocks for debug builds), not PCM.
- Timestamps are acquisition-tied (`window_end_mono_us`, `window_seq`).
- Quality is explicit on every update.
- CE is pull-or-queue consumer; never blocks I2S.
- No shared mutable PCM; no large copies.
- AE→CE is an **engine-to-engine** contract, not a replacement for the future Platform multi-contract bus (§0). When a shared bus exists, signature updates may be *optionally* mirrored as an “engine-produced” contract without requiring CE to speak I2S.
- Prefer `ae_add_signature_listener` (multi-listener) over a single hard-wired CE call site so a future Module or promoter can subscribe without redesign.
- Platform intake for AE remains behind AE wrappers so `sp_mic_take_pcm_job` can later become one of several subscribers to a shared window notify.

### 9.2 Proposed C API (conceptual)

```c
/* audio_engine → world */
esp_err_t ae_start(const ae_config_t *cfg);
esp_err_t ae_poll(void);   /* take Platform window, analyze, append hist, notify */

bool ae_get_latest_signature(ae_signature_t *out);
uint32_t ae_history_copy(ae_signature_t *out, uint32_t max_n);

typedef void (*ae_signature_listener_t)(const ae_signature_t *sig, void *ctx);
esp_err_t ae_add_signature_listener(ae_signature_listener_t fn, void *ctx);

/* candidate_engine */
esp_err_t ce_start(const ce_config_t *cfg);
void ce_on_signature(const ae_signature_t *sig);  /* listener or queue drain */
uint8_t ce_candidate_count(void);
bool ce_get_candidate(uint8_t index, ae_candidate_t *out);
bool ce_find_candidate(uint64_t id, ae_candidate_t *out);

/* Promotion hook — Observation/Workflow later */
typedef void (*ce_promotion_cb_t)(const ae_candidate_t *c, ce_promotion_event_t ev, void *ctx);
```

**Duplicate / repeated updates:** keyed by `window_seq`. CE ignores seq ≤ last processed for that candidate stream; AE never rewinds seq.

**Missing windows:** AE may call `ce_on_gap(last_seq, new_seq)` or encode `quality=gap` on the next signature; CE enters COASTING rather than creating duplicates.

### 9.3 Interface diagram

```mermaid
sequenceDiagram
  participant P as Platform mic
  participant AE as Audio Engine
  participant CE as Candidate Engine
  participant Obs as Observation later

  P->>AE: PCM job + channel snapshots window_seq=N
  AE->>AE: features + signature
  AE->>AE: history push
  AE->>CE: ae_signature_t by value
  CE->>CE: match / create / update FSM
  Note over Obs: only on promotion / existing detector bridge
  CE-->>Obs: promotion callback optional
```

---

## 10. Timing, concurrency, and performance

### 10.1 Task model (recommended)

| Task | Priority (relative) | Work |
| --- | --- | --- |
| `sp_mic` | Keep high enough for I2S reads | Unchanged acquisition |
| `ae_task` **or** poll from `ear_obs` initially | Below mic, above idle | `ae_poll` + `ce_on_signature` |
| `ear_obs` | As today | Promotion → Observation; MQTT |

**v1 pragmatic option:** run `ae_poll` + CE inside existing `ear_obs` activity loop (already calls `dbmap_ear_mic_l0_poll`) to avoid another stack until proven. **v1.1:** dedicated `ae_task` if obs latency suffers.

### 10.2 Resource estimates (labelled)

| Resource | Estimate | Basis |
| --- | --- | --- |
| CPU | ~same as today’s L0+L1+L2 per second + small CE | Measured analyze_us ≪ 1 s; CE O(candidates×features) tiny |
| RAM signatures | ~8–16 KiB | §6 |
| RAM candidates | < 2 KiB for 8 candidates | struct size estimate |
| Extra PCM | **0** if reuse Platform mic0 PCM | Stereo PCM extend = +1× window if added later |
| Stack | Prefer no large FFT on task stack (L1 already static ws) | Existing pattern |
| Queue | 2–4 signature updates | Overflow: drop oldest update, set CE gap |

**Must not** block `i2s_channel_read` loop or take Platform locks longer than today’s critical sections.

### 10.3 Worst case

- Analysis overrun → Platform `pending_overwrite`; AE must detect and gap-flag (existing overwrite counter).
- MQTT stalls must not stall AE/CE (already true for detection vs publish in Ear).

---

## 11. Future integration points

```mermaid
flowchart TB
  subgraph platform [Platform contracts]
    AUD[Audio contract]
    SEN[Sensor / Omnibus future]
  end
  AE[Audio Engine]
  AEL[Audio Engine Light]
  MON[Monitor Engine future]
  CE[Candidate Engine]
  ER[Engine Runtime future]
  MR[Module Runtime future]
  WF[Workflow Engine future]
  DF[Decision / Fusion future]
  CL[ABC / Drone-ML future]
  SP[Spatial / TDOA future]
  RE[Reporting / Observation]

  AUD --> AE
  AUD --> AEL
  SEN --> MON
  AE --> CE
  AEL --> CE
  ER -.-> AE
  ER -.-> AEL
  ER -.-> MON
  CE --> MR
  WF --> MR
  MR --> CL
  MR --> SP
  CL --> DF
  SP --> DF
  DF --> RE
  CE -->|promotion bridge| RE
```

These future pieces are **Platform or peer-Engine capabilities**. AE/CE v1 must not implement them; it must not invent private substitutes that block them (§0).

| Future component | How this design helps / preserves the path |
| --- | --- |
| Platform contract catalog + bus | AE pulls the audio contract via Platform APIs; same Engine integration model must accept sensor/Omnibus later |
| Shared multi-subscriber transport | AE intake wraps today’s single-taker PCM; AE→CE uses multi-listener signatures — patterns that evolve into Platform notify/pull |
| Engine Runtime | Registers concurrent Engines; AE/CE/Monitor as peers on shared lifecycle/health |
| Module Runtime | Runs modules on Candidate evidence / signature hist without touching I2S |
| Workflow Engine | Enables which modules run; controls promotion policy |
| Decision/Fusion | Consumes module outputs + candidate identity |
| ABC / Drone-ML | Classification modules — inputs are signatures/features, not raw MQTT |
| Spatial | Needs verified multi-mic timing; AE/CE stay unchanged except new feature fields |
| Monitor Engine | Independent consumer of non-audio contracts; not routed through AE; no audio-special path required |
| Reporting Engine | Publishes Observations from **promoted** results; must not subscribe to every AE tick |

**Stable concepts for later:** Platform contract IDs/versions, multi-subscriber notify/pull semantics, `candidate_id`, signature `window_seq` timeline, quality flags, Engine health strings, resource-policy hooks.

---

## 12. Risks, unresolved questions, alternatives

### 12.1 Risks

| Risk | Mitigation |
| --- | --- |
| Treating activity_detector as Candidate Engine | Explicit separate module + lifecycle; bridge only |
| Assuming TDOA-ready stereo | DIN0 window coherence only; spatial deferred |
| Stereo PCM cost | v1 uses mic0 PCM + mic1 metrics; extend Platform PCM API later |
| Confidence number misread as classifier posterior | Document as heuristic; keep schema name `confidence` scoped |
| Health/MQTT bloat | AE/CE state stays on-device; optional diag only |
| Enrich background mic0 gate nuance | Unrelated cleanup; do not block this design |
| Audio-only “runtime” / single-consumer foundation blocking Monitor Engine | Obey §0 principles; single-taker PCM and AE→CE stay migratable (§0.9) |
| Treating health enrichers as the Engine data plane | Enrichers are Hub telemetry; Engines need on-device pull/notify |

### 12.2 Unresolved (need verification before / during impl)

1. Exact `N_enter`, match distance, and `T_coast` — tune on EAR-001 with two mics.
2. Whether AE gets its own task in v1 or stays polled from `ear_obs`.
3. When Platform should retain stereo PCM (Mic0+Mic1) for AE — recommend a small Platform extension ADR when needed.
4. Mapping `candidate_id` → Observation `track_id` / `observation_id` — Reporting design.
5. Whether L2 publish-to-Hub remains Observation-only (yes for now).
6. When to schedule the shared Platform contract-bus / Engine registry ADR relative to Monitor Engine needs.
7. If/when Candidate Engine should generalize beyond acoustic signatures — requires a **separate justified proposal**; not assumed by this design.

### 12.3 Alternatives considered

| Alternative | Why not preferred |
| --- | --- |
| Put Audio Engine in Platform | Violates product/interpretation boundary; blocks non-audio Engines |
| Only extend activity_detector | Remains single-stream; hard to multi-candidate |
| Signature = L1 JSON blob only | Loses energy/quality/timing unity CE needs |
| Require 4 mics for AE | Blocks development; contradicted by available hardware |
| Event-per-window MQTT | Bandwidth + conflates Observation with analysis |
| Build full Engine Runtime before AE | Over-scopes; §0 allows designing compatibility first |
| Route Monitor Engine through Audio Engine | Wrong contract; Engines are peers on Platform |

---

## 13. Implementation map (when approved — not now)

| Item | Horizon | Kind |
| --- | --- | --- |
| `sp_mic_*` acquisition, health, mic0 PCM (audio contract seed) | Platform today / AE v1 uses | **Reuse** |
| Health enricher table, device config/time/MQTT | Platform today | **Reuse** (foundations; not Engine data plane) |
| L0/L1/L2 analyze functions | AE v1 | **Reuse** (Engine domain math) |
| `sp_mic_get_channel` for mic1 pair features | AE v1 | **Reuse** |
| AE intake wrapper over PCM job / listeners for CE | AE/CE v1 | **New** (migratable; not final bus) |
| Stereo PCM hand-off | Shared Platform later | **Extend** |
| Multi-subscriber window notify → shared contract transport | Shared Platform later | **Extend** / **New later** |
| Versioned Platform contract IDs + Engine/Module lifecycle registry | Shared Platform later | **New later** |
| Resource policy + formal contract timestamps | Shared Platform later | **New later** |
| Sensor / Omnibus producers + Monitor Engine | Shared Platform later + peer Engine | **New later** |
| AE runtime, signature, history | AE v1 | **New** (Engine, not Platform) |
| CE model + FSM (acoustic signatures) | CE v1 | **New** (Engine, not Platform) |
| Bridge activity_detector ↔ CE promotion | Product v1 | **Extend** |
| TDOA / classifiers / Module Runtime / Engine Runtime | Later | **New later** |

### Implementation status (progress)

| Milestone | Status |
| --- | --- |
| M1 — host-testable AE signature/history + CE lifecycle (no device wire) | **Done** — `audio_engine/`, `candidate_engine/` |
| M2 — device intake: sole `sp_mic_take_pcm_job`, align Mic0/1, CE listener, poll from `ear_obs` | **Done** — `ae_runtime/` + Platform `pcm_job.window_seq`; activity/Observation unchanged |
| M3 — Observation promotion bridge (activity_detector interim) | **Done** — Ear `ce_promotion_bridge` association-only; RMS detector remains Observation authority; no `identity.track_id`; one primary ACTIVE (evidence → ema_rms → candidate_id); Platform `config.sp.version` from `firmware/platform/VERSION` |
| M4 — AE/CE lifecycle split, gap/overwrite | **Done** — AE-only device path; CE listener; gap eval; Observation unchanged |
| M5 — Dual PCM Mic0+Mic1 + AE stereo foundation | **Done** — SP dual PCM retain; `ae_stereo`; `ae:` diag; HW audio-test verified on EAR-001 (Test A L/R still open) |
| Platform USB provision wire ownership | **Done** — `SP_PROVISION_*` tokens (`sp.version` 0.2.0); web accepts legacy `DBMAP_PROVISION_*` |
| AE time/data contract hardening | **Done** — `ae_time` publish validation; history since/expire by `window_end_mono_us`; `ae_contract.h` consumer guarantees |

**M2 association rule:** Platform publishes a coherent **window job** (`sp_mic_take_window_job`): PCM + channel snapshots for the same `window_seq` / `measured_at`, under one lock (and `s_latest` is updated before the job becomes takeable). AE must use the bundled channels — not a later `get_channel()` — and still validates seq/time before pair metrics. Missing channels stay unmasked (not zero).

**M3 Promotion Bridge (implemented):** Ear-owned `ce_promotion_bridge` registers `ce_set_promotion_cb`, defers ENTER/EXIT off the AE publish stack, and associates at most one primary `ACTIVE` candidate with the sole open RMS Observation episode. Selection: higher `evidence_count`, then higher `ema_rms_dbfs` (tie-break only), then lower `candidate_id`. The RMS activity detector remains the only Observation open/close authority (D1). M3 does **not** emit `identity.track_id` (D3). This is not multi-source event identity. CE tracks `promotion_entered` so `CE_PROMOTION_EXIT` fires exactly once when a promoted candidate’s lifecycle ends, including `PROMOTED → COASTING → EXPIRED` (gap expiry always coasts before free). Rematch after a promoted COASTING restores `PROMOTED` (no second ENTER); primary selection also blocks while `promotion_entered` is set during COASTING. Bridge UART diag (`ear_obs: ce_bridge diag …`) logs association/queue counters at the AE diag cadence.

**Platform version:** `firmware/platform/VERSION` → builder `config.sp.version` (currently **0.2.0**, independent of product `firmware_version`). USB provision wire tokens are `SP_PROVISION_*` (web accepts legacy `DBMAP_PROVISION_*`).

§0.9 constraints remain temporary in M1–M3.

### Assumptions still needing verification

- DIN0 L/R physical straps match software left/right contract on the bench board.
- 1 s non-overlapping windows suffice for first multi-candidate demos.
- Existing L0–L2 CPU headroom remains after CE (expected, not re-measured here).

---

## 14. Required diagrams (index)

1. Multi-engine Platform architecture — §0 / §0.1  
2. Complete audio data flow — §4.1  
3. Audio Engine internal processing — §4.1 (AE subgraph) + §5  
4. Candidate lifecycle — §8  
5. AE ↔ CE interface — §9.3  
6. Future Engine Runtime / Module / Workflow / Reporting — §11  

---

## 15. Review checklist

- [ ] Long-term reusable multi-engine Platform goal accepted (§0)  
- [ ] Five architectural principles accepted (independent Engines, generic contracts, shared transport direction, shared lifecycle/services, clear ownership)  
- [ ] Three horizons clear: Platform today / AE·CE v1 uses / shared Platform later  
- [ ] Candidate Engine kept audio-signature-specific unless separately justified  
- [ ] Constraint mitigations in §0.9 accepted (single-taker PCM, AE→CE locality, etc.)  
- [ ] Responsibilities Platform / AE / CE accepted  
- [ ] Glossary distinctions accepted (including contract / Engine / Module)  
- [ ] 1 s window recommendation accepted or amended  
- [ ] Feature set v1 accepted (including stereo Δ without TDOA)  
- [ ] Candidate FSM accepted as non-Observation  
- [ ] Interface precise enough for independent implementation and future multi-subscriber migration  
- [ ] Two-mic development constraint accepted  
- [ ] No Engine Runtime / bus / registry implementation required by AE/CE v1  
- [ ] No implementation until this design is reviewed  

**Most important outcome:** coherent AE/CE boundaries that preserve a progressive, reusable multi-engine Sensor Platform — not the largest feature set, and not a premature Platform Runtime.
