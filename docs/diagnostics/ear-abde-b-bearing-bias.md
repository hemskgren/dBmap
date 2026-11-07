# ABDE Phase 4A — controlled EAR B bearing bias

Baseline (unchanged): `simulator/ear/scenarios/helicopter_abde_overlap.json`  
Variants:

| File | Scenario ID | Bias |
|---|---|---:|
| `helicopter_abde_b_bearing_bias_p10.json` | `helicopter_abde_b_bearing_bias_p10_01` | **+10°** on ear_b only |
| `helicopter_abde_b_bearing_bias_p20.json` | `helicopter_abde_b_bearing_bias_p20_01` | **+20°** on ear_b only |

Profile: `shared/simulator/ear-profiles/abde.json`  
Simulator mechanism: optional observation field `bearing_bias_deg` applied after true geometry → node-relative bearing, then `% 360`. Installs, orientation, route, timing, confidence, and other EARs unchanged. Bias is **measurement error**, not missing bearing.

## Offline comparison (production analyze path; no MQTT)

Shared for all three runs:

- 20 reports → **4** complete Observations (5 reports each)
- **1** Event with **Event membership** = A,B,D,E (4)
- All four EARs retain usable bearing samples (`spatial.bearing` present)
- `bearing_selection_mode=common_epoch`
- Geometric sensitivity below is an **indicator only**, not position accuracy

| | Baseline | B +10° | B +20° |
|---|---|---|---|
| Common epoch (relative) | t=**0** s | t=**10** s | t=**30** s |
| Common-epoch excluded EARs | none | none | none |
| Forward / aligned / consensus pairs | 6 / 6 / **6** | 6 / 6 / **3** | 6 / 6 / **3** |
| Outlier pairs | none | AB, BD, BE | AB, BD, BE |
| **Localization contributors** | A,B,D,E (**4**) | A,D,E (**3**) | A,D,E (**3**) |
| Localization status | **`useful`** | **`weak`** | **`weak`** |
| Network geometric sensitivity (m) | ~7705 | ~5596 | ~7137 |
| Post-hoc GT error at selected epoch (m) | ~1.4 | ~0.5 | ~0.4 |
| Within sparse ±250 m band (post-hoc) | yes | yes | yes |

### Selected bearings at the chosen common epoch

Node-relative degrees at the epoch timestamp (algorithm input — includes B’s bias in the variants):

| EAR | Baseline t=0 | +10° @ t=10 | +20° @ t=30 |
|---|---:|---:|---:|
| A (SIM-EAR-001) | 77.91 | 82.07 | 85.31 |
| B (SIM-EAR-002) | 26.22 | 38.09 (= unbiased≈28.09 **+10**) | 52.68 (= unbiased≈32.68 **+20**) |
| D (SIM-EAR-004) | 45.93 | 40.34 | 25.73 |
| E (SIM-EAR-005) | 327.05 | 330.56 | 338.35 |

### Interpretation (do not over-claim)

- **Event membership ≠ localization contribution.** B remains an Event member with bearings; consensus drops pairs involving B as outliers.
- The biased EAR is **not** dropped from common-epoch participation; it is present in selected bearings and then rejected via **outlier pair** logic.
- Status falls from `useful` → `weak` under existing quality rules (not a new threshold).
- Epoch ranking shifts (+10 → t=10, +20 → t=30) under the existing common-epoch ranking rule; this is observed behavior, not a design change.
- Post-hoc GT error stays small because the published estimate is driven by the **ADE consensus subset** after B’s pairs are outliers. That is **not** proof of calibrated accuracy; sensitivity remains thousands of metres as a geometry indicator.

## Explicit distinctions

1. **Algorithm output** — Event membership, contributors, status, published lat/lon, pair/outlier diagnostics.
2. **Diagnostic quality** — `useful` / `weak`; geometric sensitivity ≠ accuracy.
3. **Post-hoc GT error** — haversine to source position at the selected common epoch only after localization; GT never selects bearings or thresholds.

Phase 4B time-varying schedule (0/+5/+10/+20/+5 on B):  
`helicopter_abde_b_bearing_bias_time_varying.json` — see `ear-abde-b-bearing-bias-time-varying.md`.
