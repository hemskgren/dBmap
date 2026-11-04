# L2 Hub Compatibility Preparation

**Status:** Investigation and implementation planning only. **No Hub, schema, firmware, or test code was modified for this document.**  
**Authoritative contract:** [`ear-l2-acoustic-dynamics-contract.md`](./ear-l2-acoustic-dynamics-contract.md)  
**Goal:** Accept optional `acoustic.dynamics` on protocol-v1 Observations without breaking existing L0/L1/classification consumers, before any Ear publication of non-null dynamics.

---

## 1. Current Hub EAR observation path

```text
MQTT (…/nodes/{ear}/observation, QoS1, retain=false)
  → mqtt_listener._on_message
  → mqtt_listener._ingest_observation
       ObservationBody.model_validate_json(payload)     # Pydantic; extra=forbid
  → service.ingest_observation(db, topic_node_id, body)
       identity/topic checks, Ear+active lifecycle
       dedup (node_id, sequence_number)
       persist Observation.payload_json = body.model_dump_json(by_alias=True)
  → (optional) schedule_weather_ensure(event_time)

Read path:
  GET /api/v1/observations → service.list_observations
       json.loads(payload_json) → ObservationView(**payload, received_time_utc=…)
       (re-validates through ObservationBody / ObservationAcoustic)
```

| Stage | File / symbol | Role |
| --- | --- | --- |
| MQTT subscribe / route | `hub/bootstrap/app/mqtt_listener.py` — `_on_message`, `_ingest_observation` | Reject retained; parse topic Ear id; validate JSON |
| Schema | `hub/bootstrap/app/schemas.py` — `ObservationBody`, `ObservationAcoustic`, nested L0/L1/classification | Strict hierarchical v1 |
| Ingest / persist | `hub/bootstrap/app/service.py` — `ingest_observation` | Dedup + store full JSON blob |
| ORM | `hub/bootstrap/app/models.py` — `Observation` | Columns: ids, times, **`payload_json` Text** (no per-feature columns) |
| HTTP API | `hub/bootstrap/app/api.py` — `GET /api/v1/observations`, `…/count` | Returns `ObservationView` |
| OpenAPI | Regenerated via `scripts/export-hub-openapi.py` → `docs/hub-api.openapi.yaml` | Mirrors Pydantic |

### Where `acoustic.classification` lives today

| Layer | Definition |
| --- | --- |
| Pydantic | `ObservationClassification` in `schemas.py` (`extra="forbid"`, `schema` alias → Literal `"l2"`, `version`, `source_family`, `confidence`, `hints`) |
| Parent | `ObservationAcoustic.classification: ObservationClassification \| None = None` |
| Persistence | Nested inside `payload_json` only (no dedicated DB column) |
| Tests | `tests/test_service.py` (null classification, confidence bounds); fixtures often include a sample classification object |

`ObservationAcoustic` currently:

```text
signal_level_dbfs, measurements (required L0), signature (optional L1), classification (optional)
```

**There is no `dynamics` field.** Because `extra="forbid"`, any Ear payload that includes `acoustic.dynamics` (even `null` if treated as an unknown key—see note below) **will fail validation today** once the key is present as an unknown property. Omitted `dynamics` remains valid.

Pydantic v2: a model without field `dynamics` rejects `{"dynamics": null}` under `extra="forbid"`. So Hub **must** add the optional field before firmware publishes either a null or an object under that key if the serializer always emits the key. (Firmware that omits the key entirely continues to work without Hub changes.)

---

## 2. Additive `acoustic.dynamics` evaluation

Contract placement (unchanged protocol_version **1**):

```text
acoustic.measurements      schema "l0"   (required)
acoustic.signature         schema "l1" | null
acoustic.dynamics          schema "l2" | null   ← NEW
acoustic.classification    schema "l2" | null   ← EXISTING hypothesis object; semantics unchanged
```

**Naming note:** both `classification` and `dynamics` use JSON `"schema": "l2"`, but they are **different objects** with different fields. Hub must keep separate Pydantic types (e.g. `ObservationL2Dynamics` vs `ObservationClassification`). Do not merge them.

### Compatibility checklist (contract vs current Hub)

| Requirement | Current Hub | Gap |
| --- | --- | --- |
| Payload **without** `dynamics` key | Accepted | None — keep optional field default `None` |
| Explicit `dynamics: null` | **Rejected** today (`extra=forbid`) | Add `dynamics: ObservationL2Dynamics \| None = None` |
| Valid L2 dynamics object | Rejected | New nested models + validators |
| Unknown keys inside dynamics / acoustic | Rejected (`forbid`) | Keep `extra="forbid"` on new models |
| `classification` unchanged | Present | Do not alter `ObservationClassification` |
| Protocol version stays 1 | `Literal[1]` | No change |
| Persist without lossy transform | Full JSON in `payload_json` | No DB migration if nested object stored as-is |

### Validation Hub should implement (from contract)

Authoritative field list: contract § Field table. Summarized for Hub models:

**Object meta (required when object present):**  
`schema="l2"`, `version` (recommend accept `ge=1`; see unresolved version pin), `sample_rate_hz>0`, `analysis_window_ms>0`, `fft_size>0`, `freq_min_hz≥0`, `freq_max_hz>0`, `frame_hop_samples>0`, `window_count≥0`, `invalid_window_count≥0`, `history_overflow: bool`.

**Per feature** (`spectral_flux`, `envelope_cv`, `dominant_delta_hz`):  
nested object with `valid_count≥0`, `mean`/`stdev` float|null, `allow_inf_nan=False`; means ≥0 when non-null; stdev ≥0 when non-null.

**Cross-field (model_validator recommended):**

1. If `history_overflow` is true → `window_count==0`, `invalid_window_count==0`, every feature `valid_count==0`, all means/stdevs null.
2. If `valid_count==0` → mean and stdev null.
3. If `valid_count==1` → stdev null; mean may be non-null.
4. If `valid_count≥2` → mean and stdev may be non-null (stdev required non-null only if contract treated as soft; recommend require stdev non-null when count≥2 for consistency with published Ear objects).
5. Reject unknown keys; reject classification-shaped fields inside dynamics.

---

## 3. Overflow handling on Hub

| Case | Contract signal | Hub duty |
| --- | --- | --- |
| Trusted empty interval | `history_overflow: false`, `window_count: 0`, metrics null | Accept; treat as no eligible windows |
| Ordinary invalids only | `history_overflow: false`, `window_count: 0`, `invalid_window_count>0` | Accept |
| Overflow | `history_overflow: true`, count placeholders 0, metrics null | Accept **if** cross-field rules hold; **never** interpret as empty/idle |

Storage: preserve the object verbatim in `payload_json`. Do **not** strip `history_overflow` or coerce overflow rows into a synthetic empty dynamics omission.

Analytics / scripts: any future Hub-side summarizer that reads dynamics must branch on `history_overflow` first (Option A). Current `hub_event_track.py` / map UI do not consume dynamics yet — low immediate risk, but document the rule for later consumers.

---

## 4. Compatibility and persistence

| Area | Change needed? | Notes |
| --- | --- | --- |
| DB schema / migrations | **No** (preferred) | Nested JSON in `observations.payload_json` already |
| Pydantic ingest/API models | **Yes** | Core work |
| `list_observations` / `ObservationView` | **Yes** (inherits Body) | Rehydrate must know `dynamics` or stored new payloads fail on read |
| OpenAPI export | **Yes** | Regenerate after schema change |
| Frontend (`web/index.html`, `web/map/index.html`) | **Optional / later** | Uses `signal_level_dbfs` / classification only; ignores unknown keys in JS — will not reject; may later display dynamics |
| `scripts/hub_event_track.py`, bearing helpers | **No required** | Read classification; ignore dynamics unless extended |
| Simulator | **Later** | Can remain omitting dynamics until Hub ships |
| Firmware publish | **Blocked** until Hub accepts field | Per contract rollout |
| MQTT protocol_version | **Unchanged** (1) | Additive optional field |

**Consumers that would reject/discard today:** Hub MQTT ingest (validation fail → drop). API clients using generated OpenAPI types may need regen after Hub ships. Strict typed clients that forbid unknown acoustic keys would need updating — Hub itself is the gate.

---

## 5. Exact files that would need modification (when approved)

| File | Change |
| --- | --- |
| `hub/bootstrap/app/schemas.py` | Add `ObservationL2FeatureStats` (with `valid_count`), `ObservationL2Dynamics`; extend `ObservationAcoustic` with `dynamics: … \| None = None`; overflow/count validators |
| `hub/bootstrap/tests/test_observation_l0.py` (and/or new `test_observation_l2_dynamics.py`) | Test matrix below |
| `hub/bootstrap/tests/test_service.py` | Regression ingest round-trip with/without dynamics; classification+dynamics together |
| `docs/hub-api.openapi.yaml` | Regenerate via `scripts/export-hub-openapi.py` |
| `docs/protocol.md` | Short note that `acoustic.dynamics` may be null/object (editorial; after Hub lands) — optional in same PR |

**Not required for Hub acceptance alone:** `models.py`, DB migrations, `mqtt_listener.py` routing (unless fixing unrelated attribute bugs), firmware, Kconfig.

---

## 6. Proposed implementation plan (dependency order)

1. **Schemas** — Add L2 dynamics models + `ObservationAcoustic.dynamics` optional; keep classification untouched; `extra="forbid"` everywhere.  
2. **Cross-field validators** — Overflow Option A; valid_count ↔ mean/stdev rules.  
3. **Unit tests** — Matrix §7 (validation-only first).  
4. **Ingest round-trip tests** — `ingest_observation` + `list_observations` preserve dynamics JSON.  
5. **Regenerate OpenAPI**.  
6. **Deploy Hub** before any Ear `DBMAP_EAR_OBS_L2_PUBLISH`.  
7. **Only then** firmware publish gate (separate approval; out of scope here).

Do not start firmware publication or L2 compute in this Hub track.

---

## 7. Test matrix (proposed)

| # | Case | Expected |
| --- | ---: | --- |
| T1 | Existing L0 (+ optional L1) payload **without** `dynamics` key | Accept; `dynamics is None` |
| T2 | Explicit `dynamics: null` | Accept; `dynamics is None` |
| T3 | Valid L2 dynamics object (contract “fully valid” example shape) | Accept; fields parse; `schema=="l2"` |
| T4 | Dynamics object with `"schema": "l1"` or other non-`"l2"` | **Reject** |
| T5 | Unsupported version | **Reject** `version < 1`; policy for `version > 1` — see unresolved Q1 |
| T6 | Missing required meta (e.g. no `history_overflow` / no `frame_hop_samples`) | **Reject** |
| T7 | Bad types / ranges (negative `window_count`, `mean: -1` for flux, non-finite float) | **Reject** |
| T8 | Overflow object: `history_overflow: true`, counts 0, null metrics | **Accept**; assert not conflated with empty (`history_overflow` true) |
| T8b | Overflow with `window_count: 5` or non-null mean | **Reject** (cross-field) |
| T9 | `classification` object **and** `dynamics` object together | **Accept**; both independent |
| T10 | Unknown key under `dynamics` or under `acoustic` (e.g. `dynamics.foo`) | **Reject** |
| T11 | Regression: protocol v1 L0-only / L1 null / classification null fixtures from `test_observation_l0.py` / `test_service.py` | Still **Accept** |
| T12 | Ingest + list round-trip of T3 | Persisted JSON retains dynamics; list returns equivalent |

---

## 8. Compatibility risks and unresolved questions

| ID | Topic | Notes |
| --- | --- | --- |
| **Q1** | Pin `dynamics.version` to `1` only vs `ge=1` like L1 | Contract initial package is 1. Pinning `le=1` makes T5 “unsupported version 2” crisp; `ge=1` only matches L0/L1 style. **Needs human choice before coding.** |
| **Q2** | Require `stdev` non-null when `valid_count≥2` | Contract says stdev present when n≥2 for published objects; Hub can enforce or allow null. Prefer enforce for ingest strictness. |
| **Q3** | Schema name collision (`"l2"` on classification and dynamics) | Safe if types stay separate; document for OpenAPI readers. |
| **Q4** | Downstream scripts ignoring `history_overflow` | Future risk if someone averages `dominant_delta_hz.mean` without the flag. |
| **Q5** | Ear serializers that always emit `"dynamics": null` | Require Hub deploy first (same as signature pattern). |
| **Q6** | Unrelated: `mqtt_listener` logs `body.observation_id` / `body.event_time_utc` | Hierarchical body uses `identity` / `context`; may AttributeError on success path logging/weather. **Out of L2 scope** but can affect soak tests — flag for separate fix. |

---

## 9. Confirmation

**No files were modified** to implement Hub L2 acceptance. This document is planning only.

Wait for approval before changing `schemas.py`, tests, OpenAPI, or any other source.
