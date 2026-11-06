# B2.5 – Cross-EAR Association Completeness (audit)

Primary goal: explain why available Observations are sometimes absent from the
same Candidate/Event — especially the fourth EAR in `dense_four_ear` — **without
redesigning association rules**.

**Verdict:** The four-EAR gap is an **intentional consequence of the current
experimental model** (Event freeze after promotion), not a temporal/bearing pair
failure and not a silent localization omission. B2.5 is **complete as an audit**.

## 1. Files inspected and changed

### Inspected
- `scripts/lib/cross_ear_event_correlation.py` (assembly, pair eval, Candidate join, promotion)
- `scripts/lib/cross_ear_localization.py` (consensus / outliers — separate from association)
- `scripts/hub_event_track.py`
- `simulator/ear/scenarios/cross_ear_dense_four_ear.json` (+ other cross-EAR scenarios)
- B2.1–B2.4 diagnostics and association/localization tests

### Changed (diagnostics + tests only; rules unchanged)
| Path | Change |
| --- | --- |
| `scripts/lib/cross_ear_event_correlation.py` | `JoinAttemptDiagnostic`, richer `rejected_joins`, `join_attempts` on `CorrelationResult`, membership stage trace helpers |
| `scripts/hub_event_track.py` | Print join attempts + membership trace |
| `tests/scripts/test_cross_ear_association_completeness.py` | Deterministic B2.5 audit tests |
| `docs/diagnostics/ear-cross-ear-b2.5-association-completeness.md` | This report |

**Not changed:** association scoring, promotion thresholds, 500 m radius, temporal gap default (30 s), B2.3 gates, B2.4 sensitivity math, Hub APIs, Track.

## 2. Current Candidate membership algorithm (exact predicates)

### Pipeline
1. **Publish reports** → simulator / MQTT payloads  
2. **Assemble** Observation lifecycles (`assemble_observations`)  
3. **Order** by `(interval_start, node_id, observation_id)` — deterministic  
4. **Pairwise** `evaluate_observation_pair`  
5. **Candidate create** from first accepted unassigned pair  
6. **Join** later Observations to open Candidates  
7. **Promote** when thresholds met → status `Promoted` / `CorrelatedEvent`  
8. **Localize** on Candidate/Event members (separate; outliers do not remove members)

### Pair acceptance (`evaluate_observation_pair`)
Hard reject if any of `{temporal, installation, bearing, acoustic} == incompatible`.
Then require `temporal == compatible` (abstain temporal → no association).

| Dimension | Compatible | Incompatible | Abstain |
| --- | --- | --- | --- |
| **Temporal** | Interval overlap **or** gap ≤ `association_temporal_gap_seconds` (default **30 s**) | Gap > threshold | Incomplete intervals |
| **Installation** | Distance ≤ radius (default **500 m**), or SIM long-range flag | Distance > radius (and not allowed) | Missing coordinates |
| **Bearing** | Present and not `behind_one`/`behind_both`; or geometry ineligible but present (+1 score) | `behind_one` / `behind_both` | Missing bearing on either side |
| **Acoustic** | — | — | **Always** (scenario family ignored) |

Pair score delta (for Candidate aggregate, not join gate):  
`1` (distinct Ear) + `2` if overlap else `1` if gap-ok + `0|1` bearing presence.

### Join rule (open `EventCandidate`)
A new Observation joins **iff** it is **accepted against every current member**
(pairwise). First incompatible member → join `rejected`; try next Candidate / unassigned pairing.

### Ear uniqueness
If `obs.node_id` already in `candidate.ear_ids` → **never join**  
(`ear_already_represented`), even if temporally fine. One Observation lifecycle
per Ear per Candidate.

### Promotion (`maybe_promote`)
Requires **all** of:
- `status == EventCandidate`
- `#ears ≥ min_ears_for_event` (default **3**)
- `score ≥ min_promote_score` (default **4**)
- `score_breakdown["temporal"] ≥ 2` (**some overlapping pair** required; gap-only insufficient)

On promote: status → `Promoted`, snapshot → `CorrelatedEvent`.

### Post-promotion (critical)
If Candidate is `Promoted`, a new Ear that is compatible with **all** Event
members is **not joined**. Decision:
`accepted_but_event_frozen` —  
*"compatible with Event members but Event updates are not applied in this experimental pass."*

### Membership exclusivity
`membership[observation_id] → candidate_id`. An Observation is considered by
at most one Candidate for joining; after assignment it is skipped.

## 3. A–B–C chain behavior

- Pairwise: A–B and B–C may both pass while A–C fails (e.g. contradictory bearings).
- Membership is **not transitive**: C joining Candidate(A,B) requires A–C **and** B–C.
- If A–C fails, C is rejected from that Candidate; it may form another Candidate with unassigned Observations.
- **Risk of transitive association** (if it were allowed): merging unrelated sources that only chain through a poorly constrained middle Ear.  
- **Value**: denser multi-Ear clusters when only local pairs are strong.  
Current code chooses **conservative all-pairs-with-members** without transitive closure.

## 4. Full four-EAR membership trace (`dense_four_ear`)

| Stage | SIM-EAR-001 | SIM-EAR-002 | SIM-EAR-003 | SIM-EAR-004 |
| --- | --- | --- | --- | --- |
| Publishes eligible reports | yes | yes | yes | yes |
| Assembled Observation | yes (start ~t=12s) | yes (~t=10s) | yes (~t=14s) | yes (~t=18s) |
| Pairwise vs each other | all 6 pairs **accepted** (overlap + ≤500 m + non-contradictory bearings) | | | |
| Candidate membership | yes | yes | yes | **no** |
| Event membership | yes | yes | yes | **no** |
| Localization contributing | yes | yes | yes | n/a |
| Outlier excluded | no | no | no | n/a |

**Why the fourth EAR is absent from the Event**

1. Processing order by `interval_start`: B → A → C → D.  
2. A+B form Candidate; C joins → 3 Ears → **promotes immediately** (score 5, temporal overlap).  
3. D arrives later, is compatible with A, B, and C, but Candidate is already `Promoted` → **`accepted_but_event_frozen`**.  

So D is:
- **available** and **assembled**
- **association-eligible** (pairwise)
- **not** a Candidate/Event member  
because of **Event freeze**, not because pairs failed.

Do **not** describe D as “missing Observation” or “localization outlier.”

## 5. Temporal compatibility findings

Meaning of temporal compatibility: **pairwise** interval relationship only.

- Overlap (`overlap_s ≥ 0`) → compatible, `overlapped=True` (+2 toward Candidate temporal breakdown).  
- Gap `0 < gap ≤ 30 s` → compatible, `overlapped=False`.  
- Gap `> 30 s` → incompatible.  
- **Not** required: a single common intersection of all Candidate members.  
- Promotion **does** require at least one overlapping pair (`temporal ≥ 2` in breakdown).  
- Default gap **unchanged** in this audit.

Sequential moving-source Observations that pairwise overlap/gap-ok can still form one Event without three-way common overlap (tested).

## 6. Bearing compatibility findings

- Missing bearing → **abstain** (does not reject; no +1 bearing score).  
- Contradictory for association → only `behind_one` / `behind_both` (not acute angle, not nearly_parallel alone).  
- Association uses **representative** `bearing_deg` from assembly (`_pick_bearing` / first-time geometry input), **not** B2.1 time-aware sample selection used in localization.  
- Moving-source bearing change over a long lifecycle can therefore disagree between association’s single bearing and localization’s time-aligned pairs.  
- Localization outlier exclusion **≠** association bearing gate; outliers never remove Event members.

## 7. Multi-source ambiguity risks

| Situation | Current behavior | Risk |
| --- | --- | --- |
| Two sources, overlapping EAR sets, contradictory cross bearings | Cross pairs reject; clusters tend to stay separate | Moderate protection |
| Two sources with mutually compatible bearings | May merge into one Candidate if pairwise OK | **Merge risk** |
| One Ear, two sequential sources | Second Observation same Ear → `ear_already_represented` on existing Candidate | Forces new Candidate / unassigned; limited multi-episode handling |
| Observation compatible with two open Candidates | Joins **first** open Candidate in list order, then locked in `membership` | Order-dependent cluster choice (list order follows creation / start-time sort) |
| Partial EAR overlap across Candidates | Allowed as separate Candidates | Competing Candidates not merged later |
| Promoted Event + late compatible Ear | Frozen out | Under-complete Events (four-EAR case) |

No multi-source algorithm was implemented in B2.5.

## 8. Demonstrated defects

**None that silently broaden or break pairwise association.**

Documented **design limitations** (not silent bugs):

1. **Event freeze after promotion** → late compatible Ears excluded (`accepted_but_event_frozen`). Explains four-EAR incompleteness.  
2. **Association bearing uses a single assembled bearing**, while localization uses time-aware samples — can disagree under motion.  
3. **One Observation → one Candidate** membership lock; first-match among Candidates.  
4. **nearly_parallel** is not an association reject (only behind rays) — acute geometry can still associate and then withhold in localization (B2.3).

Minimal reproducible case for (1): `dense_four_ear` + join attempt decision `accepted_but_event_frozen` for SIM-EAR-004 (covered by test).

## 9. Diagnostic improvements

- `CorrelationResult.join_attempts`: per join evaluation with pairwise temporal / bearing / installation / distance / decision.  
- Decisions: `accepted`, `rejected`, `ear_already_represented`, `accepted_but_event_frozen`.  
- `rejected_joins` enriched with `decision` + `pairwise`.  
- `build_membership_stage_trace` / hub report lines: assembly → Candidate → Event → consensus / outlier / frozen-compatible.  

## 10. Test results

```text
pytest tests/scripts/test_cross_ear_association_completeness.py \
       tests/scripts/test_cross_ear_localization_robustness.py \
       tests/scripts/test_cross_ear_localization_sensitivity.py \
       tests/scripts/test_hub_event_track.py \
       tests/simulator/ear/test_simulate_scenario.py
→ 96 passed
```

## 11. Recommended next step

### Safe fixes (behavioral, still experimental / script-local)
1. **Defer promotion** until batch end (or until a quiet period), then promote once — allows late Ears to join while Candidate is still open.  
2. Or **allow controlled post-promotion growth** when the new Ear is compatible with all Event members and does not violate ear-uniqueness — with explicit audit log.  
3. Align association bearing selection with localization’s time-aware sample (or evaluate contradiction on the same selected samples) — diagnostic consistency fix.

### Architectural decisions (need discussion)
1. Should Candidate membership stay **all-pairs** or allow limited transitive / clique relaxation?  
2. How to handle **competing Candidates** and Observations compatible with more than one?  
3. Multi-source / ambiguity policy before any Track layer.  
4. Whether “one Observation per Ear per Candidate” should become “replace / extend lifecycle” for moving sources.

### Future Track-related work
- Persistent Events that can grow/split over time.  
- Track continuity across gaps > association window.  
- Explicit multi-hypothesis tracking when Candidates compete.  
**Out of scope for B2.5.**

---

### Is B2.5 complete?

**Yes — complete as an audit.**  
Root cause of four-EAR incompleteness is identified and tested; diagnostics added; association rules intentionally left unchanged pending an architectural decision on Event growth.
