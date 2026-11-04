# Observation protocol v1 — L2 Acoustic Dynamics (proposed)

**Status:** Proposed contract only. Not implemented in Ear, Hub, or simulator.  
**Source investigation:** [`ear-l2-acoustic-features-investigation.md`](./ear-l2-acoustic-features-investigation.md)  
**Protocol version:** **`1`** hierarchical envelope (`identity` / `context` / `acoustic` / `spatial`) — unchanged.  
**Placement:** nullable sibling `acoustic.dynamics` with `schema: "l2"`.  
**Document location:** `docs/diagnostics/` (kept here by decision D4; not promoted in this step).  
**Independent of:** `acoustic.classification` (source-family hypothesis; meaning unchanged; remains null on physical Ear).

L2 describes **acoustic dynamics** of the combined microphone scene within and between analysis windows. It is an acoustic representation, not a classifier.

When L2 is disabled (future Kconfig), firmware shall serialize `acoustic.dynamics` as JSON **null** (same pattern as L1 `signature`).

This document must be approved before any firmware, Hub, protocol, or schema implementation.

---

## Resolved design decisions (D1–D5)

| ID | Decision |
| --- | --- |
| **D1** | Continuity tolerance \(T_{\mathrm{tol}} = \mathbf{\pm 100\,\mathrm{ms}}\) around the nominal 1 s window period. Tolerance absorbs small timestamp jitter only; it must **not** conceal a missing analysis window. Missing, invalid/clipped, or other continuity-breaking events still break the chain. |
| **D2** | Use a **one-window continuity cache** for `dominant_delta_hz`. Preserve it across ordinary Observation interval boundaries (including PERIODIC). Attribute each valid pair to the **later** window’s Observation interval. Clear the cache on all continuity-breaking events. |
| **D3** | On L2 history overflow: set `history_overflow: true`, **clear all** L2 interval metrics, force count placeholders to 0 (**Option A** — untrusted when the flag is set; never confuse with a trusted empty interval), and **break** dominant-frequency continuity. |
| **D4** | Keep this contract under `docs/diagnostics/`. |
| **D5** | Publish **raw** `dominant_delta_hz` with **no** flatness/tonality gate. Document peak-hopping limitations; do not add gating features. |

---

## Scope

| In scope | Out of scope |
| --- | --- |
| `spectral_flux`, `envelope_cv`, `dominant_delta_hz` only | Additional features |
| Same window association rule as L0/L1 | Source classification / source-family / confidence labels |
| Reuse of L1 FFT frame loop (no second FFT/PCM pass) | Changing L0/L1 formulas or field meanings |
| Interval-scoped publication | Episode-level acoustic rollups |
| Bounded static memory | Autocorrelation, HNR, harmonic grids, MFCC, embeddings |
| | Overlapping FFT frames (v1) |
| | Physical velocity / Doppler / “source movement” claims |
| | Flatness/tonality gating of `dominant_delta_hz` |

Observation interval association (identical to L0/L1):

```text
start < window_end_mono_us ≤ end
```

---

## Shared analysis parameters

Aligned with L1 ([`../ear-l1-signature-contract.md`](../ear-l1-signature-contract.md)):

| Parameter | Value |
| --- | --- |
| Sample rate \(f_s\) | 16 000 Hz |
| Analysis window | 1000 ms PCM buffer |
| FFT size / frame length \(N\) | 1024 samples (64 ms) |
| Frame hop | 1024 samples (non-overlapping) |
| Complete frames per 1 s window | \(\lfloor 16000/1024\rfloor = 15\) (trailing 576 samples unused) |
| Window function (FFT path) | Hann (identical to L1); **envelope RMS uses raw frame samples before Hann** |
| Analysis band | 80–7600 Hz |
| Bin width | \(f_s / N = 15.625\) Hz |
| Energy floor | `1e-12` (FS-relative total in-band power), same numeric class as L1 |
| Clip rule | peak ≥ −0.05 dBFS → window clipped (same as L0/L1) |

L1 output semantics (centroid, bandwidth, dominant, flatness, interval aggregates) are **unchanged**. L2 may read intermediate frame powers and L1 window scalars; it must not alter L1 published values.

---

## Normative feature definitions

### 1. `spectral_flux` (per analysis window)

#### 1.1 Frame power

During the existing L1 non-overlapping FFT loop, after each complete frame \(i = 0 \ldots F-1\) (\(F =\) `frame_count`, normally 15), let

\[
P_i[k] = \Re(X_i[k])^2 + \Im(X_i[k])^2
\]

be the **FS-relative linear power** at bin \(k\) for that frame (identical quantity L1 accumulates into `power_acc` before averaging). No second FFT is permitted.

#### 1.2 Analysis bins

Let \(\mathcal{B}\) be the set of bins with

\[
f_k = k \cdot f_s / N \in [80,\,7600]\ \text{Hz}
\]

(same closed band as L1). Bins outside \(\mathcal{B}\) are ignored.

#### 1.3 Frame-to-frame distance

For each \(i = 1 \ldots F-1\), define the **absolute** (two-sided) spectral difference, energy-normalized by the **current** frame:

\[
d_i =
\frac{\displaystyle\sum_{k \in \mathcal{B}} \lvert P_i[k] - P_{i-1}[k]\rvert}
     {\displaystyle\sum_{k \in \mathcal{B}} P_i[k]}
\]

- Numerator uses **absolute** differences (positive-only / half-wave rectification is **not** used).
- Denominator is current-frame in-band power (not previous, not mean spectrum).
- \(d_i\) is dimensionless and \(\ge 0\). It is **not** bounded above by 1.

#### 1.4 Window aggregation

\[
\mathrm{spectral\_flux} = \frac{1}{M}\sum_{i \in \mathcal{V}} d_i
\]

where \(\mathcal{V}\) is the set of frame indices \(i \ge 1\) for which the pair \((i-1, i)\) is **flux-valid**, and \(M = |\mathcal{V}|\).

A frame pair is flux-valid iff all of:

1. Both frames exist (\(F \ge 2\)).
2. \(\sum_{k \in \mathcal{B}} P_i[k] > 1\mathrm{e}{-12}\).
3. \(\sum_{k \in \mathcal{B}} P_{i-1}[k] > 1\mathrm{e}{-12}\).

If \(M = 0\), the window has **no** `spectral_flux` value: it does **not** increment `spectral_flux.valid_count`. That feature-specific failure alone does **not** increment `invalid_window_count` (see § Window counts versus feature counts).

#### 1.5 Contribution when the window is L2-eligible

| Condition on an associated window | Effect |
| --- | --- |
| Fails **general** L2 eligibility (L1 invalid, clipped, or \(F < 2\)) | Counts in `invalid_window_count`; no flux sample |
| L2-eligible and \(M \ge 1\) | Increments `spectral_flux.valid_count`; contributes one flux sample |
| L2-eligible but \(M = 0\) (all frame pairs failed energy gates) | Remains in `window_count`; **no** flux sample; **not** an `invalid_window_count` case |

Near silence: frame pairs failing the energy floor are skipped for flux only.

**Implementation constraint:** compute \(d_i\) inside the L1 frame loop (retain previous-frame band-limited power, \(\approx 513\) floats max; implementations may store only bins in \(\mathcal{B}\)). Do not re-read PCM for flux. Do not change L1’s averaged `power_acc` feature path beyond reading per-frame \(P_i\) before/while accumulating.

---

### 2. `envelope_cv` (per analysis window)

#### 2.1 Frame RMS (time domain)

For each complete non-overlapping frame \(j = 0 \ldots F-1\) of \(N = 1024\) int32 samples \(s[n]\) (same framing as L1, **before** Hann), with full-scale magnitude \(S = 2^{\mathrm{full\_scale\_bits}-1}\):

\[
r_j = \sqrt{\frac{1}{N}\sum_{n=0}^{N-1}\left(\frac{s[n]}{S}\right)^2}
\]

\(r_j\) is FS-relative linear RMS (dimensionless amplitude), \(\ge 0\).

Hann weighting is **not** applied to envelope RMS (envelope describes time-domain level; Hann is only for the FFT path).

#### 2.2 Coefficient of variation

Over the set \(\mathcal{R}\) of frames with \(r_j > \varepsilon_{\mathrm{rms}}\):

\[
\mu_{\mathrm{RMS}} = \frac{1}{R}\sum_{j \in \mathcal{R}} r_j,
\quad
\sigma_{\mathrm{RMS}} = \sqrt{\frac{1}{R}\sum_{j \in \mathcal{R}}(r_j - \mu_{\mathrm{RMS}})^2}
\quad\text{(population stdev)}
\]

\[
\mathrm{envelope\_cv} = \frac{\sigma_{\mathrm{RMS}}}{\mu_{\mathrm{RMS}}}
\]

| Constant | Value | Role |
| --- | --- | --- |
| \(\varepsilon_{\mathrm{rms}}\) | \(10^{-6}\) (FS linear) | Drop near-silent frames from \(\mathcal{R}\) |
| Minimum \(R = |\mathcal{R}|\) | **2** | Below this → no envelope_cv value |
| Minimum \(F\) | **2** | Align with flux framing; single-frame CV undefined |

If \(\mu_{\mathrm{RMS}} \le \varepsilon_{\mathrm{rms}}\) or \(R < 2\), the window has **no** `envelope_cv` value (feature-specific; does not by itself increment `invalid_window_count`).

Clipped / L1-invalid / \(F < 2\) windows fail **general** eligibility and do not contribute `envelope_cv`.

`envelope_cv` is dimensionless and \(\ge 0\) (unbounded above in principle; pathological spikes only near the silence gate).

A window may yield a valid `envelope_cv` while yielding **no** `spectral_flux` (for example when frame-pair energy gates fail for flux). The reverse can also occur. Feature `valid_count` fields are independent.

#### 2.3 Limitation — 5 Hz AM and 64 ms frames (normative warning)

Frame rate \(f_{\mathrm{frame}} = f_s / N \approx 15.625\) Hz.  
For amplitude modulation at \(f_m = 5\) Hz:

- Modulation period \(T_m = 200\) ms ≈ **3.125 frames per period**.
- Nyquist frequency of the envelope sampler ≈ **7.81 Hz**, so 5 Hz is theoretically below Nyquist but **coarsely sampled**.
- Measured CV is **phase-sensitive**: depending on where the 64 ms non-overlapping blocks land relative to the modulation waveform, captured peak/trough content varies.
- Host tests **must** exercise **multiple modulation phase offsets**.
- Tests must **not** require every phase offset to produce the same CV.
- Tests must **not** define one optimistic universal CV threshold that all phase offsets must pass.
- v1 **does not** introduce overlapping frames.

**Contract consequence:** `envelope_cv` is a coarse intra-window level-fluctuation index. Host validation should confirm **qualitative** elevation versus steady tones where supported, and **document phase sensitivity**, rather than claim calibrated 5 Hz modulation-depth measurement.

---

### 3. `dominant_delta_hz` (pairwise across analysis windows)

#### 3.1 Definition

Let \(f_n^{\mathrm{dominant}}\) be the L1 per-window `dominant_hz` for analysis window \(n\) (bin centre of the in-band peak of the **mean** frame power spectrum — unchanged L1 definition).

For a **valid pair** \((n-1, n)\):

\[
\Delta f_n = \bigl\lvert f_n^{\mathrm{dominant}} - f_{n-1}^{\mathrm{dominant}}\bigr\rvert
\]

Unit: **Hz**, \(\ge 0\). Quantization is multiples of the bin width 15.625 Hz when both peaks are stable on-bin.

**Interpretation (normative):** \(\Delta f_n\) is only a change in the dominant spectral peak between consecutive analysis windows. It is **not** physical source motion, velocity, bearing change, or Doppler.

#### 3.2 Raw publication — peak hopping (D5)

Publish \(\Delta f_n\) **raw**. Do **not** gate pairs using spectral flatness, tonality, HNR, or any other classifier-like measure.

**Limitation:** in noisy or broadband scenes the argmax bin can jump between unrelated peaks from window to window. That can produce **large** `dominant_delta_hz` values that do **not** necessarily represent a meaningful physical frequency shift of a single tonal source. Consumers must treat large deltas in high-flatness / noise-like contexts with caution. v1 does not add features to hide this.

#### 3.3 What counts as a valid pair

Both windows must be:

1. L1-`valid` and **unclipped**.
2. **Time-consecutive** under the continuity rule (§ Continuity and reset).
3. Ordered by increasing `window_end_mono_us`.

The **first** analysis window after boot, after episode reset, or after a continuity break has **no** predecessor → **no** \(\Delta f\) for that window.

#### 3.4 Pair count ≠ window count

| Quantity | Meaning |
| --- | --- |
| `window_count` | Associated windows meeting **general** L2 eligibility (see § Window counts versus feature counts) |
| `dominant_delta_hz.valid_count` | Number of **valid pairs** whose \(\Delta f_n\) enter this interval’s aggregate |

**Never** use `window_count` as a substitute for pair count. With \(W\) consecutive eligible windows and no gaps, pair count is at most \(W-1\), and may differ further when pairs cross Observation boundaries (attribution below).

#### 3.5 Attribution to an Observation interval (D2)

A valid pair \((n-1, n)\) **contributes to the Observation interval that associates window \(n\)** (the later window), even if window \(n-1\) was associated to a **previous** Observation interval.

This keeps ordinary PERIODIC boundaries from erasing continuity when analysis time continues without a gap.

---

## Window counts versus feature counts

These definitions are normative for JSON and aggregation.

### General L2 window eligibility

An associated analysis window is **L2-eligible** iff all of:

1. L1 `valid == true`
2. Not clipped (peak &lt; −0.05 dBFS)
3. \(F \ge 2\) complete FFT-aligned frames

| Counter | Definition |
| --- | --- |
| `window_count` | Count of associated **L2-eligible** windows |
| `invalid_window_count` | Count of associated windows that **fail general eligibility** (not L1-valid, clipped, or \(F < 2\)) |

`invalid_window_count` must **not** include L2-eligible windows that merely fail a **feature-specific** calculation (for example flux with \(M = 0\), or envelope with \(R < 2\)).

### Feature-level counts

| Field | Counts |
| --- | --- |
| `spectral_flux.valid_count` | L2-eligible associated windows that produced a flux value (\(M \ge 1\)) |
| `envelope_cv.valid_count` | L2-eligible associated windows that produced an envelope_cv value |
| `dominant_delta_hz.valid_count` | **Valid pairs** attributed to this interval (not windows) |

Independence examples:

- `envelope_cv` may be valid while `spectral_flux` has no valid frame pairs because flux energy thresholds were not met → `window_count` still includes the window; only `spectral_flux.valid_count` stays flat for that window.
- `spectral_flux` may be valid while `envelope_cv` fails its RMS set → same pattern with roles reversed.
- `dominant_delta_hz.valid_count` can be 0 while `window_count ≥ 1` (no predecessor / continuity break).

---

## Continuity and reset

### Normative continuity behaviour (D2)

Maintain a **one-window continuity cache**: last eligible L1 `dominant_hz`, its `window_end_mono_us`, and a validity flag. Update the cache after each analyzed window according to the rules below.

| Event | Continuity cache |
| --- | --- |
| Ordinary Observation interval boundary (START→PERIODIC, PERIODIC→PERIODIC, PERIODIC→END) **without** a continuity-breaking event | **Preserve** |
| Missing analysis window (e.g. PCM overwrite / poll skip) | **Clear** (break) |
| Invalid or clipped window | **Clear** (break); do not bridge across |
| Timestamp discontinuity (gap rule) | **Clear** (break) |
| History overflow (D3) | **Clear** (break) |
| Episode END / registry release / detector return to IDLE / OTA forced end | **Clear** (break) |
| Boot / cold start | Empty (no previous state) |
| Comparing non-consecutive windows | **Forbidden** — never silently pair across a break |

### Gap rule (D1)

Windows \(n-1\) and \(n\) are time-consecutive iff

\[
\bigl\lvert (t_n - t_{n-1}) - T_w \bigr\rvert \le T_{\mathrm{tol}}
\]

where \(t\) is `window_end_mono_us`, \(T_w =\) `analysis_window_ms` × 1000 µs (nominally \(10^6\) µs), and

\[
T_{\mathrm{tol}} = 100\,\mathrm{ms} = 100000\,\mu\mathrm{s}
\]

**Meaning of the tolerance:** it only absorbs **small timestamp deviations** around the nominal 1 s period (scheduler jitter, measurement skew). It must **not** be used to treat a skipped/missing analysis window as consecutive. A missing window, invalid/clipped window, overflow, episode reset, or other listed break **always** clears the cache regardless of whether a naive timestamp delta might still fall inside \(\pm 100\) ms of some other reference.

**Empirical status:** \(\pm 100\) ms is the **contract decision (D1)**. It is **not yet validated** against measured `window_end_mono_us` jitter on EAR-001. See § EAR-001 continuity-tolerance validation. Do not widen or narrow \(T_{\mathrm{tol}}\) from assumptions alone; if hardware evidence shows ordinary jitter exceeds \(\pm 100\) ms, report that finding for human review before changing the contract.

### Architecture alignment note

**Existing behaviour that supports this model:**

- L0/L1 window rings are **not** cleared on START/PERIODIC/END finalize; intervals only re-associate by timestamp.
- PERIODIC intervals are contiguous in the activity detector (`interval_start = last_emit`, association exclusive-start / inclusive-end), so analysis windows continue across report boundaries.

**Implementation requirement (not present in today’s code):**

- L1 interval finalize alone cannot form cross-boundary pairs because it only sees windows inside one emission span.
- Firmware must implement the explicit **1-deep continuity cache** described above. This is **not** an episode-level acoustic rollup and does **not** change L0/L1 formulas.

---

## Interval aggregation

### Association

For each Observation emission, consider analysis windows with `start < window_end ≤ end`.

Apply § Window counts versus feature counts, then aggregate each feature over its own contributors only.

### Means and standard deviations

For a multiset of contributing samples \(\{x_1,\ldots,x_n\}\) (windows for flux/cv; **pairs** for dominant delta):

- `mean` = arithmetic mean  
- `stdev` = **population** standard deviation \(\sqrt{\frac{1}{n}\sum (x_i-\mathrm{mean})^2}\) when \(n \ge 2\); otherwise JSON **null**

(Same population convention as the L1 contract.)

### Null behavior

| Situation | JSON |
| --- | --- |
| Feature `valid_count = 0` | `"mean": null`, `"stdev": null` |
| Feature `valid_count = 1` | numeric `mean`, `"stdev": null` |
| Feature `valid_count ≥ 2` | numeric `mean` and `stdev` |
| Entire interval with `window_count = 0` | Publish schema-valid object; all feature means/stdevs null; all feature `valid_count` = 0 |

### History overflow (D3)

If the Observation window history/ring cannot retain usable windows for a **complete, trustworthy** L2 interval aggregate (same failure class as L0 `history_overflow`):

1. Set `history_overflow: true`.
2. **Clear all** L2 feature metrics for that emission: every feature `valid_count = 0`, all means/stdevs **null**. Do **not** publish partial flux/cv/delta aggregates as if they represented a complete interval.
3. **Break** the dominant-frequency continuity cache.
4. **Count representation (Option A — retained):**
   - Set `window_count = 0`.
   - Set `invalid_window_count = 0`.
   - These zeros are **placeholder / untrusted**, not measured tallies.
   - Do **not** invent a partial eligible/invalid tally from whatever still happens to sit in the ring.
   - Do **not** overload `invalid_window_count` to mean “overflow” or “corrupt history.” Overflow is signaled **only** by `history_overflow: true`.

#### Overflow counts: alternatives considered

Zero counts after overflow can look like a genuinely empty interval if a consumer ignores `history_overflow`. Alternatives:

| Option | Representation | Pros | Cons |
| --- | --- | --- | --- |
| **A (recommended)** | Keep integer counts; force both to **0**; define them as **invalid/untrustworthy whenever `history_overflow` is true** | No JSON type change; Hub can keep `int ≥ 0`; matches already-agreed wire shape; single boolean distinguishes empty vs overflow | Consumers that ignore the flag can misread zeros as “no windows” |
| **B** | Set `window_count` / `invalid_window_count` to JSON **null** on overflow | Makes unavailability explicit in the type | Changes the agreed “counts always present as int” shape; Hub/`extra=forbid` models need `int \| null`; more churn for little gain if the flag is checked first |
| **C** | Sentinel (e.g. −1) or a separate overflow-count field | — | Worse typing than null; invents new fields without need |

**Recommendation: Option A.** Do not silently switch to null counts. Require consumers (and Hub analytics) to treat **all** of `window_count`, `invalid_window_count`, feature `valid_count`, and feature means/stdevs as **untrustworthy whenever `history_overflow` is true**. The zeros are wire placeholders for schema stability, not observations.

#### Empty interval vs overflow (must not be confused)

| Situation | `history_overflow` | `window_count` | `invalid_window_count` | Feature metrics | Meaning |
| --- | --- | --- | --- | --- | --- |
| Genuinely empty associated set | **false** | **0** | **0** (or &gt;0 only if associated rejects exist without eligibles) | null / `valid_count` 0 | Trusted: no L2-eligible windows in the interval |
| Associated only invalid/clipped | **false** | **0** | **&gt;0** | null / 0 | Trusted: windows seen but none eligible |
| History overflow / corrupt aggregate | **true** | **0** (untrusted placeholder) | **0** (untrusted placeholder) | null / 0 | **Not** empty; **not** ordinary invalids — discard L2 numerics |

**Normative consumer rule:** if `history_overflow` is true, do not interpret zero counts as “no activity” or as “no invalid windows.” Interpret only: “L2 interval aggregate unavailable.”

---

## Proposed JSON structure

### Envelope relationship

```text
acoustic.measurements     → schema "l0"   (required; unchanged)
acoustic.signature        → schema "l1" | null  (unchanged)
acoustic.dynamics         → schema "l2" | null  (NEW, proposed)
acoustic.classification   → existing L2-class hypothesis schema | null  (unchanged; not dynamics)
```

`protocol_version` remains **1**, subject to compatibility review when Hub gains the field (same additive pattern as optional `signature`).

### Hub property wiring (proposed; not implemented)

Hub today rejects unknown keys (`extra=forbid` on `ObservationAcoustic`). Before any Ear build publishes a non-null `dynamics` object, Hub must accept it explicitly.

| Item | Proposed contract |
| --- | --- |
| Location | Optional property `acoustic.dynamics` on the hierarchical Observation body (sibling of `measurements`, `signature`, `classification`) |
| Absent | Older firmware / L2 publish off: property may be **omitted** or JSON **null** — both mean “no dynamics” |
| Null | JSON `null` is valid and means L2 not published for this message |
| Present object | Must validate as L2 dynamics (below); unknown keys rejected |
| Discriminator | Object field `schema` must equal `"l2"` (string literal) |
| Version | Integer `version` ≥ 1; initial package **1** |
| Independence | `acoustic.classification` remains a separate optional object with its existing meaning; Hub must **not** require, merge, or reinterpret classification when dynamics is present or null |

**Validation expectations for a present dynamics object:**

- Required meta: `schema`, `version`, `sample_rate_hz`, `analysis_window_ms`, `fft_size`, `freq_min_hz`, `freq_max_hz`, `frame_hop_samples`, `window_count`, `invalid_window_count`, `history_overflow`.
- Required nested feature objects: `spectral_flux`, `envelope_cv`, `dominant_delta_hz`, each with `valid_count` (int ≥ 0) and `mean` / `stdev` that are either finite numbers or JSON **null** per § Null behavior.
- Counts: `window_count` ≥ 0, `invalid_window_count` ≥ 0; when `history_overflow` is true, both counts **must** be 0 and all feature `valid_count` **must** be 0 with null means/stdevs. Hub/analytics **must not** treat those zeros as a trusted empty interval (Option A).
- Feature means, when non-null: finite, non-NaN; flux/CV ≥ 0; `dominant_delta_hz` ≥ 0.
- Feature stdev, when non-null: finite, ≥ 0; requires `valid_count ≥ 2`.
- Do not accept classification fields inside `dynamics`.

Until Hub ships this acceptance, firmware publication of non-null `dynamics` is forbidden by rollout policy (see § Compatibility and rollout).

### Field table (`acoustic.dynamics`)

| Field | Type | Unit / range | Validity / null rules |
| --- | --- | --- | --- |
| `schema` | `"l2"` | — | Always when object present |
| `version` | int ≥ 1 | semantics package; start at **1** | Always |
| `sample_rate_hz` | int > 0 | Hz | Always (e.g. 16000) |
| `analysis_window_ms` | int > 0 | ms | Always (e.g. 1000) |
| `fft_size` | int > 0 | samples | Always (1024) |
| `freq_min_hz` | number | Hz | Always (80.0) |
| `freq_max_hz` | number | Hz | Always (7600.0) |
| `frame_hop_samples` | int > 0 | samples | Always (1024) |
| `window_count` | int ≥ 0 | associated **L2-eligible** windows when `history_overflow` is false; if overflow, **0** as untrusted placeholder (Option A) | Always present as int |
| `invalid_window_count` | int ≥ 0 | associated general-eligibility failures when `history_overflow` is false; if overflow, **0** placeholder — **not** an overflow flag | Always present as int |
| `history_overflow` | bool | — | Always; sole overflow/corruption signal (D3). When true, **all** counts and feature numerics are untrustworthy |
| `spectral_flux.valid_count` | int ≥ 0 | windows with a flux value | Always |
| `spectral_flux.mean` | number \| null | dimensionless ≥ 0 | null if valid_count=0 |
| `spectral_flux.stdev` | number \| null | dimensionless ≥ 0 | null if valid_count&lt;2 |
| `envelope_cv.valid_count` | int ≥ 0 | windows with an envelope_cv value | Always |
| `envelope_cv.mean` | number \| null | dimensionless ≥ 0 | null if valid_count=0 |
| `envelope_cv.stdev` | number \| null | dimensionless ≥ 0 | null if valid_count&lt;2 |
| `dominant_delta_hz.valid_count` | int ≥ 0 | **pairs** | Always |
| `dominant_delta_hz.mean` | number \| null | Hz ≥ 0 | null if valid_count=0 |
| `dominant_delta_hz.stdev` | number \| null | Hz ≥ 0 | null if valid_count&lt;2 |

Unknown keys inside `dynamics` shall be rejected by Hub (`extra=forbid`), matching L0/L1.

### Example — fully valid interval

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
  "history_overflow": false,
  "spectral_flux": {
    "valid_count": 5,
    "mean": 0.042,
    "stdev": 0.011
  },
  "envelope_cv": {
    "valid_count": 5,
    "mean": 0.18,
    "stdev": 0.05
  },
  "dominant_delta_hz": {
    "valid_count": 5,
    "mean": 15.625,
    "stdev": 0.0
  }
}
```

Note: `dominant_delta_hz.valid_count` may equal 5 when continuity carries one pair across the previous interval boundary plus four in-interval successors (example only). That count is still **pairs**, not windows.

### Example — L2-eligible window with feature-specific flux failure

Illustrative: five eligible windows; one has \(M = 0\) for flux but valid envelope_cv.

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
  "history_overflow": false,
  "spectral_flux": { "valid_count": 4, "mean": 0.04, "stdev": 0.01 },
  "envelope_cv": { "valid_count": 5, "mean": 0.12, "stdev": 0.03 },
  "dominant_delta_hz": { "valid_count": 4, "mean": 0.0, "stdev": 0.0 }
}
```

### Example — insufficient data / empty interval

Trusted empty: `history_overflow` is **false**. Contrast with the overflow example below (same zero counts, different flag).

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
  "window_count": 0,
  "invalid_window_count": 0,
  "history_overflow": false,
  "spectral_flux": { "valid_count": 0, "mean": null, "stdev": null },
  "envelope_cv": { "valid_count": 0, "mean": null, "stdev": null },
  "dominant_delta_hz": { "valid_count": 0, "mean": null, "stdev": null }
}
```

### Example — first interval with windows but no dominant pairs

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
  "window_count": 1,
  "invalid_window_count": 0,
  "history_overflow": false,
  "spectral_flux": { "valid_count": 1, "mean": 0.01, "stdev": null },
  "envelope_cv": { "valid_count": 1, "mean": 0.02, "stdev": null },
  "dominant_delta_hz": { "valid_count": 0, "mean": null, "stdev": null }
}
```

### Example — invalid / clipped associated windows only

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
  "window_count": 0,
  "invalid_window_count": 4,
  "history_overflow": false,
  "spectral_flux": { "valid_count": 0, "mean": null, "stdev": null },
  "envelope_cv": { "valid_count": 0, "mean": null, "stdev": null },
  "dominant_delta_hz": { "valid_count": 0, "mean": null, "stdev": null }
}
```

### Example — history overflow

Same numeric zeros as an empty interval; **`history_overflow: true`** is the only trustworthy signal that this is **not** empty and that counts must be ignored (Option A).

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
  "window_count": 0,
  "invalid_window_count": 0,
  "history_overflow": true,
  "spectral_flux": { "valid_count": 0, "mean": null, "stdev": null },
  "envelope_cv": { "valid_count": 0, "mean": null, "stdev": null },
  "dominant_delta_hz": { "valid_count": 0, "mean": null, "stdev": null }
}
```

### Serialization precision (informative)

- Hz: about one decimal place in examples  
- Dimensionless flux / CV: about three decimal places  
- Wire formatting is an implementation detail; host/device comparison rules are defined in § Golden-vector numerical tolerances.

---

## Processing architecture (proposed)

```text
I2S → SPIRAM PCM window
        ↓
   L0 analyze (unchanged)
        ↓
   L1 FFT frame loop (unchanged outputs)
        ├─ per frame: P_i[k], accumulate power_acc
        ├─ per frame: r_j RMS (raw samples) → envelope_cv
        └─ per frame: |P_i−P_{i−1}| / sum P_i → spectral_flux
        ↓
   L1 features_from_power (unchanged) → dominant_hz
        ↓
   Continuity cache → maybe Δf_n
        ↓
   Push L0 / L1 / L2 window scalars to rings
        ↓
   On emission: interval finalize → acoustic.dynamics object
```

| Feature | Where computed | Memory |
| --- | --- | --- |
| `spectral_flux` | Inside L1 FFT loop | +previous frame power (≤513 float) in existing workspace class |
| `envelope_cv` | Same frame PCM touch as FFT input fill | +~15 floats scratch / running moments |
| `dominant_delta_hz` | After L1 window scalars available | +1-deep continuity cache (~16 B) |

**Forbidden:** retained PCM after poll; second full PCM pass; heap allocation for spectra; SPIRAM spectrum mirrors; modifying L0/L1 published contracts; CPU frequency / PM / DFS changes for this work.

L2 runs in the existing observation / analysis processing path alongside L0/L1.

---

## Performance assumptions and measurement requirements

### Baseline (measured on EAR-001 @ 160 MHz — user-reported)

| Stage | last | max |
| --- | ---: | ---: |
| L0 | ~62 ms | ~67 ms |
| L1 | ~40 ms | ~45 ms |
| Combined | ~102 ms | ~112 ms |

These L0/L1 figures are **measured baseline evidence**.

### L2 cost

| Claim | Status |
| --- | --- |
| “≈ 10 ms additional” from the investigation | **Estimate only — not measured** |
| Preferred target | **≤ 15 ms** incremental L2 `us_max` |
| Design ceiling | **30 ms** incremental; exceeding the preferred target requires investigation; exceeding the ceiling blocks publish-enable without revisit |
| Actual performance | **Must be measured on EAR-001** before locking budgets |
| CPU / PM / DFS | **No changes** (remain 160 MHz; no `CONFIG_PM_ENABLE` introduction for this work) |

### Required instrumentation (when implemented)

Extend serial diag (same cadence as L0/L1 perf lines):

```text
ear_obs: L2 perf analyze_n=… us_last=… us_max=…
```

Also continue monitoring:

- L0/L1 us_last/max (no material regression)
- PCM ring **overwrite** count (**must remain `overwrite=0`** in soak tests)
- `freq_mhz` / configured MHz
- free heap / SPIRAM

Soak: ≥500 analysis windows before declaring device budget OK. Do not treat host µs as S3 evidence.

---

## Host validation plan

Deterministic synthetic PCM (16 kHz, 24-bit FS int32, 1 s windows unless noted). Numeric gates marked **PROVISIONAL** require golden-vector calibration before hardening (see next section).

| # | Case | Expectation |
| --- | --- | --- |
| H1 | Stable pure tone (e.g. 1 kHz) | Low flux; low envelope_cv; \(\Delta f\) pairs ≈ 0 (≤ 1 bin) |
| H2 | Stable multi-tone | Low flux; low CV; \(\Delta f\) ≈ 0 if same peak wins |
| H3 | Stationary broadband noise | Flux moderate vs tone (**PROVISIONAL** ordering); CV low–moderate; \(\Delta f\) **may be large** due to raw peak hopping (D5) — document; not a continuity-logic failure by itself |
| H4 | AM tone 1 kHz × 5 Hz, depth ~0.5, **multiple phase offsets** (e.g. \{0, π/4, π/2, π, 5π/4, 3π/2\}) | Verify **qualitative** CV behaviour vs H1 where supported; **record phase sensitivity**; do **not** require identical CV across phases; do **not** impose one universal CV pass threshold for all phases; do **not** claim calibrated modulation-depth measurement |
| H5 | Linear FM chirp within one window | Flux **higher than H1**; CV moderate |
| H6 | Periodic pulse train | CV high; flux elevated vs steady tone |
| H7 | Near-silence / below energy floor | Feature values absent per feature rules; general invalid accounting only when eligibility fails |
| H8 | Abrupt dominant change between windows (1 kHz then 3 kHz) | \(\Delta f\) ≈ 2000 Hz ± 1 bin on the boundary pair |
| H9 | Missing / invalid preceding window | No pair across the hole; continuity cleared |
| H10 | Timestamp gap &gt; \(T_{\mathrm{tol}}\) (100 ms) | No pair; continuity cleared |
| H11 | Episode reset between windows | No pair across reset |
| H12 | Observation interval boundary **without** real time gap | Boundary pair **is** formed (D2 cache) and attributed to the **later** interval |

### Golden-vector numerical tolerances

Host tests are the normative reference implementation of the formulas. Device/firmware floats may differ slightly from host doubles/floats. Prefer **feature-specific** comparison rules over one global ε.

#### Comparison predicates (normative style)

| Predicate | Definition | Typical use |
| --- | --- | --- |
| `eq_null` | Both sides JSON-null / “no value” | Validity / empty paths |
| `near_abs(a,b,ε)` | \(\lvert a-b\rvert \le \varepsilon\) | Near-zero references; Hz on a bin grid |
| `near_rel(a,b,ε_rel,ε_abs)` | \(\lvert a-b\rvert \le \max(\varepsilon_{\mathrm{abs}},\,\varepsilon_{\mathrm{rel}}\cdot\max(\lvert a\rvert,\lvert b\rvert))\) | Dimensionless flux/CV away from zero |
| `ordering(a,b)` | \(a > b\) (strict) or \(a \ge b\) as stated | Cross-case qualitative gates (H1 vs H5) |

**Near-zero references:** when the golden/reference value is 0 (or the contract expects “≈ 0”), use **absolute** tolerance only (`near_abs`). Do **not** use relative tolerance alone — relative error is undefined or explosive at zero.

**Host vs ESP32-S3:** host golden vectors freeze expected values. On-device or cross-compiled checks against the same PCM may use the **same predicates** with an optional device slack multiplier only after a measured host↔device delta study; until that study exists, device numeric bit-identity with host is **not** required by this contract. Structural/validity behaviour (nulls, counts, continuity breaks) must match exactly.

#### Feature-specific provisional gates

Values below are **PROVISIONAL**. They are starting proposals for host self-consistency and cross-case ordering — **not** evidence-backed production thresholds. Finalize by: (1) generating deterministic golden PCM; (2) recording host reference outputs; (3) choosing ε from observed host run-to-run / float-vs-double spread plus a documented margin; (4) human approval of the frozen table.

| Feature / quantity | Comparison | Provisional ε / rule | Needed to finalize |
| --- | --- | --- | --- |
| `dominant_delta_hz` vs expected tonal step | `near_abs` | \(\varepsilon = 15.625\) Hz (**1 bin**) for stable on-bin tones (H8) | Confirm peak bin selection on golden tones |
| `dominant_delta_hz` steady tone continuity | `near_abs` to 0 | \(\varepsilon = 15.625\) Hz | Same |
| `spectral_flux` steady tone (H1) | `near_abs` to host golden | **PROVISIONAL** \(\varepsilon_{\mathrm{abs}} = 5\cdot 10^{-3}\) until golden recorded | Host golden mean flux on H1 PCM |
| `spectral_flux` chirp vs tone (H5 vs H1) | `ordering` | `flux_chirp > flux_tone` (no fixed ratio yet) | Optional ratio bound after goldens |
| `envelope_cv` steady tone (H1) | `near_abs` to host golden | **PROVISIONAL** \(\varepsilon_{\mathrm{abs}} = 0.02\) | Host golden CV on H1 |
| `envelope_cv` AM vs tone (H4) | qualitative + phase log | No universal threshold; expect CV elevated vs H1 for **some** phases; record all phases | Phase survey on host; never force all phases through one cut |
| `envelope_cv` / flux away from zero | `near_rel` vs golden | **PROVISIONAL** \(\varepsilon_{\mathrm{rel}} = 0.05\), \(\varepsilon_{\mathrm{abs}}\) as row above | Host float/double parity study |
| Interval `mean`/`stdev` | Same predicates on aggregates | Inherit per-feature rules; `stdev` null checks exact | — |
| Counts / flags | Exact integer / bool equality | `window_count`, `valid_count`, `history_overflow`, nullness | — |

Do **not** loosen provisional ε merely to make a failing test green without a recorded golden and a stated numerical justification.

---

## EAR-001 continuity-tolerance validation

**Contract value remains** \(T_{\mathrm{tol}} = \pm 100\) ms (D1). This section defines how to **empirically check** fitness; it does not change D1 by itself.

### Procedure (when L2 compute / diag timestamps are available)

1. Run EAR-001 at normal 1 s analysis cadence, 160 MHz, L0+L1 (and L2 compute when present), MQTT as in normal idle/active use.
2. Collect **≥ 500** consecutive **valid, unclipped** analysis windows with recorded `window_end_mono_us` (serial diag or host-side capture of the same monotonic stamps).
3. For each successive pair, compute \(\delta_n = (t_n - t_{n-1}) - 10^{6}\,\mu\mathrm{s}\).
4. Report **min**, **max**, mean, and a simple distribution (e.g. percentiles p50/p95/p99) of \(\delta_n\).
5. **Pass criterion for “ordinary jitter fits”:** among pairs with no intentional skip, \(\lvert\delta_n\rvert \le 100\) ms for all (or document any rare outliers separately with root cause).
6. **Break-criterion checks (must still break continuity regardless of tolerance):**
   - Force or observe a **missing** window (e.g. induced overwrite if safely testable) → continuity must clear.
   - Insert / observe an **invalid or clipped** window → continuity must clear.
   - Episode reset / END → continuity must clear.
7. Keep **`overwrite=0`** during the nominal jitter soak unless the missing-window case intentionally induces overwrite.
8. If ordinary operation shows \(\lvert\delta\rvert > 100\) ms without a missing window, **do not silently widen** \(T_{\mathrm{tol}}\) in firmware — file the measurement for human review of D1.

Until this procedure is executed, state in reports: “D1 = ±100 ms decided; EAR-001 jitter **unvalidated**.”

---

## Compatibility and rollout

1. **Approve** this written L2 contract (D1–D5 resolved; remaining items documented below).  
2. **Hub:** additive optional `acoustic.dynamics` acceptance per § Hub property wiring.  
3. **Host:** L2 calculate + tests (no MQTT); freeze goldens and ε.  
4. **Firmware:** compute behind `DBMAP_EAR_OBS_L2`; publish only when `DBMAP_EAR_OBS_L2_PUBLISH` and Hub are ready; diag timers.  
5. **Measure** on EAR-001 (L2 us_*, **overwrite=0**, continuity-tolerance procedure).  
6. **Enable publication** only after Hub acceptance + device validation.

Older firmware without `dynamics` remains valid. L0/L1 field meanings stay unchanged. Do not repurpose `acoustic.classification`.

---

## Configuration (proposed; not in tree)

Proposed Kconfig symbols (names only — **not** added to firmware in this documentation step):

| Symbol | Role |
| --- | --- |
| `DBMAP_EAR_OBS_L2` | Enable **L2 computation** (flux / envelope_cv / continuity cache / interval accumulators / perf counters) |
| `DBMAP_EAR_OBS_L2_PUBLISH` | Enable **MQTT/JSON publication** of a non-null `acoustic.dynamics` object |

Computation and publication remain **logically separate** (measure without publishing; publish only after Hub + budget sign-off).

### Required Kconfig dependency

**Normative proposal for implementation:**

```text
DBMAP_EAR_OBS_L2_PUBLISH depends on DBMAP_EAR_OBS_L2
```

Publication without computation is meaningless. The dependency must be **build-time** (`depends on` in Kconfig), not only a runtime fallback, so the Disabled/Enabled cell cannot be selected in `menuconfig` / sdkconfig.

Additionally (unchanged architecture): L2 computation should itself depend on L1 being enabled (shared FFT loop), e.g. `DBMAP_EAR_OBS_L2 depends on DBMAP_EAR_OBS_L1` (exact L1 symbol as already used in tree when implemented).

| L2 computation (`DBMAP_EAR_OBS_L2`) | L2 publication (`DBMAP_EAR_OBS_L2_PUBLISH`) | Expected behaviour |
| --- | --- | --- |
| Disabled | Disabled | No L2 calculation; `acoustic.dynamics` is JSON **null** (or omitted per serializer policy) |
| Enabled | Disabled | Calculate L2 and update diag/perf; **do not** publish a dynamics object (`null` / omit) |
| Enabled | Enabled | Calculate L2 and publish `acoustic.dynamics` subject to validity / overflow rules; requires Hub support |
| Disabled | Enabled | **Forbidden by Kconfig `depends on`.** Not a supported product state. If a hand-edited sdkconfig somehow sets it, firmware shall still treat publication as inactive (`dynamics` null) and should log a config error — but correct trees prevent the combination |

Requires L1 enabled (shared FFT loop). Requires L0 mic path for PCM lifecycle.

---

## Summary checklists

### Normative features

- [x] `spectral_flux` — absolute bin diffs, current-frame energy normalize, band 80–7600 Hz, inside L1 loop  
- [x] `envelope_cv` — population σ/μ of raw-frame RMS, hop 1024, no overlap  
- [x] `dominant_delta_hz` — raw \|Δ dominant\| on continuity-valid pairs only; not motion/Doppler; no flatness gate  

### Counts

- [x] `window_count` = general L2-eligible associated windows  
- [x] `invalid_window_count` = general eligibility failures only  
- [x] Per-feature `valid_count` (pairs for delta); feature failures do not inflate `invalid_window_count`  
- [x] On overflow: Option A — `history_overflow: true`, feature metrics cleared, counts forced to 0 as **untrusted placeholders**; empty vs overflow distinguished by the flag only  

### JSON / Hub / Kconfig

- [x] `acoustic.dynamics` sibling; independent `schema: "l2"` version  
- [x] Hub optional wiring + validation expectations documented  
- [x] `DBMAP_EAR_OBS_L2_PUBLISH` **depends on** `DBMAP_EAR_OBS_L2` (build-time); compute≠publish  
- [x] Null mean/stdev rules; empty/invalid/first-interval/feature-mismatch/overflow examples  

### Validity / reset

- [x] Clip/invalid/gap/episode/boot/overflow breaks  
- [x] Preserve continuity across ordinary Observation boundaries (D2)  
- [x] Overflow clears **all** L2 metrics (D3)  
- [x] \(T_{\mathrm{tol}} = 100\) ms (D1); EAR-001 validation procedure documented (unvalidated on hardware)  

### Host tests

- [x] Matrix H1–H12; AM multi-phase without universal CV threshold; boundary continuity per D2  
- [x] Feature-specific golden comparison predicates; provisional ε table; near-zero uses absolute tolerance  

### Performance

- [x] Measured L0/L1 baseline vs estimated L2 cost; ≤15 ms target / 30 ms ceiling; overwrite=0; no CPU/PM changes  
- [x] No claim that L2 cost has been measured on device  

### Compatibility

- [x] Hub optional field before publish; protocol v1; classification untouched; doc stays under `docs/diagnostics/` (D4)  

---

## Items still needing human approval or hardware evidence

Contract text for the former “five remaining questions” is now written. The following still need **approval or measurement** before treating them as frozen production facts:

| Item | Status in this document | Still needs |
| --- | --- | --- |
| Golden-vector ε | Feature-specific predicates + **PROVISIONAL** numbers | Host golden PCM run + human freeze of ε table |
| Kconfig names | Proposed symbols + **`DBMAP_EAR_OBS_L2_PUBLISH depends on DBMAP_EAR_OBS_L2`** | Confirm symbol spelling when firmware work is authorized |
| Hub wiring | Proposed `acoustic.dynamics` optional property + validation rules | Implement/accept on Hub (out of scope here); review against Pydantic/`extra=forbid` |
| Overflow counts | **Option A retained:** zeros + `history_overflow`; zeros untrusted when flag set; empty≠overflow | Implementation + Hub consumers must check the flag |
| ±100 ms tolerance | **D1 decided**; validation procedure specified; **not measured on EAR-001** | Execute § EAR-001 continuity-tolerance validation; human review if jitter exceeds ±100 ms |

D1–D5 remain resolved as previously decided. L2 remains **unimplemented** and **unmeasured** on device.

---

## Explicit non-goals (repeat)

No source classification; no episode acoustic rollups; no autocorrelation / HNR / harmonics / tonality features; no flatness gating of dominant delta; no extra features beyond the three named; no L0/L1 semantic edits; no CPU frequency or power-management changes; no implementation in this documentation step.
