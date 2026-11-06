# B2.4 – Localization sensitivity, geometric dilution and EAR layout

Experimental diagnostics only. Builds on B2.3 quality gates without changing
association, Event promotion, or claiming calibrated position uncertainty.
Ground truth remains post-hoc.

## 1. Files changed and why

| Path | Why |
| --- | --- |
| `scripts/lib/cross_ear_localization.py` | Add `baseline/sin(θ)` geometric sensitivity indicator, ±bearing perturbation analysis, leave-one-EAR-out layout analysis; attach per-pair dilution to localization details |
| `scripts/eval_dense_sparse_localization.py` | Report sensitivity / layout diagnostics on selected baselines; show dilution in row output; keep dense/sparse separate |
| `scripts/hub_event_track.py` | Print dilution + baseline per pair and network sensitivity indicator |
| `docs/simulator.md` | Document B2.4 evaluator diagnostics |
| `tests/scripts/test_cross_ear_localization_sensitivity.py` | Deterministic B2.4 coverage |
| `docs/diagnostics/ear-cross-ear-b2.4-localization-sensitivity.md` | This report |

No Hub API / DB / Track / promotion-rule changes. B2.2–B2.3 scenarios and gates preserved.

## 2. New diagnostics and definitions

### Geometric dilution / sensitivity indicator

\[
\text{geometric\_dilution\_m} = \frac{\text{baseline\_m}}{\sin(\theta)}
\]

- \(\theta\): acute LOB intersection angle (degrees → radians for \(\sin\)).
- Units: metres (same as baseline).
- Degenerate angles (\(\sin\theta < 10^{-6}\)) → `inf` / reported as `degenerate`.
- **Not** a confidence radius, accuracy claim, or calibrated \(\sigma\).
- Limitations: ignores range along the LOB, multi-ray fusion, timing error, and measurement models; near-zero residuals remain unrelated.

Network summary: **max** dilution among consensus pairs (after B2.3 outlier exclusion).

### Bearing sensitivity

For each contributing Ear and each of \(\pm1^\circ,\pm3^\circ,\pm5^\circ\):

- Re-run localization only (association not re-run).
- Compare perturbed estimate to the **unperturbed baseline**.
- Record status, withhold flag, displacement (m), best angle, network sensitivity.
- \(+\delta\) and \(-\delta\) are both measured (asymmetry is expected).

### Leave-one-EAR-out layout

For each Ear removed:

- Re-estimate on the remaining set.
- Flag **material geometry loss** if status demotes, position becomes withheld, displacement \(> 25\) m, or dilution worsens substantially.
- Ears without material loss may be listed as **redundant** (possible duplicate information).
- Four-Ear is **not** assumed better than three-Ear.

## 3. Scenario results (dense vs sparse separate)

Evaluator: `./scripts/eval_dense_sparse_localization.py`.

### Dense (controls + stress; not pooled with sparse)

| Scenario | Baseline loc | Network sens | Notes |
| --- | --- | --- | --- |
| dense_stationary | useful | ~555 m | Best angles ~60°; max ±5° disp ~16 m |
| dense_good_moving | useful | ~1948 m | One pair acute (~14°) drives high max dilution |
| dense_four_ear | useful | ~916 m | Event often associates 3 of 4 Ears (see §8) |
| dense_poor_collinear | useful/weak | (row sens varies) | B2.2 control retained |
| stress_nearly_parallel | unavailable | ~2508 m | Withheld; best angle ~3.7° |
| stress_far_source | unavailable | ~6346 m | Withheld; best angle ~8.7° |
| stress_poor_distribution | useful/weak | (moderate) | B2.4 layout diagnostics included |

Dense located-case median errors remain on the order of metres to tens of metres for controls (same B2.3 character); stress withholds keep km-scale outliers out of accuracy pools.

### Sparse (B1 only)

| Scenario | Baseline loc | Network sens | Notes |
| --- | --- | --- | --- |
| sparse_b1_moving | weak | ~2329 m | Timing-limited; ±5° often withholds; median err ~18.5 m among located cases |

## 4. Bearing sensitivity results

| Geometry | Max disp @ ±1/3/5° (order) | Observation |
| --- | --- | --- |
| Dense stationary | ~3 / 10 / 16 m | Near, fairly symmetric |
| Dense moving | up to ~32 m @ 5° | Asymmetry present; one ±5° trial demotes useful→weak |
| Four-Ear (3 associated) | up to ~29 m @ 5° | Remains useful under tested ±5° |
| Sparse B1 | up to ~162 m before withhold | ±5° often `unavailable`; +/− not symmetric |
| Nearly parallel / far source | n/a (baseline withheld) | Perturbations stay withheld |

Symmetric ±δ do **not** always yield equal displacement (explicitly reported).

## 5. Three-EAR versus four-EAR

- Dense stationary / moving **three-Ear** triangles: every Ear is critical under leave-one-out (drop to two Ears → weak / withhold).
- Dense four-Ear **scenario** still frequently promotes an Event with **three** Ears only under current association (ear_d present in publishes but not always in the Event). Explicit four-Ear localization (all assembled Observations) supports leave-one-out with 3-Ear remainders; four-Ear is not automatically better.
- Synthetic near-duplicate fourth Ear: removable without withhold — additional Ear need not improve the solution.

## 6. Leave-one-EAR-out findings

- **Good triangle**: all three Ears critical (status demotion or withhold).
- **Sparse B1**: SIM-EAR-002 classified **redundant** because its pairs are B2.3 outliers excluded from consensus — perturbing that Ear by ±5° yields **0 m** displacement. Layout diagnostic correctly surfaces “present but not contributing to consensus.”
- **Nearly parallel / far source**: full solution already withheld; leave-one-out stays withheld (no “critical” improvement path).

## 7. Positions withheld

Unchanged B2.3 behavior, confirmed under B2.4 diagnostics:

- Nearly parallel (all associating baselines).
- Far source (all runs).
- Sparse noise_5deg / several ±5° sensitivity trials.
- Along-baseline under noise.
- Removing the Ear that provides the only adequate crossing from dense_good_moving (SIM-EAR-003 in leave-one-out).

## 8. Limitations / unexpected findings

1. **`baseline/sinθ` can look large even for useful dense moving** when one acute pair exists (~14°) while other pairs are healthy (~86°). Network max is conservative; pair-level reporting matters.
2. **Four-Ear Event membership** often excludes one Ear under current association — a layout/association issue, not fixed in B2.4 (promotion rules unchanged).
3. **Outlier-excluded Ears** can look “redundant” and insensitive to bearing perturbation even though they still appear in association — diagnostics must be read with outlier flags.
4. Still **no calibrated σ**; sensitivity displacements are simulation diagnostics, not field validation.
5. Sensitivity analysis does not re-run association (by design); extreme perturbations that would have failed association gates are therefore not represented as “no Event.”

## 9. Test results

```text
pytest tests/scripts/test_cross_ear_localization_sensitivity.py \
       tests/scripts/test_cross_ear_localization_robustness.py \
       tests/scripts/test_hub_event_track.py \
       tests/simulator/ear/test_simulate_scenario.py
→ 85 passed
```

Evaluator completed successfully with B2.4 sections.

## 10. B2.4 completeness

**B2.4 is complete** for the requested experimental scope:

- Bearing sensitivity ±1/3/5° with displacement vs baseline
- Geometric dilution indicator with degenerate handling
- Three- vs four-Ear and leave-one-out layout diagnostics
- Dense/sparse kept separate; GT post-hoc; association/promotion unchanged
- B2.3 withhold gates still enforced

**Optional follow-ups (not blocking):** improve four-Ear association completeness; pair-weighted (not max) network sensitivity; deployment-time layout advisor using dilution maps without GT.
