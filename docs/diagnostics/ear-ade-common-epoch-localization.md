# ADE common-epoch localization diagnostic

Originally an investigation-only comparison. Production
`estimate_localization` now defaults to Event-level **common-epoch** selection
with pairwise `select_time_aware_bearing_pair` as complete fallback (see
`COMMON_EPOCH_RANKING_RULE` in `scripts/lib/cross_ear_localization.py`).
Association, promotion, timing gates, and scenarios remain unchanged. Ground
truth is used **only** for post-hoc error reporting.

## Reproduce

```bash
uv run --project hub/bootstrap python scripts/lib/localization_epoch_diagnostic.py

uv run --project hub/bootstrap pytest tests/scripts/test_localization_epoch_diagnostic.py -q
```

Offline ADE assembly uses `helicopter_ade_overlap.json` bearings as emitted by
the scenario simulator (install geometry + route). No MQTT. Started-at clock for
the offline replay: `2026-11-07T12:00:00Z`.

## 1. Root cause of mixed-epoch selection

Production path (`scripts/lib/cross_ear_localization.py`):

1. For each Observation **pair**, call `select_time_aware_bearing_pair`.
2. Among timestamped samples, minimize `|Δt|`.
3. Tie-break: maximize `bearing.confidence` on sample A, then on sample B.
4. `aligned = (Δt ≤ LOCALIZATION_MAX_BEARING_DT_S)` with gate **1.0 s**.
5. Build one forward intersection per pair; mark multi-ray outliers; consensus
   mean of non-outlier forward loci; set status / `contributing_ears`.

There is **no** shared localization epoch across Event members. Each pair is
optimized independently.

On ADE, all five cadence ticks are exact matches (`Δt = 0`) for every pair, so
step 2 never distinguishes epochs. Authored confidences then decide:

| Pair | Peak confidence epoch | Selected time (offline) |
| --- | --- | --- |
| SIM-EAR-001 ↔ 004 (A–D) | A peaks at **t=20** (0.78) | 12:00:20 |
| SIM-EAR-001 ↔ 005 (A–E) | A peaks at **t=20** | 12:00:20 |
| SIM-EAR-004 ↔ 005 (D–E) | D peaks at **t=30** (0.72) | 12:00:30 |

That matches the live Hub run (10 s apart ≈ 300 m at 30 m/s). Mixing t=20 and
t=30 loci makes A–D and D–E **outliers**; consensus keeps only A–E →
**2 contributors**, status **`weak`** (single forward consensus pair / outlier
path).

## 2. Common-epoch diagnostic mode

`scripts/lib/localization_epoch_diagnostic.py`:

- **Current mode:** `estimate_localization(members, nodes)` unchanged.
- **Common-epoch mode:** for each timestamp where ≥2 Ears have a sample within
  the 1 s gate, restrict each Observation to that sample and call the **same**
  `estimate_localization`. Pairs cannot mix epochs.
- A diagnostic-only picker ranks epochs by: ear count → status → aligned forward
  pair count → lower network sensitivity → higher min ∠ → earlier time.
  **Not** a production policy; **not** GT-driven.

### ADE offline results (summary)

| Mode / epoch | Pair times | Forward pairs | Outliers | Contributors | Status | Spread | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **Current pairwise** | AD/AE @20s; DE @30s | 3 | AD, DE | **001, 005** | **weak** | n/a (single consensus) | Matches live ADE symptom |
| Common @0 s | all @0 | 3 | 0 | 001,004,005 | useful | ~2.3 m | Diagnostic pick (lowest sens) |
| Common @10 s | all @10 | 3 | 0 | 001,004,005 | useful | ~1.6 m | |
| Common @20 s | all @20 | 3 | 0 | 001,004,005 | useful | ~1.1 m | Same epoch as current AD/AE |
| Common @30 s | all @30 | 3 | 0 | 001,004,005 | useful | ~0.7 m | Same epoch as current DE |
| Common @40 s | all @40 | 3 | 0 | 001,004,005 | useful | ~0.5 m | Highest DE dilution |

Post-hoc GT (not used for selection): current locus vs route at evaluation time
≈ **1 m**; diagnostic common @0 s ≈ **0.5 m**. Current vs common positions are
~600 m apart because they evaluate different epochs along a 30 m/s track — that
is epoch choice, not a claim that one mode is “more accurate.”

## 3. Does common-epoch improve internal consistency?

**Yes, for this moving synchronized case.** Forcing one timestamp:

- removes cross-pair source motion between LOBs;
- keeps all three ADE pairs as non-outlier forward intersections;
- restores **three** contributing Ears and status **`useful`** under existing
  gates (spread ≪ sparse 250 m heuristic).

Material change vs production ADE result: weak/2-EAR → useful/3-EAR, and the
published locus tracks a single coherent epoch instead of a mixed-epoch
survivor pair.

## 4. Limitations of common-epoch

- **Missing report at an epoch:** that Ear is omitted; remaining Ears can still
  form pairs (covered by unit test). Not all epochs remain fully three-Ear.
- **No shared timestamp (≥2 Ears):** common-epoch mode yields no candidate;
  pairwise mode still runs (often misaligned / weak). Covered by unit test.
- **Unequal cadence / staggered START-only samples:** common epochs exist only
  where clocks coincide within the 1 s gate; staggered periodics with Δt=2 s
  never form a common epoch under the current gate.
- **Which epoch to pick** when several are usable is policy, not geometry.
  Confidence-max, mid-pass, lowest dilution, and densest triple coverage can
  disagree; the diagnostic heuristic is explicit and non-GT, but not final.
- **Does not fix poor geometry** at a bad epoch (e.g. acute DE at route end);
  it only prevents mixing incompatible motion states.

## 5. Production implementation (completed)

`estimate_localization(..., bearing_selection="auto")` now:

1. Lists candidate epochs (≥2 Ears with a sample within `LOCALIZATION_MAX_BEARING_DT_S`).
2. Restricts each Observation to ≤1 reported sample at that epoch.
3. Runs the existing pairwise geometry / consensus / quality pipeline on the
   restricted set (never mixes epochs inside one result).
4. Ranks candidates with `COMMON_EPOCH_RANKING_RULE` (no GT):
   `ear_count > status(useful>weak>unavailable) > aligned_forward_pair_count >
   lower network_geometric_sensitivity_m > higher min_intersection_angle_deg >
   earlier common_epoch_utc`.
5. If no candidate exists → complete `pairwise_fallback` (unchanged
   `select_time_aware_bearing_pair`).

Details expose `bearing_selection_mode`, `common_epoch_utc`, participating /
excluded Ears, selected bearings, and pair time deltas. Association membership
is unchanged; excluded-at-epoch Ears remain Event members when associated.

**Open design note:** among ADE’s five all-`useful` epochs, ranking currently
prefers **t=0** (lowest network sensitivity). Mid-pass epochs are equally
coherent; a future policy could prefer max min-∠ or mid-window without GT.

## Files

| Path | Role |
| --- | --- |
| `scripts/lib/cross_ear_localization.py` | Production common-epoch + pairwise fallback |
| `scripts/lib/localization_epoch_diagnostic.py` | Offline pairwise_only vs production compare |
| `tests/scripts/test_common_epoch_localization.py` | Production ADE / fallback / gate / ranking tests |
| `tests/scripts/test_localization_epoch_diagnostic.py` | Diagnostic comparison tests |
| `docs/diagnostics/ear-ade-common-epoch-localization.md` | This report |
