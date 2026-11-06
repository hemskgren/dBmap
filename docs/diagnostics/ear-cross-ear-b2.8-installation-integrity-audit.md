# B2.8 – Installation Integrity, Human Error & Sensor Placement Audit

**Nature:** audit + isolated diagnostic experiments only. No production, API, schema, association, localization-algorithm, or Phase A changes.
**Method:** synthesize Observations with **true** EAR geometry; corrupt **only** Hub-style installation metadata used by association/localization. Ground truth is **post-hoc only**.

Local reproduction artifacts (gitignored under `shared/results/ear-tests/**`): `b2.8-installation-audit/results.json`, `run_b28_experiments.py`.

---

## 1. Current implementation facts

| Parameter | Representation / units | Validation | Default | How set | Uncertainty represented? | Used in calculations? |
| --- | --- | --- | --- | --- | --- | --- |
| `latitude` | WGS84 decimal degrees (`float`) | API `[-90, 90]` | none (required on PUT) | Manual / phone / simulator | **No** | **Yes** — EAR origin for LOB geometry and association distance |
| `longitude` | WGS84 decimal degrees (`float`) | API `[-180, 180]` | none (required) | Manual / phone / simulator | **No** | **Yes** |
| `height_m` | metres, optional, `ge=0` | non-negative | `null` | Manual / inferred | Companion `height_accuracy_m` exists but unused | **No** in association/localization (display-only as “mount height”) |
| `height_accuracy_m` | metres, optional | `ge=0` | `null` | Manual | is itself | **No** |
| `floor` | int, optional | type only | `null` | Manual | No | **No** |
| `mount_type` / `environment` | string ≤64 | length | `null` | Manual | No | **No** |
| `orientation_deg` | degrees `[0,360)`, clockwise from **geographic true north** to Ear reference axis | API range | `null` (optional in Hub) | Manual / simulator | **No** | **Yes** — `geographic = (orientation_deg + node_relative_bearing) % 360`; missing → LOB excluded |
| Observation `bearing.deg` | `[0,360)`, **node-relative**, `reference: "node"` only | schema | n/a | Firmware / simulator | `confidence ∈ [0,1]` **not** mapped to degrees | **Yes** |

### Verified semantic facts

- Localization is **2D** east–north tangent-plane geometry. **No vertical localization.**
- True north (not magnetic) is the documented and coded orientation reference.
- Simulator: `node_bearing = (geographic_bearing − orientation_deg) % 360`.
- **No AGL vs AMSL enum.** `height_m ≥ 0` alone does not define the vertical datum.
- No explicit decimal-place validation; floats survive JSON/SQLite without stated precision policy.
- B2.3/B2.4 localization tests assume installation metadata matches simulator truth.

---

## 2. Baseline results (unperturbed)

| Scenario | Event? | Status | Post-hoc err (m) | Network sens. (m) | Member EARs | LOO critical | LOO redundant |
| --- | --- | --- | --- | --- | --- | --- | --- |
| dense_stationary | True | useful | 2.00 | 554.51 | ['SIM-EAR-001', 'SIM-EAR-002', 'SIM-EAR-003'] | ['SIM-EAR-001', 'SIM-EAR-002', 'SIM-EAR-003'] | [] |
| dense_four_ear | True | useful | 240.00 | 916.14 | ['SIM-EAR-001', 'SIM-EAR-002', 'SIM-EAR-003'] | ['SIM-EAR-001', 'SIM-EAR-002', 'SIM-EAR-003'] | [] |
| dense_poor_collinear | True | useful | 192.00 | 566.95 | ['SIM-EAR-001', 'SIM-EAR-002', 'SIM-EAR-003'] | ['SIM-EAR-001', 'SIM-EAR-002', 'SIM-EAR-003'] | [] |
| stress_far_source | True | unavailable | — | 6346.37 | ['SIM-EAR-001', 'SIM-EAR-002', 'SIM-EAR-003'] | [] | [] |

Notes:
- `dense_stationary` is the clean focus case (~2 m post-hoc error, `useful`).
- `dense_four_ear` still has **three** Event members (B2.5 promote-freeze); SIM-EAR-004 is not in the Event, so “redundant fourth EAR” install errors are invisible to localization.
- Large post-hoc errors on moving/poor-geometry baselines are **not** installation errors; do not over-interpret absolute metres there—use **Δerr / estimate shift / status change** vs that scenario’s own baseline.
- `stress_far_source` baseline is already `unavailable` (B2.3 withhold).

---

## 3. Coordinate precision and displacement

Focus: `dense_stationary`, corrupt **one** EAR (`SIM-EAR-001`).

At ~59.34°N: 1×10⁻⁵ ° lat ≈ 1.11 m north; 1×10⁻⁵ ° lon ≈ 0.57 m east. Consumer phone GPS is often **metres-class CEP**, unrelated to how many decimals a form stores.

### 3.1 Decimal-place truncation (storage precision ≠ accuracy)

| Decimals | Physical EAR origin shift (m) | Status | Err (m) | Δerr (m) | Est. shift (m) | Detectable inconsistency? | Identifiable EAR? |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 5883.48 | weak | 2.00 | 0.00 | 0.01 | True | False |
| 2 | 608.20 | weak | 2.00 | 0.00 | 0.01 | True | False |
| 3 | 59.38 | useful | 2.07 | 0.07 | 0.07 | False | False |
| 4 | 3.78 | useful | 2.00 | 0.00 | 0.00 | False | False |
| 5 | 0.44 | useful | 2.00 | 0.00 | 0.00 | False | False |
| 6 | 0.00 | useful | 2.00 | 0.00 | 0.00 | False | False |
| 7 | 0.00 | useful | 2.00 | 0.00 | 0.00 | False | False |

**Findings:**
- **≤2 decimals:** EAR origin wrong by hundreds of metres → kilometres; association breaks (`event_lost`). That is detectable as association failure, not as “EAR-001 has bad coordinates.”
- **3 decimals:** ~59 m origin error; Event still forms; estimate barely moves (~0.07 m) in this geometry — **silent** for status-based detectors.
- **4 decimals:** ~3.8 m origin error; estimate shift ~0.005 m — effectively invisible operationally.
- **5 decimals:** ~0.45 m origin error; negligible.
- **≥6 decimals:** truncation error **0** for this fixture’s stored values → **minimum useful stored precision for these scenarios is 6 decimals**, while remembering that **installation accuracy** may still be only metres if the pin came from a phone.

### 3.2 Physical displacement

| Perturbation | Status | Err (m) | Δerr (m) | Est. shift (m) | Detectable inconsist.? | ID’able? |
| --- | --- | --- | --- | --- | --- | --- |
| east_0.1m | useful | 2.07 | 0.07 | 0.07 | False | False |
| east_0.5m | useful | 2.33 | 0.33 | 0.33 | False | False |
| east_1m | useful | 2.67 | 0.67 | 0.67 | False | False |
| east_3m | useful | 4.00 | 2.00 | 2.00 | False | False |
| east_5m | useful | 5.33 | 3.33 | 3.33 | False | False |
| east_10m | useful | 8.67 | 6.67 | 6.67 | False | False |
| east_25m | useful | 18.67 | 16.67 | 16.67 | False | False |
| north_1m | useful | 2.00 | -0.00 | 0.00 | False | False |
| north_5m | useful | 1.99 | -0.01 | 0.01 | False | False |
| north_10m | useful | 1.99 | -0.01 | 0.01 | False | False |
| north_25m | weak | 2.00 | 0.00 | 0.01 | True | False |

**Findings:**
- Easting errors roughly **transfer into the fix** (~0.67× to ~0.67× of EAR shift for 1–25 m east in this triangle)—post-hoc measurable, **not** status-flagged until association breaks (north 25 m triggered `event_lost`).
- Northing 1–10 m barely changes GT error (geometry-dependent cancellation); **do not generalize** “north is safe.”
- **None** of the metre-scale east shifts were identifiable to the faulty EAR via LOO heuristics.
- Cross-scenario 5 m east: also **no** status-based detection on four-ear / collinear / far-source.

---

## 4. Altitude semantics and sensitivity

**Verified:** `height_m`, `height_accuracy_m`, and `floor` are **not** read by `cross_ear_localization` or bearing geometry.

| Case | Status | Err (m) | Δerr | Est. shift | Detectable? |
| --- | --- | --- | --- | --- | --- |
| height_m_0 | useful | 2.00 | 0.00 | 0.00 | False |
| height_m_1 | useful | 2.00 | 0.00 | 0.00 | False |
| height_m_5 | useful | 2.00 | 0.00 | 0.00 | False |
| height_m_10 | useful | 2.00 | 0.00 | 0.00 | False |
| height_m_20 | useful | 2.00 | 0.00 | 0.00 | False |
| height_m_50 | useful | 2.00 | 0.00 | 0.00 | False |
| height_m_74 | useful | 2.00 | 0.00 | 0.00 | False |
| height_m_155 | useful | 2.00 | 0.00 | 0.00 | False |
| mixed_refs_agl_3m | useful | 2.00 | 0.00 | 0.00 | False |
| mixed_refs_amsled_confused_as_agl_74m | useful | 2.00 | 0.00 | 0.00 | False |
| mixed_refs_amsled_confused_as_agl_155m | useful | 2.00 | 0.00 | 0.00 | False |

**Can the current system detect an AGL/AMSL mistake?** **No.** Altitude is unused; mixed “AGL-like” and “AMSL-confused” values across EARs produced **zero** change in status, error, or estimate. There is also no datum field to even *express* the distinction.

**Recommendation (design only):** add an explicit height reference enum (`agl` | `amsl` | `unknown`) and units before any 3D geometry; until then treat `height_m` as non-authoritative placement notes.

---

## 5. Orientation convention and error experiments

**Convention (verified):** clockwise degrees from **true north** to enclosure forward axis; Observation bearings are enclosure-relative. Wrong `orientation_deg` rotates that EAR’s geographic LOB by the same angle.

### 5.1 One-EAR orientation errors — `dense_stationary`

| Error | Status | Err (m) | Δerr (m) | Est. shift (m) | Detectable inconsist.? | ID’able EAR? |
| --- | --- | --- | --- | --- | --- | --- |
| 1° | useful | 1.22 | -0.78 | 3.22 | False | False |
| 3° | useful | 7.68 | 5.68 | 9.68 | False | False |
| 5° | useful | 14.20 | 12.20 | 16.20 | False | False |
| 7° | useful | 20.80 | 18.80 | 22.80 | False | False |
| 10° | useful | 30.96 | 28.96 | 32.96 | False | False |
| 15° | weak | 48.93 | 46.93 | 50.92 | True | False |
| 30° | weak | 136.65 | 134.65 | 138.38 | True | False |
| 45° | weak | 201.01 | 199.01 | 202.74 | True | False |
| 90° | weak | 554.25 | 552.25 | 555.97 | True | False |
| 180° | weak | 2.00 | 0.00 | 0.01 | True | False |

### 5.2 Explicit 3° / 7° / 180° across geometries

| Scenario | Error | Status | Err (m) | Δerr (m) | Est. shift (m) | Detectable? | ID’able? |
| --- | --- | --- | --- | --- | --- | --- | --- |
| dense_stationary | 3° | useful | 7.68 | 5.68 | 9.68 | False | False |
| dense_stationary | 7° | useful | 20.80 | 18.80 | 22.80 | False | False |
| dense_stationary | 180° | weak | 2.00 | 0.00 | 0.01 | True | False |
| dense_four_ear | 3° | useful | 240.24 | 0.24 | 16.78 | False | False |
| dense_four_ear | 7° | useful | 242.47 | 2.47 | 40.82 | False | False |
| dense_four_ear | 180° | useful | 240.00 | -0.00 | 0.00 | False | False |
| dense_poor_collinear | 3° | useful | 199.35 | 7.35 | 24.49 | False | False |
| dense_poor_collinear | 7° | weak | 210.01 | 18.01 | 52.46 | True | False |
| dense_poor_collinear | 180° | weak | 192.00 | 0.00 | 0.02 | True | False |
| stress_far_source | 3° | unavailable | — | — | — | True | False |
| stress_far_source | 7° | unavailable | — | — | — | True | False |
| stress_far_source | 180° | unavailable | — | — | — | True | False |

**Findings:**
- **3° (good triangle):** still `useful`; estimate shifts ~**10 m**; GT error ~8 m — **field-undetectable** with current status/outlier gates, but **material vs B2.3 dense medians**.
- **7°:** still `useful` on good triangle; estimate shifts ~**23 m**; GT error ~21 m — still often **undetectable**, already larger than many published dense errors.
- **15°+:** status demotes to `weak`; large shifts; sometimes association loss.
- **180° reversed enclosure:** often **drops the bad EAR from association** (bearing/geometry incompatible) while the remaining pair still localizes near GT → **small GT error, status/event change**. Detectable as “something wrong,” **not** identifiable as “EAR-001 orientation reversed,” and easy to misread as “localization still accurate.”
- **Poor/collinear geometry:** 7° already flips `useful`→`weak` (more fragile).
- **Independent 3°/−5°/+7° on three EARs:** ~32 m error, still `useful`, **not** flagged.
- **Systematic +10° on all EARs:** for a near-centered source in an equilateral layout, pair intersections disperse but the **consensus can remain near GT** while status softens to `weak`. Shared compass/declination bias can look like mild quality loss without an obvious culprit EAR.

---

## 6. Multi-EAR installation error detection

| Case | Status | Err (m) | Δerr | Outlier pairs | Detectable inconsist.? | ID’able? |
| --- | --- | --- | --- | --- | --- | --- |
| redundant_SIM-EAR-004_orient_+7 | useful | 240.00 | 0.00 | 0 | False | False |
| redundant_SIM-EAR-004_orient_+180 | useful | 240.00 | 0.00 | 0 | False | False |
| critical_SIM-EAR-001_orient_+7 | useful | 242.47 | 2.47 | 0 | False | False |
| critical_SIM-EAR-001_orient_+180 | useful | 240.00 | -0.00 | 0 | False | False |
| two_ears_orient_+15_-15 | weak | 290.78 | 50.78 | 2 | True | False |

**Findings:**
1. **Detect inconsistency:** possible for large / multi-EAR orientation faults (status demotion, more outliers, association loss). **Not** reliable for few-degree or metre-scale coordinate faults.
2. **Identify responsible EAR:** **not achieved** in these runs by LOO heuristics. LOO “critical” on a clean 3-EAR net marks **all** ears critical — useless for blame.
3. **Estimate parameter error:** **not available** in current diagnostics.
4. **Distinguish install error vs true source motion:** **not solved**; both move the estimate similarly.
5. Corrupting SIM-EAR-004 orientation on `dense_four_ear` did **nothing** — that EAR is not an Event member (B2.5 freeze). More EARs ≠ automatic install reference if they never join.

---

## 7. Detectability versus identifiability

| | Small orient (≤7°) | Large orient (≥15°, 180°) | Metre-scale coords | Altitude / AGL↔AMSL | Shared systematic orient |
| --- | --- | --- | --- | --- | --- |
| Post-hoc measurable (with GT) | **Yes** | **Yes** (or association drops EAR) | **Yes** (geometry-dependent) | **No** (unused) | Often **weak** GT signal on centered sources |
| Detect inconsistency (no GT) | Usually **no** | Often **yes** (status/event/outliers) | Usually **no** until huge | **No** | Sometimes soft status only |
| Identify which EAR | **No** | **Unreliable** | **No** | N/A | **No** (all wrong) |
| Estimate the parameter error | **No** | **No** | **No** | N/A | **No** |

Displacement of the estimate alone is **not** detection.

---

## 8. Effects on previous localization tests

B2.3/B2.4 figures assume perfect installation metadata. Under that assumption, dense useful medians were order **~1–10 m**. This audit shows:
- **3–7° one-EAR orientation error** can move a good-triangle fix by **~10–23 m** without leaving `useful`.
- Those installation errors are therefore **comparable to or larger than** the sensor/timing imperfections those tests study.
- Prior results remain valid as **algorithm/geometry** baselines, **not** as field accuracy under ordinary install practice.
- Altitude mistakes do not affect those tests (height unused).

---

## 9. Human installation risks

| Risk | Realistic cause | System impact |
| --- | --- | --- |
| Phone GPS pin | Installer uses mobile location | Silent metre-scale EAR origin error |
| Truncated decimals in UI/export | “59.34 looks enough” | Equivalent to tens–thousands of metres if ≤3 decimals |
| Visual orientation, no compass | 3D-printed box “faces the road” | Dominant LOB bias; 3–15° easy |
| 180° enclosure reversal | Unclear which face is 0° | Bad EAR may drop out; remaining fix looks fine |
| Magnetic vs true north | Phone compass without declination | Few-degree systematic bias |
| Unlevel plastic mount | Roll/pitch ignored | Unmodeled (azimuth-only system) |
| AGL vs AMSL / building height | `height_m` unlabeled | No current calc impact; future 3D hazard |
| Copy-pasted site template | Same orientation for every EAR | Shared bias; hard to detect |

---

## 10. Recommended installation requirements (before wider EAR deployment)

1. Molded **true-north / forward-axis marks** on the enclosure; written clockwise-from-true-north convention.
2. **Require** `orientation_deg` for any geographic localization (already effectively required).
3. Dense (~≤500 m) sites: target **≤1–2 m** map/survey coordinates; treat phone GPS as provisional only.
4. Persist coordinates with **≥6 decimal degrees** in API/UI, and store **separate accuracy metadata** (storage precision ≠ accuracy).
5. Add **height reference** semantics (`agl`/`amsl`/`unknown`) before any vertical use; do not imply 3D competence today.
6. Capture **orientation method/confidence** (survey / compass / phone / visual guess).
7. Plan phone-assisted calibration / QR alignment as product work — not claimed as available now.
8. **Do not** claim that “multiple EARs will catch install mistakes” without a dedicated verification protocol.

---

## 11. Recommended future validation and diagnostics

- Surface pair-inconsistency / association dropout as **installation suspicion**, distinct from source motion.
- Installation wizard: map pin + compass heading + confidence + height datum.
- Optional on-site check: known source or surveyed mutual bearings.
- Coordinate tier: `provisional_phone` vs `surveyed`.
- Still **not implemented** in B2.8.

---

## 12. Limitations and untested assumptions

- Metadata corruption after true synthesis; does not model internal array misalignment.
- No real GNSS traces; metre offsets are synthetic.
- Tilt/roll and magnetic declination not simulated.
- Detectability uses existing B2.3/B2.4 signals only.
- Four-EAR membership limited by B2.5 freeze.
- Results are geometry-specific; equilateral centered source understates systematic orientation bias.

---

## Executive summary

### Measurable
- **Orientation** and **horizontal coordinates** materially affect localization (post-hoc).
- Orientation is the highest-leverage human error for LOB geometry.

### Detectable but not identifiable
- Large orientation faults and extreme coordinate truncation can break association or demote status.
- Identifying **which** EAR/parameter is wrong was **not** reliably possible in these experiments.

### Not observable with current measurements
- **AGL/AMSL mistakes** (altitude unused).
- Small orientation (≲7°) and metre-scale coordinate biases that preserve `useful` status.
- Shared systematic orientation offsets on favorable geometries.
- Vertical error generally.

### Minimum guidance before wider deployment
- Enclosure true-north marking + required orientation entry.
- ≥6 decimal coordinate storage **plus** accuracy metadata; metre-class pins for dense nets.
- Explicit height datum labeling; no 3D claims.
- Treat published dense localization accuracy as conditional on correct install metadata.

