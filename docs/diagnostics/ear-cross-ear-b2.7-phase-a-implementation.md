# B2.7 Phase A – Implementation notes

Implements the approved Phase A of
[`ear-cross-ear-b2.7-event-membership-lifecycle-design.md`](ear-cross-ear-b2.7-event-membership-lifecycle-design.md).

**Not in this phase:** Hub Event persistence, lateness/stabilization defaults,
Track, LoRaWAN, association score/threshold changes, B2.8 install metadata.

## What changed

### Grow-after-promotion
- Removed script-local `accepted_but_event_frozen` refusal path.
- Compatible Observations join `Promoted` Candidates under the existing
  B2.5 all-members rule.
- `CorrelatedEvent` membership and localization are synced when membership grows;
  `event_id` remains the identity assigned at first promotion.

### Multi-compatible Observations
- Before joining, each Observation is evaluated against **all** open
  Candidates/Events.
- Compatible with exactly one → join that one.
- Compatible with multiple → `UnresolvedAssociation` (no first-match).
- Events are never silently merged.

### Shared complete-snapshot analysis
- `AnalysisSnapshot` / `AnalysisConfig` / `AnalysisResult` and `analyze(snapshot)`
  in `scripts/hub_event_track.py`.
- `fetch_all_observation_reports()` paginates under Hub `snapshot_id`.
- Both `list` and `watch` rebuild a complete snapshot and call `analyze()`.
- Watch re-fetches nodes/weather on each reanalyze (no longer omits site context).
- Acoustic `event_time_utc` remains association chronology; `received_time_utc`
  is ingestion/pagination only.

### Localization contributors vs Event membership
- `LocalizationResult.contributing_ears` is now the subset of EARs that appear
  in non-outlier consensus pairs (not the full association membership).
- Diagnostics print association membership counts separately from localization
  contributor counts.

## Explicitly deferred (documented, not invented)
- Stabilization / allowed-lateness numeric defaults (B2.7 open decision).
- Hub-persisted Event revisions.
- Optional multi-compatible auto-resolve flag (default remains unresolved).

## Tests
- `tests/scripts/test_cross_ear_event_membership_lifecycle.py` (Phase A coverage).
- Updated B2.5 four-EAR expectations for post-promotion growth.

## Focused verification (post Phase A)

### Why the original synthetic fourth bearing failed
Fixture used `bearing_deg=90` on SIM-EAR-004 while SIM-EAR-001/002/003 used
centroid-pointing bearings (180°/300°/60°). Pair **004↔003** was rejected by the
existing B2.5 rule: `bearing == incompatible` because LOB geometry was
`behind_one` (not a useful forward locus). Rules were **not** relaxed.

A geometrically consistent fourth bearing is **0°** (due north of the centroid
with `orientation_deg=0`). Test A uses that; Test B separately uses abstain
(`bearing=None`).

### `contributing_ears` semantics
Unique EARs appearing in **non-outlier consensus pairs that feed a published
position**. Empty when the position is withheld. Independent of Event
membership. Diagnostics also expose:
`association_member_ears`, `ears_with_usable_bearing`,
`forward_intersection_ears`, `consensus_selected_ears`, `outlier_pair_ears`.

### Snapshot completeness and immutability
Hub freezes pages at `Observation.id <= snapshot_id`.

**Verified (Hub concurrency tests):** each MQTT report is one immutable DB row.
There is **no** append/update of an existing report’s `payload_json` on late
delivery. A late report for an existing `observation_id` still inserts a **new**
row with a higher `id`, so it cannot enter an already frozen snapshot. Duplicate
`(node_id, sequence_number)` re-delivery is a no-op and does not mutate the row.
A subsequent snapshot (higher `snapshot_id`) includes the late rows.

Completeness of “all reports ever” is **not** guaranteed by one freeze: later
ingests appear only after a rebuild. The script sets `complete_snapshot` only
when `fetched_report_count == total_count`.

## Pre-flight note
Uncommitted workspace already contained B2.3–B2.8 diagnostics and Step 5/6 docs;
those were preserved. Private B2.8 harness under `shared/results/ear-tests/**`
remains gitignored.
