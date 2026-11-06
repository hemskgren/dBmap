# B2.7 – Event Membership, Temporal Lifecycle and Deterministic Analysis Design

**Phase type:** architecture design (complete). Phase A implementation notes:
[`ear-cross-ear-b2.7-phase-a-implementation.md`](ear-cross-ear-b2.7-phase-a-implementation.md).  
**Inputs:** B2.3–B2.6 diagnostics; `hub_event_track.py`, `cross_ear_event_correlation.py`, `cross_ear_localization.py`, Hub report ingest/assembly.  
**Original design constraints:** No Hub schema/API, association-score, threshold, Track, or LoRaWAN changes in the design phase. Phase A implements script-local grow-after-promote + complete-snapshot `analyze()` only.

Legend used throughout:

| Tag | Meaning |
| --- | --- |
| **Verified** | Current behavior confirmed in B2.5/B2.6 |
| **Proposed** | Target architecture recommended here |
| **Unresolved** | Needs human approval before implementation |

---

## 1. Current verified behavior (from B2.5 / B2.6)

### 1.1 Pipeline today
1. Hub stores **reports** (`event_time_utc`, `received_time_utc`; dedup `(node_id, sequence_number)`).
2. Script assembles Observations by `(node_id, observation_id)` using acoustic `event_time_utc`.
3. Correlation orders Observations by `(interval_start, node_id, observation_id)`.
4. Pairwise association → Candidates → promote when ears≥3, score≥4, temporal overlap≥2.
5. After promote, compatible joins are refused: `accepted_but_event_frozen`.
6. Localization runs on members; outliers/withholds do not remove members.
7. `list` / `watch` analyze at most one Hub page (watch: newest ≤500); watch re-runs `report()` without durable Events and without nodes on updates.

### 1.2 Verified gaps this design closes
| Gap | Effect |
| --- | --- |
| Post-promotion freeze | Fourth EAR in `dense_four_ear` never joins despite all-pairs OK |
| Page-bounded analysis | Incomplete evidence; list ≠ watch ≠ full replay |
| Coupled mental model | Members ≈ localization inputs; counts not surfaced distinctly |
| No lifecycle / lateness policy | No provisional vs stabilized vs corrected semantics |
| First-assignment exclusivity | No explicit multi-Event ambiguity representation |

### 1.3 What must not change without an explicit decision
- B2.5 all-members pairwise compatibility for joins.
- B2.5 promotion thresholds and association scoring.
- B2.3 localization quality gates / withhold rules.
- B2.4 dilution / sensitivity / leave-one-out diagnostics.
- GT / simulator source labels out of production association and localization.

---

## 2. Proposed target architecture (summary)

**Proposed** analysis stack:

```
Hub reports (paginated)
        │
        ▼
 AnalysisSnapshot  ── complete report set + nodes + config + cutoff
        │
        ▼
 ObservationAssembly  ── event_time chronology; OOO/partial OK
        │
        ▼
 AssociationEngine  ── Candidates/Events; grow after promote; all-members checks
        │
        ├── AssociationDecision / UnresolvedAssociation
        │
        ▼
 EventRecord  ── association membership + lifecycle + revisions
        │
        ▼
 LocalizationEngine  ── subset selection; independent LocalizationRevision
        │
        ▼
 AnalysisResult  ── deterministic for identical Snapshot + config
```

**Recommended defaults (subject to §11):**

1. **Remove Event freeze**; allow membership growth after promotion under the same all-members rule.  
2. **Primary correctness path:** rebuild from a **complete AnalysisSnapshot** (not incremental state).  
3. **`list` and `watch` share one analysis function**; watch may show provisional intermediates.  
4. Lifecycle: `provisional → stabilized → corrected`. Stabilization never means permanent rejection.  
5. Multi-compatible Observations become **unresolved** until deterministic evidence resolves them — never arrival-order first-match.  
6. Hub Event persistence remains a later product phase; script-local types are enough for Phase A.

---

## 3. Event membership versus localization membership

### 3.1 Principles
| Principle | Status |
| --- | --- |
| Event = full associated Observation set | **Proposed** (adopt B2.6) |
| Localization = measurement subset for current fix | **Verified** in spirit; **Proposed** to expose explicitly |
| Missing/rejected bearing ≠ leave Event | **Verified** for outliers; **Proposed** to keep when growing membership |
| Localization recalculation without membership change | **Proposed** |
| No membership cap from promotion/localization needs | **Proposed** |

### 3.2 Distinct counts (always report separately)
| Count | Definition |
| --- | --- |
| `report_count` | Raw Hub reports in the snapshot (after Hub dedup) |
| `associated_observation_count` | Unique `(node_id, observation_id)` on the Event |
| `unique_associated_ear_count` | Unique `node_id` among associated Observations |
| `observations_with_bearing_count` | Associated Observations with usable bearing evidence |
| `localization_contributor_ear_count` | EARs in current localization revision’s `contributing_ears` |
| `localization_excluded_measurement_count` | Bearings/pairs considered then excluded (with reason) |
| `unresolved_association_count` | Observations pending multi-Event arbitration |

### 3.3 Example — city-wide explosion
- 105 EARs deployed; 100 report the blast → **100 associated Observations**, **100 unique EARs**.  
- 5 have useful bearings; localization selects **3** → contributor count 3.  
- Event still lists all 100; localization diagnostics list 3 contributors + exclusions.  
- Promotion still occurs at the existing threshold (e.g. 3 ears); remaining 97 may join afterward while provisional/corrected.

### 3.4 Structural separation (**Proposed** interfaces)
Do **not** invent Hub tables in Phase A. Extend script-local result models:

```text
EventAssociationMembership
  event_id
  observation_refs: list[(node_id, observation_id)]
  ear_ids: derived unique set
  membership_revision: int
  added_at: list of MembershipAddRecord  # event_time, received_time, process_time optional

LocalizationRevision
  event_id
  localization_revision: int
  based_on_membership_revision: int
  contributing_ears / contributing_observation_ids
  excluded: list[(ref, reason)]   # outlier, geometry, missing bearing, …
  result: LocalizationResult      # existing B2.3/B2.4 fields unchanged in meaning

EventRecord
  event_id
  lifecycle_state: provisional | stabilized | corrected
  association: EventAssociationMembership
  localization: LocalizationRevision | None
  history: list[RevisionAuditEntry]
```

**Verified today:** `CorrelatedEvent.members` holds association members used as localization inputs; freeze blocks growth. Localization `contributing_ears` / outliers exist but Event does not version membership vs localization independently.

---

## 4. Lifecycle and revision semantics

### 4.1 States (**Proposed**)

| State | Meaning |
| --- | --- |
| `provisional` | Event exists (promotion criteria met); membership and localization may still change as compatible evidence arrives |
| `stabilized` | Quiet / lateness policy says the normal completion window has elapsed; published “current” result is the stable view |
| `corrected` | After stabilization, late evidence caused a documented membership and/or localization revision |

`EventCandidate` (pre-promotion) remains as today until thresholds fire → create `EventRecord` in `provisional`.

### 4.2 What creates a provisional Event
**Proposed:** same promotion predicates as today (ears, score, temporal overlap). Promotion creates Event identity; it does **not** freeze membership.

### 4.3 When membership may grow
**Proposed:** whenever a new/updated Observation is **accepted against every current member** (B2.5 rule), and the Observation is not already assigned elsewhere or unresolved:

- During `provisional`: normal growth.  
- During `stabilized` / `corrected`: growth allowed if lateness policy accepts the report → transition to `corrected` (or stay `corrected`) with a new membership revision.

### 4.4 What causes stabilization
**Proposed default:** stabilize when **both**:

1. **Receive quiet period:** no new associated report with `received_time_utc` in the last `stabilize_quiet_seconds` (configurable), measured against analysis clock / snapshot cutoff; **and**  
2. **Optional acoustic quiet:** no associated Observation interval ending after `now_event_horizon` if such a horizon is configured.

**Unresolved:** exact default values; whether acoustic quiet is required; whether operator “finalize” exists.

Stabilization **must not** imply permanent rejection of later reports.

### 4.5 Late arrivals

| Arrival | **Proposed** behavior |
| --- | --- |
| Late **compatible** Observation (new) | If lateness policy admits: join → new membership revision → recalculate localization → `provisional` stay or `stabilized`→`corrected` |
| Late **incompatible** Observation | Do not join; record reject reason; may start/join another Candidate/Event or remain singleton |
| Late report **extends** existing associated Observation | Re-assemble Observation; if still compatible with all members, keep membership (possibly extend intervals); if newly incompatible with some members → **Unresolved** conflict handling (§6.4), do not silently drop history |
| Late report for Observation already on another Event | Do not dual-assign; treat as assembly update on that Event’s member |
| Outside lateness “soft window” | Still **admissible** as `corrected` or as `UnresolvedAssociation` / quarantine — never hard-delete evidence |

### 4.6 Revisions
**Proposed:**

- **Membership revision** increments on add/remove/reassignment of associated Observations.  
- **Localization revision** increments when the localization subset or estimate changes; always references `based_on_membership_revision`.  
- Correction after stabilize: append `RevisionAuditEntry` (what changed, which reports, `received_time_utc`, optional `processed_at`).  
- Prefer **immutable revision history** + “current pointer” over silent overwrite of the only result.

Localization may revise **without** membership change (e.g. better subset selection, new bearing on an existing member). Membership change usually triggers localization recalculation but can leave position withheld.

### 4.7 Ambiguous / unresolved membership
**Proposed:** first-class `UnresolvedAssociation` records (not forced into an Event). Diagnostics list them separately from Event members. See §6.

---

## 5. Timestamp and lateness policy

### 5.1 Distinct clocks
| Field | Role | Status |
| --- | --- | --- |
| `event_time_utc` | Acoustic chronology, assembly intervals, association temporal checks, localization sample times | **Verified** exists |
| `received_time_utc` | Ingest order, pagination, lateness / quiet periods | **Verified** exists |
| `processed_at` | When an analysis run touched a report/Event | **Proposed** optional later; not required for Phase A correctness |

**Proposed rule:** association uses acoustic evidence (`event_time_utc` + intervals + installation + bearing gates). Transport arrival order must not decide association.

### 5.2 Physics and clock limits (**Proposed** guidance)
- `event_time_utc` is **detection time at the EAR**, not source origin time.  
- Propagation across a city can stagger EAR times by seconds; physical EAR clocks are ~1 s resolution.  
- Therefore: keep **interval overlap / gap** association (existing), do **not** treat timestamps as TDOA, do **not** compare monotonic clocks across EARs.  
- Clock uncertainty already partially surfaces via `timing_quality` / `timestamp_uncertainty_ms` in payloads — use as soft diagnostics later; do not invent precision that sensors lack.

### 5.3 Configurable lateness policy (**Proposed**)
Parameters (names illustrative; values **Unresolved**):

| Parameter | Intent |
| --- | --- |
| `stabilize_quiet_seconds` | Receive-side quiet before `provisional`→`stabilized` |
| `soft_lateness_seconds` | Expected late window (`received − event` or since Event create); still normal `corrected` path |
| `hard_review_lateness_seconds` | Beyond this, auto-join only if still all-members compatible **and** mark `corrected` with high-visibility audit; alternatively force `UnresolvedAssociation` for human/heuristic review |

**Proposed:** never a hard “reject forever” based on lateness alone. Incompatible evidence is rejected for association reasons, not because it was late.

### 5.4 Example — report several minutes late
1. Event already `stabilized` with 3 EARs.  
2. Report arrives with `event_time` inside the blast window but `received_time` = +4 minutes.  
3. Assembly completes Observation for EAR-4.  
4. All-members checks pass → membership revision N+1, lifecycle → `corrected`, localization recalculated (may still use only 3 contributors).  
5. Audit records lateness metrics; no identity change of `event_id`.

---

## 6. Multi-compatible Observation arbitration

### 6.1 Problem
**Verified:** today an Observation is assigned to at most one Candidate via processing order / first successful join; Promoted Events freeze. That is arrival/processing-order dependent for edge cases and cannot express “compatible with A and B.”

### 6.2 Decision categories (**Proposed**)
For each Observation after evaluating open Events/Candidates:

| Outcome | Meaning |
| --- | --- |
| `assigned_unique` | Compatible with exactly one Event/Candidate under all-members rule |
| `unresolved_multi` | Compatible with ≥2 Events/Candidates |
| `insufficient` | Not enough evidence (e.g. temporal abstain, missing install) to join any |
| `contradictory` | Hard incompatible with all candidates tried; may remain singleton or start new Candidate |
| `resolved_later` | Was `unresolved_multi`; later evidence / policy resolved it |

### 6.3 Deterministic arbitration (**Proposed**)
**Do not** use first-match or report arrival order.

When `unresolved_multi`:

1. **Default:** leave Observation in `UnresolvedAssociation` with the list of compatible `event_id`s and pairwise scores/reasons. Do **not** assign.  
2. **Optional auto-resolve** (only if approved): choose Event with highest aggregate association score vs members; ties broken by `(earliest event_id, lexicographic)`. Document that auto-resolve is a **policy**, not proof of single-source truth.  
3. **Never** merge competing Events silently.  
4. **Never** assign one Observation to multiple Events.  
5. Later evidence (new members, completed lifecycle, clearer bearings) may re-run arbitration in a new analysis revision.

### 6.4 Preserving B2.5 all-members rule
**Proposed:** joining Event E still requires accepted pairs vs **every current member**. Compatibility with “some” members is insufficient. Unrestricted transitive association remains out of scope.

If a late report **updates** a member Observation so it becomes incompatible with another member:

- **Unresolved decision:** either (a) mark Event `association_conflict` and withhold localization, or (b) move the offending Observation to unresolved and shrink membership with audit. Prefer (a) or unresolved over silent keep. **Do not** silently merge or drop without audit.

### 6.5 Example — two simultaneous sources
- Source S1 → Event E1 (EARs A,B,C). Source S2 → Event E2 (EARs D,E,F).  
- EAR G hears both (or ambiguous interval) and is all-members-compatible with E1 **and** E2 (weak temporal / abstain bearing).  
- **Proposed:** G → `unresolved_multi` linked to {E1, E2}; neither Event absorbs G; no merge.  
- Localization of E1/E2 unchanged by G until resolved.

### 6.6 Competing Events
**Proposed:** keep both Events. Diagnostics show overlap, unresolved Observations, and score margins. Track / multi-hypothesis fusion remains out of scope (explicit future work).

---

## 7. Complete snapshot and batch/watch convergence

### 7.1 Shared input contract (**Proposed**)

```text
AnalysisSnapshot
  snapshot_id          # stable id for this input set (hash or explicit)
  cutoff_received_time # inclusive upper bound for reports in this snapshot (optional)
  reports[]            # ALL pages in scope after Hub dedup
  nodes{}              # installation metadata for all referenced node_ids
  environmental_context # optional; same for list and watch when available
  config               # association/localization parameters (unchanged defaults)
  analysis_clock       # wall time used for quiet/lateness evaluation (injected for tests)
```

**Fetch rule:** paginate until exhausted (or until window predicate ends). Never treat “page 1 newest 500” as complete unless the window is proven smaller.

**Dedup:** rely on Hub `(node_id, sequence_number)`; analysis treats report identity as that key plus `observation_id` for assembly.

### 7.2 Shared analysis function (**Proposed**)

```text
analyze(snapshot: AnalysisSnapshot) -> AnalysisResult
```

Both `list` and `watch` call `analyze`. Differences are only **how often** snapshots are built and whether UI labels results `provisional`.

### 7.3 Determinism (**Proposed**)
For identical `reports` + `nodes` + `config` (+ fixed `analysis_clock` when lifecycle evaluated):

- Same Observation assemblies  
- Same Event membership sets and unresolved sets  
- Same localization contributors / status / position (existing deterministic ties)  
independent of original transport arrival order **inside** the snapshot.

**Implementation note:** rebuild-from-snapshot with fixed Observation sort + grow-after-promote (no freeze) is sufficient for Phase A. Incremental Event state is an optimization, not a correctness requirement.

### 7.4 Watch behavior (**Proposed**)
1. On change, build complete snapshot for the configured window (not newest-500 alone).  
2. Always pass nodes (and weather when available) as in `list`.  
3. Run `analyze(snapshot)`.  
4. May display lifecycle `provisional` while quiet period not met.  
5. Final result after all reports are in the snapshot must match batch replay of that snapshot.

### 7.5 Example — same reports, different arrival orders
Reports for EARs A,B,C,D (all pairwise OK) arrive as D,C,B,A vs A,B,C,D.

| Mode | **Verified** today | **Proposed** |
| --- | --- | --- |
| Single complete batch | Promote at 3 → freeze out 4th depending on sort+promote timing | All four associated; same `event_id` membership set |
| Watch partial then late D | Page may miss D; freeze if promoted | Snapshot includes D when present; D joins; same final as batch |

### 7.6 Safe implementation sequence (**Proposed**)
1. Complete snapshot fetch + `analyze()` as reference.  
2. Point `list` and `watch` at `analyze()`.  
3. Determinism / replay tests (including shuffled report order).  
4. Only then consider durable incremental state / Hub Events / optimization.

---

## 8. Scale and transport considerations

### 8.1 Scale (**Proposed** expectations)
- Association membership for **100+ EARs** is in-memory feasible for experimental analysis.  
- Localization must remain a **subset** selector; do not run unconstrained all-combinations explosion as membership grows — keep current consensus approach and B2.3/B2.4 diagnostics, with summary output (counts + contributors + exclusions).  
- Diagnostics default to aggregates; detail on demand.  
- Snapshot fetch must handle multi-page report sets.

### 8.2 Transport-independent requirements (**Proposed**)
| Requirement | Why |
| --- | --- |
| Association keyed by acoustic evidence + EAR identity | Wi-Fi or LoRaWAN must not redefine Event meaning |
| Complete snapshots + lateness policy | Absorbs delay, batching, OOO delivery |
| Hub dedup remains authoritative for retries | Duplicate uplink common on constrained links |
| Partial Observation assembly | Incomplete START/PERIODIC/END delivery |
| No transport-specific branches in association scoring | Avoid LoRaWAN assumptions in acoustic logic |

**Verified today:** Wi-Fi/LAN predominates; LoRaWAN not implemented.  
**Proposed:** design as above; implement no LoRaWAN code in follow-on Phase A either unless separately scoped.

---

## 9. Proposed data structures / interfaces

Clearly **proposals** for script-local Phase A (not Hub schema):

| Name | Purpose |
| --- | --- |
| `AnalysisSnapshot` | Complete input contract (§7.1) |
| `AnalysisResult` | Events, candidates, unresolved, assemblies, diagnostics |
| `EventRecord` | Lifecycle + membership + current localization pointer |
| `EventAssociationMembership` / `MembershipAddRecord` | Association set + audit |
| `LocalizationRevision` | Independent localization versioning |
| `UnresolvedAssociation` | Multi-compatible / insufficient / conflict |
| `RevisionAuditEntry` | Membership or localization change log |
| `LatenessPolicy` | Configurable quiet / soft / review thresholds |
| `analyze(snapshot) -> AnalysisResult` | Shared list/watch entry |

Existing types to **retain and adapt** (not delete): `AssembledObservation`, `EventCandidate`, `AssociationEvidence`, `LocalizationResult`, join diagnostics from B2.5.

**Hub schema / API:** none required for Phase A. Future product persistence is Phase C (Event, membership, localization_revision tables) — out of this design’s implementation scope.

`processed_at`: optional analysis-run metadata; **Unresolved** whether to persist.

---

## 10. Migration / implementation phases

| Phase | Scope | Hub schema? |
| --- | --- | --- |
| **A – Correctness** | Complete snapshot; remove freeze (grow-after-promote); separate membership vs localization diagnostics; shared `analyze()` for list/watch; unresolved multi-compatible stub; tests | No |
| **B – Lifecycle** | `provisional`/`stabilized`/`corrected`; lateness policy params; revision audit; stabilization clock | No (script-local) |
| **C – Persistence** | Durable Events across restarts; APIs; optional `processed_at` | Yes (separate decision) |
| **D – Optimization** | Incremental updates only if profiling requires; never before A–B tests green | Maybe |
| **Track** | Explicitly deferred | N/A |

**Proposed conflict with current behavior (explicit, not silent):** removing `accepted_but_event_frozen` changes experimental Event membership for four-EAR and similar cases. That is an intentional correction of an admitted shortcut, not a change to pairwise scoring or promotion thresholds.

---

## 11. Open decisions and risks

### 11.1 Decisions requiring human approval

| ID | Decision | Recommendation |
| --- | --- | --- |
| **D1** | Grow-after-promote vs defer-promote-only vs both | **Grow-after-promote** as required; optionally also defer promote until end of a single snapshot pass for cleaner “first publish” timing |
| **D2** | Stabilization clock | **Primary: `received_time_utc` quiet period**; optional acoustic horizon as secondary |
| **D3** | Multi-compatible auto-resolve vs always unresolved | **Default leave unresolved**; auto-resolve only behind an explicit flag |
| **D4** | Member becomes incompatible after late report update | Prefer conflict/unresolved + audit over silent membership shrink |
| **D5** | Snapshot window definition for watch | Time window by `received_time` and/or `event_time`, or “all reports since T0”; needs operational choice |
| **D6** | When to add Hub Event persistence | After Phase A/B proven in script |
| **D7** | Default lateness parameter values | Do not hardcode in design; set at implementation with documented defaults |
| **D8** | Keep one-Observation-per-EAR-per-Event (`ear_already_represented`) | **Keep** unless a separate design revisits multi-Observation same EAR on one Event |

### 11.2 Risks
| Risk | Mitigation |
| --- | --- |
| Growing membership weakens source separation | Keep all-members rule; unresolved multi; no silent merge |
| Large-N localization cost | Subset selection; do not couple localization N to association N |
| Watch performance on full snapshot rebuild | Accept for Phase A; optimize only after correctness |
| Stabilization too aggressive | Soft/hard lateness still allow `corrected` |
| Operators confuse associated vs localized counts | Mandatory separate counters in diagnostics |
| Changing freeze alters golden scenarios | Update expectations deliberately; keep B2.3/B2.4 tests intact |

---

## 12. Test strategy

Implement with the architecture (not in this design phase). Deterministic unit/integration tests:

| # | Scenario | Expect |
| --- | --- | --- |
| 1 | Fourth EAR after promote | Joins Event; same `event_id` |
| 2 | 100 associated EARs | Membership 100; localization contributors ≤5 (fixture-chosen 3) |
| 3 | Many reports / one EAR | Unique EAR count 1 |
| 4 | OOO START/PERIODIC/END | Identical assembly |
| 5 | Late compatible Observation | Joins; revision + optional `corrected` |
| 6 | Late incompatible | Does not join |
| 7 | Loc uses 3 of 100 | Contributor count 3; membership 100 |
| 8 | Localization outlier | Membership unchanged |
| 9 | Batch vs watch same snapshot | Identical `AnalysisResult` core fields |
| 10 | Duplicate sequences | No membership inflation |
| 11 | Shuffled report order | Identical final membership + localization |
| 12 | Two sources + ambiguous EAR | No auto-merge; unresolved or two Events |
| 13 | B2.3 withhold cases | Still withhold; Event can exist |
| 14 | B2.4 sensitivity | Still computed on localization revision |
| 15 | Restart + replay snapshot | Deterministic identical result |
| 16 | Stabilization then late join | `stabilized`→`corrected` with audit |

Regression: existing B2.3/B2.4/B2.5 suites remain green except tests that assert freeze behavior — those must be rewritten to assert growth.

---

## 13. Worked examples (design walkthrough)

### 13.1 100 EARs / 3–5 localize
Promote at 3 → `provisional` Event. Remaining compatible Observations join as the snapshot is analyzed. Localization selects best 3–5 bearings under B2.3/B2.4. Diagnostics: membership 100, contributors 3–5, exclusions listed.

### 13.2 Fourth EAR after promotion (`dense_four_ear`)
A,B,C promote → `provisional`. D all-members-compatible → **join** (today: frozen). Localization may use 3 or 4; membership always 4.

### 13.3 Minutes-late report
See §5.4 → `corrected` with membership/localization revisions; Event identity stable.

### 13.4 Competing hypotheses
See §6.5 → unresolved Observation; two Events remain; no Track merge.

### 13.5 Shuffled arrival
See §7.5 → complete snapshot + no freeze ⇒ order-independent finals.

---

## 14. Concise recommended architecture

1. **Event membership** = all associated Observations; **localization membership** = independent subset with its own revision.  
2. **Promotion creates** a provisional Event; **it does not freeze** membership. Growth keeps **B2.5 all-members** checks.  
3. Lifecycle **`provisional → stabilized → corrected`** with configurable receive-side quiet and soft lateness; never permanent reject-on-timeout.  
4. **`analyze(AnalysisSnapshot)`** is the single deterministic core; list/watch only differ in snapshot building and provisional labeling.  
5. Multi-compatible Observations default to **unresolved**, not first-match; no silent Event merges.  
6. Association remains acoustic/spatiotemporal and **transport-independent**.  
7. Implement Phase A (snapshot + grow + shared analyze + tests) before lifecycle polish, Hub persistence, or incremental optimization.  
8. Do **not** change B2.3/B2.4 gates or B2.5 scoring/thresholds in that work.

### Decisions that genuinely need human approval before coding
- **D1** Grow-after-promote (recommended) and whether to also defer promote within a snapshot pass.  
- **D2/D7** Stabilization / lateness parameter policy.  
- **D3** Unresolved-only vs optional auto-resolve for multi-compatible Observations.  
- **D4** Conflict handling when a late update breaks intra-Event compatibility.  
- **D5** Operational snapshot window for watch.  
- **D6** Timing of Hub Event persistence (recommend after A/B).

**B2.7 status:** design complete; ready for review. No implementation in this phase.
