# Step 6 — Hub-side Episode Assembly

**Status:** Implemented (Hub read model).  
**Scope:** Reconstruct acoustic episodes from stored Observation reports for offline labeling and classification research.  
**Non-goals:** Firmware/protocol changes, L0/L1/L2 recalculation, cross-interval averaging, labeling UI.

## Architecture

Episodes are a **deterministic, side-effect-free read model** over [`observations`](../../hub/bootstrap/app/models.py). Original rows and `payload_json` are never modified.

```text
observations (source of truth)
        │
        ▼
episode_service.assemble / list_episodes_page
        │
        ▼
GET /api/v1/episodes
GET /api/v1/episodes/{observation_id}?node_id=…
```

| Concept | Implementation |
| --- | --- |
| Group key | `(node_id, observation_id)` |
| Report order | ascending `sequence_number` (then `id`) |
| Snapshot freeze | `Observation.id <= snapshot_id` for **all** pages and detail |
| Status filter | SQL episode-level `complete` / `incomplete` **before** `LIMIT/OFFSET` |
| Policy | Reuses `observations.read` |

Code: [`hub/bootstrap/app/episode_service.py`](../../hub/bootstrap/app/episode_service.py), schemas in [`schemas.py`](../../hub/bootstrap/app/schemas.py), routes in [`api.py`](../../hub/bootstrap/app/api.py).

## Database design

**No new tables.** Additive read model only.

Existing columns used: `id`, `node_id`, `observation_id`, `sequence_number`, `event_time_utc`, `received_time_utc`, `payload_json`.

## Episode lifecycle rules

Expected shapes (among stored reports):

- `start → periodic* → end`
- `start → end`
- lone `complete` (schema / Phase-1 path; not emitted by live RMS detector)

Terminal kinds: `end`, `complete`.

## Data integrity rules

Diagnostics are **additive** (`quality.issues`). Nothing is silently repaired.

| Code | Meaning |
| --- | --- |
| `missing_start` | No `start` and no `complete` |
| `missing_end` | Has `start` but no terminal emission |
| `suspected_sequence_gap` | Gap between consecutive episode report `sequence_number` values. **Not confirmed episode-local missing data** — `sequence_number` is **global per Ear** (firmware NVS allocator; Hub dedup key `(node_id, sequence_number)`). Gaps may be lost MQTT, refused publish that still consumed a seq, or another interleaved episode. |
| `duplicate_emission` | More than one `start` / `end` / `complete` |
| `unexpected_transition` | Emission kind order not in the expected grammar |
| `out_of_order_arrival` | `received_time_utc` regresses vs sequence order |
| `timing_discontinuity` | Same-Ear `capture_timestamp_monotonic_us` decreases (monotonic session break). Comparisons never cross EARs. |
| `wall_time_regression` | Second-resolution `event_time_utc` moves backward; **not** used to infer interval adjacency/overlap |
| `payload_invalid` | JSON / `ObservationBody` validation failure |
| `identity_mismatch` | Payload identity ≠ row keys |
| `incomplete_acoustic` | Present L1/L2 object fails structure checks. **`signature: null` / `dynamics: null` are valid absence**, not incomplete. |

## API specification

### `GET /api/v1/episodes`

Query: `node_id?`, `page`, `page_size`, `order=newest|oldest`, `snapshot_id?`, `status=any|complete|incomplete`.

Envelope: `{ "pagination": PaginationMeta, "items": [EpisodeSummary] }`.

`pagination.snapshot_id` is always the resolved freeze id on the first response; clients must reuse it for subsequent pages.

`status=complete`: start+terminal, or complete-only.  
`status=incomplete`: everything else.  
Filter + `total_count` use the same snapshot.

### `GET /api/v1/episodes/{observation_id}?node_id=…`

Full `EpisodeView` with ordered `intervals[]`. Optional `snapshot_id` excludes late reports for that episode.

## Timing notes

| Field | Role |
| --- | --- |
| `event_time_utc` | Wall **interval start**, **second** resolution |
| `capture_timestamp_monotonic_us` | Device mono **interval end**; compare only within one Ear and one continuous mono session |
| `episode_duration_ms` on episode | Taken from the last interval’s Ear-reported value when present |

## Example assembled episode JSON

```json
{
  "node_id": "EAR-001",
  "observation_id": "a5314310-1111-4222-8333-555555555555",
  "first_event_time_utc": "2026-11-05T18:00:00Z",
  "last_event_time_utc": "2026-11-05T18:00:10Z",
  "episode_duration_ms": 12000.0,
  "report_count": 3,
  "max_report_id": 42,
  "quality": {
    "has_start": true,
    "has_end": true,
    "has_complete": false,
    "has_periodic": true,
    "is_terminal": true,
    "issue_codes": [],
    "issues": []
  },
  "intervals": [
    {
      "report_id": 40,
      "sequence_number": 10,
      "emission_kind": "start",
      "termination_reason": null,
      "event_time_utc": "2026-11-05T18:00:00Z",
      "capture_timestamp_monotonic_us": 10000000,
      "received_time_utc": "2026-11-05T18:00:00.120000Z",
      "interval_duration_ms": 2000.0,
      "episode_duration_ms": 2000.0,
      "signal_level_dbfs": -42.5,
      "measurements": { "schema": "l0", "version": 1 },
      "signature": null,
      "dynamics": null
    }
  ]
}
```

## Tests

[`hub/bootstrap/tests/test_episode_assembly.py`](../../hub/bootstrap/tests/test_episode_assembly.py) covers START→PERIODIC→END, START→END, missing start/end, suspected sequence gaps, duplicate emissions, out-of-order arrival, mono discontinuity, multi-episode / multi-EAR isolation, null L1/L2 not incomplete, corrupt payload, snapshot freeze (including late END excluded), API list/detail, and observations API smoke.

## Known limitations / future work

- No materialized episode table or acoustic feature search indexes.
- No cross-EAR monotonic time comparison.
- Wall-clock episode span from `event_time_utc` is second-granularity only.
- `suspected_sequence_gap` cannot prove which of lost MQTT / refuse / interleave occurred.
- `COMPLETE` is rare on the live RMS path.
- No labeling UI, classifier, or cross-interval feature aggregation (explicitly deferred).

## Implemented vs proposed

| Implemented | Proposed later |
| --- | --- |
| Read-model assembly + integrity | Materialized episodes / search |
| Snapshot-stable pagination + status filter | Streaming / push of episode close |
| Interval-preserving L0/L1/L2 access | Offline classifier feature store |
| Additive quality metadata | Auto-repair or imputation (out of scope by design) |
