# Temporal localization diagnostics (Phase 5A)

Observational diagnostic that evaluates every eligible common epoch for an
existing Event using the production localization pipeline.

## What it shows

For one promoted Event, the diagnostic lists each discovered common epoch and
reports the `LocalizationResult` that production geometry/consensus/quality
logic would produce if that epoch were selected:

- participating / excluded EARs and exclusion reasons
- selected bearing per EAR (and source report timestamp)
- forward / time-aligned / consensus pair counts
- outlier pairs, contributors, status, position (if published)
- geometric sensitivity indicator

It does **not** change Event association, membership, production epoch ranking,
or the published `event.localization`.

## Production vs diagnostic rows

| | Production | Temporal diagnostic |
| --- | --- | --- |
| Purpose | Publish one Event localization | Observe all eligible epochs |
| Epoch policy | Rank candidates; keep the best | Evaluate every discovered candidate |
| Geometry / consensus / quality | `_estimate_localization_pairwise` | Same (via `estimate_localization_at_epoch`) |
| Stored on Event | `event.localization` | Separate report object only |

Shared entry point: `estimate_localization_at_epoch(...)` in
`scripts/lib/cross_ear_localization.py`. That helper restricts members to the
requested epoch, runs `_estimate_localization_pairwise`, then applies the same
common-epoch finishing used by production (metadata + full-history intra-Ear
conflict demotion).

Production `estimate_localization(..., bearing_selection="auto")` ranks raw
common-epoch trials (restriction + pairwise only — status before full-history
finishing), then finishes the selected winner through
`estimate_localization_at_epoch`. Ranking therefore stays identical to the
pre-5A path. Diagnostic rows call `estimate_localization_at_epoch` directly for
every discovered epoch and never assign the result back onto the Event.

Pairwise fallback remains a complete alternate mode when no common epoch
exists. It is never mixed with common-epoch selection inside one result.

## Geometric sensitivity is not position accuracy

`network_geometric_sensitivity_m` is `baseline / sin(θ)` for consensus pairs.
Larger values mean the intersection is more sensitive to bearing error. It is
**not** a confidence radius, calibrated σ, or GT error. Near-zero two-ray
residuals also do not prove accuracy.

## Not separate Events and not a Track

Each row is a time-aligned localization trial for the **same** Event membership.
Rows are not new Events, not Track points, and not identity continuity. Future
Track work may consume this kind of series; Phase 5A only observes it.

## How to run

```bash
# Live Hub list (opt-in; default list output unchanged)
uv run --project hub/bootstrap python scripts/hub_event_track.py list \
  --allow-long-range-sim-ears \
  --temporal-localization

# Offline unit coverage (ABDE time-varying + edge cases)
uv run --project hub/bootstrap pytest \
  tests/scripts/test_temporal_localization_diagnostic.py -q
```

Ground truth is intentionally not an input to
`evaluate_event_localization_epochs`. If GT metadata is available, evaluate
positions afterward with `evaluate_against_ground_truth`.
