# B2.3 – Localization robustness and failure-boundary evaluation

Experimental Cross-EAR localization diagnostics only. Ground truth is post-hoc.
Event association/promotion rules were not changed to improve localization.
No calibrated uncertainty is claimed.

## 1. Files changed

| Path | Change |
| --- | --- |
| `scripts/lib/cross_ear_localization.py` | Intersection-angle gates, multi-ray outlier marking, near-window intra-Ear conflict detection, position withholding, extended sensor-imperfection helpers |
| `scripts/eval_dense_sparse_localization.py` | B2.3 stress scenarios + bearing-outlier imperfections; geometry-class summaries (not pooled) |
| `scripts/hub_event_track.py` | Report LOB angles, outliers, withheld position |
| `docs/simulator.md` | Document stress scenarios |
| `tests/scripts/test_cross_ear_localization_robustness.py` | Deterministic failure-boundary tests |
| `simulator/ear/scenarios/cross_ear_stress_*.json` | Six new stress fixtures |

B2.2 control scenarios (`cross_ear_dense_*`, `cross_ear_event_pass`) and Event promotion thresholds were left unchanged.

## 2. Scenarios added

| Scenario | Geometry class | Intent |
| --- | --- | --- |
| `cross_ear_stress_nearly_parallel.json` | nearly_parallel | Tight ~160 m baseline; source ~2.5 km north |
| `cross_ear_stress_near_baseline.json` | near_baseline | Collinear EARs; source ~15 m off baseline |
| `cross_ear_stress_along_baseline.json` | along_baseline | Collinear EARs; source travels along baseline |
| `cross_ear_stress_far_source.json` | far_source | Good ~480 m triangle; source ~3 km away |
| `cross_ear_stress_poor_distribution.json` | poor_distribution | Two EARs clustered (~60 m); third ~450 m offset |
| `cross_ear_stress_inconsistent_bearing.json` | bearing_outlier | Good triangle control for bias / drop / conflict |

Controls retained: good triangle (moving + stationary), collinear poor geometry, four-EAR, sparse B1.

## 3. Geometry failure cases discovered

| Case | Observation |
| --- | --- |
| Nearly parallel LOBs | Best intersection angle ~3–4°. Position **withheld** (`unavailable`). Events still form when association holds; large noise can make bearings association-incompatible (`behind_*`) and prevent Event creation — that is association evidence, not localization. |
| Far source ≫ EAR spacing | Best angle ~9°. Clean/noise cases **withheld**. Previously, a single acute forward pair after noise could look “weak” with ~800–1150 m error despite near-zero residuals — now withheld. |
| Along-baseline motion | Often `weak` (best angle ~18°). With ≥1° noise, association or multi-ray consistency frequently fails / withholds. |
| Near-baseline source | Still often geometrically workable in this fixture (`useful`/`weak`, low error); not automatically fatal. |
| Poor EAR distribution | Can remain `useful` when the distant Ear still provides ≥20° crossings; clustering alone is not a withhold trigger. |
| Collinear control (B2.2) | Remains estimable for a north-of-line track; status mixes `useful`/`weak`. Collinearity ≠ automatic refuse when LOB angles are adequate. |

## 4. Outlier handling and diagnostics

- **Intersection angle**: `best < 10°` → withhold; `best < 20°` → at most `weak`; single forward pair with `best < 20°` → withhold.
- **Multi-ray outliers**: pair locus >150 m from median of other loci flagged; excluded from consensus; diagnostics retain the pair; status ≤ `weak`.
- **Extreme raw spread** with no inliers → withhold.
- **Intra-Ear conflict**: near-window (≤5 s) bearing disagreement ≥15° → cannot be `useful` (full-episode span is not used — that conflates legitimate motion).
- **Sensor helpers**: deterministic `ear_bearing_bias_deg`, `drop_bearing_node_ids`, `conflicting_ear_bias_deg` (seeded).
- **+25° bias** on one Ear: Event kept; localization `weak`; ~59 m shift vs clean; pair residuals stay tiny → residuals are not accuracy.
- **+90° wrong bearing**: association marks contradictory geometry → **no Event** (association gate, not localization “useful”).
- **Missing bearings** on one Ear: Event still created; localization `weak` from remaining pairs.
- **Calibrated uncertainty**: still always `None`.

## 5. Dense and sparse results (separate)

Evaluator: `./scripts/eval_dense_sparse_localization.py` (GT post-hoc only).

### Dense (controls + stress; do not pool with sparse)

- Located estimates: median error **~1.5 m** among cases that publish a position (stress withholds remove the previous km-scale outliers from the pool).
- Control medians (aspirational bands ≤50/100/250 m):

| Scenario | median_err | notes |
| --- | --- | --- |
| dense_stationary | ~3.0 m | baseline `useful` |
| dense_good_moving | ~10.3 m | baseline `useful`; timing/noise → `weak` |
| dense_four_ear | ~4.9 m | mostly `useful` |
| dense_poor_collinear | ~5.6 m | `useful`/`weak` |

### Sparse (B1 stress only)

| Scenario | median_err | notes |
| --- | --- | --- |
| sparse_b1_moving | ~18.5 m | consistently `weak` (timing/Δt); noise_5deg withheld |

Do not average dense+sparse into one accuracy figure.

## 6. Cases where localization was withheld

- **nearly_parallel**: all associating runs → `unavailable`, no estimate.
- **far_source**: all runs → `unavailable`, no estimate.
- **along_baseline** + noise: often unavailable / no Event.
- **sparse noise_5deg**: multi-ray disagreement → withheld.
- **wrong_ear_b_+90deg**: no Event (association incompatible), hence no localization.

## 7. Can the system identify unreliable geometry?

**Yes, within heuristic limits.**

It can refuse or demote based on LOB intersection angle, multi-ray locus disagreement, near-window bearing conflict, and timing gates — without treating two-ray residuals as proof of accuracy. Events remain independent of localization quality when association evidence is sufficient.

It does **not** yet provide calibrated σ, full DOP/sensitivity maps, or automated EAR-layout recommendations.

## 8. Remaining limitations and recommended next step

**Limitations**

- Gates are diagnostic thresholds, not a statistical error model.
- Near-baseline / poor-distribution fixtures can still look accurate under ideal scripted bearings; real noise + layout search would stress them harder.
- Outlier rejection uses a median-of-pairs heuristic; three-Ear graphs have limited redundancy.
- Extreme bearing contradictions are handled at association time (`behind_*`), which removes the Event entirely — desirable for association integrity, but distinct from “Event with unavailable localization.”

**Recommended next step**

B2.4: quantitative sensitivity / dilution reporting (e.g. approximate range amplification ≈ baseline / sin(θ) per pair) and optional layout-aware deployment checks — still without claiming calibrated uncertainty, and still without feeding GT into association or position math.

## Test evidence

- `pytest tests/scripts/test_cross_ear_localization_robustness.py tests/scripts/test_hub_event_track.py` → 54 passed
- `pytest tests/simulator/ear/test_simulate_scenario.py` → 20 passed
- `scripts/eval_dense_sparse_localization.py` → completed (dense/sparse/geometry-class summaries)
