# Helicopter offline B2 golden path (Phase 1)

Reproducible offline path from the existing
`simulator/ear/scenarios/helicopter_east_west.json` scenario through restored
B2 analysis. No MQTT, Hub API, Analysis container, or map changes.

## Why `independent_detection`

Scripted authored points use exclusive Ear windows (B → C → A) with no
simultaneous multi-EAR timestamps. Localization needs overlapping bearings.

`independent_detection` evaluates every Ear every shared time step and emits
START/PERIODIC/END reports inside each Ear’s range-gate episode. With
`--max-range-m 4000` this scenario produces a sustained three-Ear overlap
(~86–159 s logical).

## Exact reproduction

From the repository root, using the Hub bootstrap venv:

```bash
hub/bootstrap/.venv/bin/python scripts/helicopter_offline_b2_golden_path.py
```

Focused tests:

```bash
hub/bootstrap/.venv/bin/pytest tests/scripts/test_helicopter_offline_b2_golden_path.py -q
```

## Deterministic parameters

| Knob | Value |
|------|-------|
| Scenario | `simulator/ear/scenarios/helicopter_east_west.json` (unchanged) |
| Mode | `independent_detection` |
| `max_range_m` | `4000` |
| `time_step_seconds` | `1` |
| `observation_interval_seconds` | `5` |
| `reference_level_dbfs` | `-30` |
| `reference_distance_m` | `1000` |
| `path_loss_exponent` | `1` |
| Scenario start UTC | `2026-11-07T12:00:00+00:00` |
| `allow_long_range_sim_ears` | `true` (SIM baselines are multi-km) |
| `min_ears_for_event` | `3` |

Constants live in `scripts/lib/helicopter_offline_golden_path.py`.

## Ground-truth separation

- Trajectory comes from scenario `source.movement` via
  `scripts/lib/scenario_ground_truth.py`.
- GT is **not** written into Observation `spatial.position`.
- GT is **not** passed to `analyze()` / association / localization.
- Post-hoc only: `evaluate_against_ground_truth` after B2 results exist.
- `classification.hints.simulated_source_id` may appear in reports as simulator
  metadata; association policy ignores it (`ignored_for_association_and_localization`).

## Observation lifecycle note

Independent-detection episodes now mint one opaque `observation_id` per Ear
detection episode (shared by START/PERIODIC/END). Without that, each emitted
report became a separate Observation and B2 could not promote a three-Ear
Event for this scenario. IDs are never derived from `ground_truth_id`.

## Expected shape (sanity)

Typical offline collect with the parameters above:

- ~102 MQTT-shaped reports
- 3 unique Observation IDs (one per Ear episode)
- 1 Promoted Event with membership `SIM-EAR-001/002/003`
- Localization may be `useful` or `weak`; contributors may be a subset of
  membership
- GT position error is reported when a position is published; not tuned for a
  target figure

## Out of scope (later phases)

- Analysis scratch serialization / map overlays
- Live MQTT → Hub ingestion path
- Hub Observation API changes
- Migrating B2 into the Analysis container
