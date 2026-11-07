# ABDE Phase 4B — time-varying EAR B bearing bias

Baseline (unchanged): `helicopter_abde_overlap.json`  
Phase 4A (unchanged): `helicopter_abde_b_bearing_bias_p10.json`, `…_p20.json`  
New scenario: `simulator/ear/scenarios/helicopter_abde_b_bearing_bias_time_varying.json`  
ID: `helicopter_abde_b_bearing_bias_time_varying_01`  
Profile: `shared/simulator/ear-profiles/abde.json`

## Simulator support

**No new framework.** Phase 4A’s per-observation `bearing_bias_deg` already applies a deterministic offset to the true node-relative bearing before `% 360`. Phase 4B is the same field with a **different value per report row**:

| Epoch | Authored `bearing_bias_deg` on ear_b |
|---|---:|
| 0 s | 0 |
| 10 s | +5 |
| 20 s | +10 |
| 30 s | +20 |
| 40 s | +5 |

A, D, E omit the field (unbiased). Installs, route, timing, confidence, acoustics unchanged. Bias of `0` is a no-op (loaded events omit `bearing_bias_deg` when zero).

## Generated bearings (time-varying scenario)

Node-relative degrees after bias (offline `load_scenario`):

| t (s) | A | B (bias) | D | E |
|---:|---:|---:|---:|---:|
| 0 | 77.910 | 26.222 (**0**) | 45.926 | 327.045 |
| 10 | 82.069 | 33.094 (**+5**) | 40.337 | 330.555 |
| 20 | 84.105 | 40.228 (**+10**) | 33.644 | 334.326 |
| 30 | 85.311 | 52.676 (**+20**) | 25.728 | 338.351 |
| 40 | 86.107 | 40.500 (**+5**) | 16.607 | 342.614 |

B at t=0 matches the unbiased baseline; A/D/E match the baseline at every epoch.

## Production localization comparison

All four cases: **4** Observations, **1** Event, **Event membership** = A,B,D,E. All four EARs have usable bearings. Mode = `common_epoch`. Geometric sensitivity is an **indicator**, not accuracy.

| Case | Selected epoch | Status | Contributors | Fwd / aligned / consensus | Outliers (B pairs) | Sens (m) | Post-hoc GT error (m) |
|---|---:|---|---|---|---|---:|---:|
| Baseline | **0** | `useful` | A,B,D,E | 6 / 6 / **6** | none | ~7705 | ~1.4 |
| Const +10° | **10** | `weak` | A,D,E | 6 / 6 / **3** | AB, BD, BE | ~5596 | ~0.5 |
| Const +20° | **30** | `weak` | A,D,E | 6 / 6 / **3** | AB, BD, BE | ~7137 | ~0.4 |
| **Time-varying** | **0** | `useful` | A,B,D,E | 6 / 6 / **6** | none | ~7705 | ~1.4 |

Time-varying production output **matches the baseline** because the selector picks **t=0**, where B’s bias is **0°**.

### Why t=0 is selected (existing ranking)

Production ranking (unchanged):  
`ear_count > status(useful>weak>unavailable) > aligned_forward_pair_count > lower network_geometric_sensitivity_m > higher min_intersection_angle_deg > earlier common_epoch_utc`.

Forced single-epoch evaluations of the time-varying Observation set (diagnostic only — not used to pick production):

| Forced epoch | B bias | Status | Contributors | Consensus | B-involving outliers |
|---:|---:|---|---|---:|---|
| 0 | 0° | `useful` | A,B,D,E | 6 | none |
| 10 | +5° | `weak` | A,D,E | 3 | AB, BD, BE |
| 20 | +10° | `weak` | A,D,E | 3 | AB, BD, BE |
| 30 | +20° | `weak` | A,D,E | 3 | AB, BD, BE |
| 40 | +5° | `weak` | A,D,E | 3 | AB, BD, BE |

Only t=0 is `useful`, so it outranks the later `weak` epochs. The selector does **not** prefer the mid-pass epochs with larger B bias.

### Selected bearings at production epoch (time-varying = baseline)

At t=0: A 77.91°, B 26.22°, D 45.93°, E 327.05° (same timestamps `…T14:00:00+00:00`).

## Distinctions

1. **Algorithm output** — membership, contributors, status, published lat/lon, pair/outlier diagnostics.  
2. **Diagnostic quality** — `useful`/`weak`; geometric sensitivity ≠ accuracy.  
3. **Post-hoc GT error** — haversine to source at the **selected** common epoch only after localization.

## Limitation / observed behavior

Phase 4B does **not** force the pipeline to localize at a biased epoch. With an available unbiased epoch (t=0), production prefers it. To stress mid-pass bias under production selection, a follow-on experiment would need a schedule without a clean early epoch (out of scope here — not changing ranking or thresholds).
