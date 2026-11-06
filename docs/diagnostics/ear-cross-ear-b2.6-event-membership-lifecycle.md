# B2.6 – Temporal ingestion, late reports and Event membership lifecycle

**Phase type:** architecture and implementation audit + design proposal.  
**Not implemented in this phase.** Association scoring, promotion thresholds, B2.3 gates, B2.4 sensitivity, Hub APIs/schemas, and Track are unchanged. No commit.

---

## Target principle (adopted for design)

**Event membership** = full set of associated Observations (and unique observing EARs).  
**Localization membership** = subset of measurements used for the current position estimate.  

These must remain independent. An Event must not be capped by how many EARs are needed to promote or localize it.

Illustrative (not thresholds): 100 associated EARs / Observations, 5 with useful bearings, 3 used in the current fix — all 100 remain Event evidence.

---

## 1. Files inspected and changed

### Inspected
- `scripts/hub_event_track.py` (`list` / `watch`, fetch, `report`, `observation_diff`)
- `scripts/lib/cross_ear_event_correlation.py` (assembly, join, promotion freeze — B2.5)
- `scripts/lib/cross_ear_localization.py` (contributor / outlier subset)
- `hub/bootstrap/app/models.py` (`Observation` table)
- `hub/bootstrap/app/service.py` (`ingest_observation`, `list_observations_page`)
- `hub/bootstrap/app/schemas.py` (`ObservationContext`, `ObservationView`)
- B2.1–B2.5 diagnostics and tests

### Changed
- This document only (`docs/diagnostics/ear-cross-ear-b2.6-event-membership-lifecycle.md`).

No code, API, schema, or association-rule changes.

---

## 2. Verified current `list` and `watch` behavior

| Aspect | `list` | `watch` |
| --- | --- | --- |
| Fetch | One Hub page (`--page`, `--page-size` ≤500, default 200) | Initial page + later polls: **page 1, size 500, `order=newest`** |
| Ordering | `received_time_utc` newest/oldest | Always newest page |
| Full history | **No** — only the selected page unless the operator pages manually | **No** — only newest ≤500 reports |
| Association state | Stateless; one-shot `report()` | **No persistent Events**; on count change, re-runs `report(current)` |
| What is reanalyzed | N/A | Entire **current page**, not “new reports only” |
| `observation_diff` | N/A | Used to detect/log new report keys; **does not** feed incremental correlation |
| Nodes / weather | Fetched once | Nodes **not** passed on update cycles (`include_site_context=False`) → Cross-EAR geometry/association without installation metadata on updates |
| Restart | Fresh | Fresh; `known_ids` lost |
| Output | Full text analysis | Same analysis on newest page; site/weather omitted on updates |

**Verified:** `list` and `watch` are **not** equivalent end-to-end processors. Both are page-bounded snapshots. Watch does **not** maintain incremental Candidate/Event state; it re-correlates the latest page from scratch when the Hub count changes.

---

## 3. Verified timestamp semantics

| Name | Exists today? | Meaning |
| --- | --- | --- |
| **event_time** | Yes — `context.event_time_utc` (payload) and `observations.event_time_utc` (column) | Acoustic interval time from the EAR (timezone-required). Used for assembly intervals, association temporal checks, localization sample times. |
| **received_at** | Yes — `observations.received_time_utc` / API `received_time_utc` | Hub ingest time (`default=utcnow` on insert). Used for **list/watch pagination order**, not for association chronology. |
| **processed_at** | **No** dedicated field | Processing is ephemeral in `hub_event_track` (no store). |

They are **not interchangeable**. Association/localization use **event_time**. Fetch windows use **received_at**.

Hub dedup: unique `(node_id, sequence_number)`. Duplicate delivery returns `False` without a new row. Same sequence with different `observation_id` → error.

No Hub “processed_at”; re-running the script reprocesses the same stored reports.

---

## 4. Event freeze — root cause and proposed solution

### Verified root cause (B2.5)
After `maybe_promote`, Candidate status becomes `Promoted`. Later Observations that are pairwise-compatible with all Event members are refused with:

`accepted_but_event_frozen` —  
“compatible with Event members but Event updates are not applied in this experimental pass.”

This is an **intentional experimental limitation / implementation shortcut** in script-local correlation, **not** a Hub schema limit (Hub has **no Event table** today) and **not** caused by pairwise incompatibility in the four-EAR case.

### Proposed target (not implemented)
1. Keep a single Event identity after first promotion.  
2. Allow **post-promotion membership growth** when a new Observation passes the same association predicates against current members (or an explicitly approved policy).  
3. Record membership additions with timestamps (`event_time`, `received_at`, optional process time).  
4. Recalculate localization as a **subset** of members; never delete association membership because localization excluded a bearing.  
5. Prefer **deferring promotion** until batch end *or* allowing growth after promote — both achieve four-EAR completeness; growth after promote also serves late arrivals in watch/replay.

Open decision: grow-after-promote vs defer-promote-until-stable-window (see §14).

---

## 5. Proposed Event membership model

### Concepts (prefer fields on existing experimental types; Hub Event entity is future)

| Concept | Proposed representation | Notes |
| --- | --- | --- |
| Event-associated Observations | Ordered set of `observation_id` (+ `node_id`) on the Event | Full association membership |
| Observing EARs | Unique `node_id` derived from associated Observations | Count ≠ report count |
| Localization contributors | Localization result `contributing_ears` + selected samples / non-outlier pairs | Subset; may be empty |
| Excluded/rejected evidence | Join/attempt diagnostics + localization outlier/exclusion notes | Reasons required |

### Counts to keep distinct
- Report count  
- Unique Observation count  
- Unique EAR count  
- Bearing sample count  
- Localization contributor count  

### Existing structures
- Script-local `CorrelatedEvent.members` today = association membership **and** localization input set (coupled).  
- Localization already can mark pair outliers without removing members — but Event cannot grow after promote.  
- Hub DB stores **reports only**; no Event/Candidate persistence.

### Future schema (listed separately in §12)
If Hub persists Events: association membership table vs localization revision table. **Not in this phase.**

---

## 6. Separation of association vs localization

### Verified today
- Association and localization are separate **scores/gates**.  
- Localization outliers do not remove Candidate/Event members.  
- Missing/unsuitable bearings can still associate (bearing abstain).  
- **But** Event membership is frozen at promote, so late associated-eligible Observations never become members.

### Target
- Association membership grows with compatible Observations only.  
- Localization may use 3 of 100 EARs, or withhold position entirely, without shrinking association membership.  
- Preserve B2.3 withhold gates and B2.4 sensitivity; no calibrated σ.

---

## 7. Late-report and allowed-lateness proposal

### Observed late/OOO cases (assembly capability)

| Case | Current assembly behavior |
| --- | --- |
| PERIODIC before START | Grouped by `(node_id, observation_id)`; emission counts track kinds; order of arrival does not redefine identity |
| END before PERIODIC / START after END | Same; `lifecycle_complete` when start+end or complete present |
| Late report extends Observation | Re-assembly over full report set extends interval min/max by **event_time** |
| Duplicate sequence | Hub rejects second insert |
| Valid event_time, late received_at | Stored; list/watch may miss it if outside newest page |
| Report after prior `list` | Next `list`/`watch` fetch can see it if still in page window |

Assembly is already largely **event-time based** within the reports it is given. The gap is **which reports are fed in** (pagination / watch window) and **Event freeze**.

### Proposed lifecycle states (script-local or future Hub)

| State | Meaning |
| --- | --- |
| `provisional` | Promoted; still accepting compatible late Observations |
| `stabilized` | Normal completion window elapsed (policy-based) |
| `corrected` | Evidence after stabilization caused a documented membership and/or localization revision |

**Stabilization policy options (open decision):**
- Wall-clock since last membership change (`received_at` based), and/or  
- Time since last associated Observation’s `event_time`, and/or  
- Explicit operator finalize  

Timeout must **not** mean “reject forever.” Post-stabilization arrivals → `corrected` with audit trail, or quarantine as unresolved association.

**Configurable allowed lateness:** e.g. max(`received_at - event_time`) or max age since Event create — **do not hardcode** in this phase; define as a parameter when implementing.

---

## 8. Batch/replay vs watch equivalence

### Verified gap
Given the **same complete report set**, current tools do **not** guarantee convergence:

1. Both modes often see **incomplete** pages.  
2. Watch re-runs correlation on newest-500 without nodes on updates.  
3. No durable Event identity across cycles.  
4. Promote-then-freeze means **arrival order of Observations into a single in-memory batch** changes whether the fourth EAR joins (B2.5): early promotion freezes out late-compatible Ears even inside one complete batch.

### Target requirement
Same complete report set → same final:
- Observation assemblies  
- Event association membership  
- Localization contributor selection (deterministic ties)  
- Localization status/position (subject to B2.3/B2.4)

Watch may show **provisional** fewer members; replay of the full set must match the eventual watch result after all reports are included.

### Smallest architecture change (proposed)
1. **Input layer:** always analyze a **complete snapshot** (paginated fetch-all by `observation_id` / time window / snapshot_id), not a single newest page.  
2. **Correlation layer:** either defer promotion until end of snapshot, or allow post-promotion growth with deterministic member set (e.g. sort Observations by `(interval_start, node_id, observation_id)` then apply joins without freeze, promote once).  
3. **Watch:** maintain or rebuild from full snapshot on each change; optional provisional UI vs final replay flag.  
4. **Determinism:** fixed sort keys already exist for assembly/correlation ordering — keep them; remove order-dependent freeze.

---

## 9. Wi-Fi now vs LoRaWAN later

| Factor | Wi-Fi LAN (now) | LoRaWAN (later) |
| --- | --- | --- |
| Latency | Usually low | Highly variable; batch uplink |
| Ordering | Often near-chronological | Frequently OOO / batched |
| Duplicates | Retries possible | Retries common |
| Capacity | High | Limited; partial Observation delivery |
| Implication | Freeze + newest-page watch already hurts | Same bugs become severe |

**Design rule:** transport latency must not redefine `event_time` or Observation identity. Association stays acoustic/spatiotemporal, not transport-aware. Allowed-lateness and complete-snapshot replay are the transport-agnostic fixes. No LoRaWAN code in this phase.

---

## 10. Large-scale membership feasibility (100+ EARs)

### Today
- In-memory lists; pairwise join against all members → O(members) per join attempt; Candidate build is fine for hundreds if snapshots are complete.  
- Localization uses combinations of members — already costly at large N; **must** stay a subset selector.  
- Hub report store scales with reports; no Event store.  
- Text diagnostics will need summaries (counts + top contributors), not full pairwise dumps by default.

### Feasible without arbitrary localization-based caps
Yes for association membership at ~100 EARs in script-local memory, if:
- Membership is a set keyed by `(node_id, observation_id)`  
- Localization runs on a selected subset (geometry/quality filters)  
- Diagnostics are aggregated  

Bottlenecks to watch later: full pairwise re-eval on every late join; printing; Hub API page limits for replay.

---

## 11. Multi-source association risks

Growing membership must **not** weaken separation (B2.5):

- Still require compatibility (proposed: keep **all-pairs with current members** unless a future clique policy is explicitly approved).  
- No unrestricted transitive association.  
- No silent merge of competing Events.  
- One Observation → at most one Event (deterministic arbitration if multi-compatible: e.g. highest score, earliest Event id, or leave unresolved).  
- Same Ear, new Observation after gap: new association decision, not automatic append.

Open decision: multi-compatible Observation arbitration.

---

## 12. Required API/schema changes (separate; not in this phase)

| Change | Needed for | Phase |
| --- | --- | --- |
| None for script-local growth-after-promote / defer-promote | Experimental `hub_event_track` only | Implementation follow-on |
| Fetch-all / windowed complete snapshot helper | list/watch equivalence | Follow-on |
| Hub Event + membership tables | Durable Events, lifecycle across restarts | Future Hub product |
| `processed_at` (optional) | Audit of analysis runs | Optional |
| Localization revision table | Versioned contributors | Future |

**This audit does not require schema changes to proceed with a script-local design review.**

---

## 13. Test results and test designs

### Executed this phase
No new production behavior; prior suites remain green from B2.5 (`96 passed` last run). No B2.6 implementation tests added (architecture not implemented).

### Required deterministic tests (design — implement with the architecture)

1. Four-EAR: fourth compatible EAR joins after promotion (or before final promote if defer strategy).  
2. Event membership can hold 100 EARs; localization uses ≤3 contributors.  
3. Multiple reports per EAR → unique EAR count = 1.  
4. OOO START/PERIODIC/END → same assembled Observation.  
5. Late compatible Observation joins existing Event.  
6. Late incompatible Observation does not join.  
7. Localization subset of 3 from 100-member Event.  
8. Dropping a localization contributor / outlier does not remove Event membership.  
9. Batch complete snapshot vs incremental watch converge on same final membership + localization.  
10. Duplicate/retry sequences do not inflate membership.  
11. Restart + replay deterministic given same snapshot.  
12. Two distinct sources not auto-merged.  
13. B2.3 withhold intact.  
14. B2.4 sensitivity intact.

---

## 14. Recommended implementation phases

### Phase A — Script-local (no Hub schema)
1. Complete-snapshot fetch for analysis (paginate until done / snapshot_id).  
2. Remove Event freeze **or** defer promotion to end of snapshot (choose one in review).  
3. Explicit association membership vs localization contributor lists in diagnostics.  
4. Watch rebuilds from full snapshot (provisional banner allowed).  
5. Tests 1–11, 13–14.

### Phase B — Lifecycle policy
1. Configurable allowed lateness + `provisional` / `stabilized` / `corrected`.  
2. Revision audit log (membership add, localization rev).  

### Phase C — Hub persistence (product)
1. Event/membership/localization-revision tables.  
2. API read models.  
3. Restart-safe state.  
4. Still no Track until multi-hypothesis work is scoped.

### Open architectural decisions (need review before coding)
1. **Grow-after-promote** vs **defer-promote-until-batch-end** (or both).  
2. Stabilization clock: `received_at` vs `event_time` vs hybrid.  
3. Multi-compatible Observation arbitration.  
4. Whether join remains all-pairs-with-members forever.  
5. When Hub persistence becomes mandatory vs script-local-only.

---

## Clarity map

| Verified current behavior | Proposed target | Open decision |
| --- | --- | --- |
| Promote then freeze | Membership can grow; localization stays a subset | Grow vs defer promote |
| list/watch page-bounded, watch re-correlates newest page without nodes | Complete snapshot; watch≈replay final | Snapshot window definition |
| event_time ≠ received_at; no processed_at | Keep distinct; optional process audit | Persist processed_at? |
| No Hub Events | Optional durable Events later | When to add schema |
| Association predicates (B2.5) | Keep; don’t silently broaden | Arbitration / all-pairs |

---

## B2.6 status

**Complete as an audit and design proposal.**  
**Not implemented.** Awaiting review of open decisions before any association lifecycle or watch/list equivalence work.
