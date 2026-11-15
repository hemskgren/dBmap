# Product profile: dBmap EAR

EAR-specific modules and policies on top of Sensor Platform + hardware
`esp32-s3-n16r8`. Capacities and thresholds with citations:
[capacities.md](./capacities.md).

## Composition

| Axis | Value | Source |
| --- | --- | --- |
| Product | dBmap EAR | `firmware/products/dbmap/ear/` |
| App project | `firmware/esp-ear` | builder target `ear-esp32-s3` |
| Hardware profile | `esp32-s3-n16r8` | [hardware profile](../../hardware/esp32-s3-n16r8.md) |
| ESP-IDF | `v5.4.4` | [idf/v5.4.4.md](../../idf/v5.4.4.md) |
| Platform SemVer | `0.2.0` (file) | `firmware/platform/VERSION` |
| Product firmware series base in defaults | `0.8.0` string in sdkconfig.defaults | `CONFIG_DBMAP_FIRMWARE_VERSION` — catalog builds bump; do not treat defaults string as the only fleet version |

## Module ownership (tree)

| Module | Role | Tree path |
| --- | --- | --- |
| Platform mic / node_common / observation | Shared infrastructure used by EAR | `firmware/platform/` |
| Audio Engine (AE) | Signatures + history from Platform audio | `…/ear/audio_engine/` |
| Candidate Engine (CE) | Candidate tracks; create/match defaults | `…/ear/candidate_engine/` |
| EAR acoustic L0/L1/L2 | Product measurement/feature path | `…/ear/ear_acoustic/` |
| EAR Observation + Manager | Promotion-gated Obs lifecycle, RMS detector, MQTT publish | `…/ear/ear_observation/` |

AE/CE architecture narrative:
[docs/architecture/audio-and-candidate-engine.md](../../../architecture/audio-and-candidate-engine.md).

## Product policies (configured today)

Documented in detail in [capacities.md](./capacities.md):

- RMS Activity Detector ENTER/EXIT and PERIODIC interval (Kconfig).
- CE create floor `min_rms_dbfs`, match distance, coast/enter counts (C defaults).
- EAR Obs Manager binding / reservation / pending limits (compile-time).
- Promotion signature snapshot cap (compile-time).
- L0/L1/L2 enable and publish flags (`sdkconfig.defaults` + Kconfig).

**Keep separate:** CE `min_rms_dbfs` (−80) vs RMS ENTER/EXIT (−45/−55). Platform
episode slots (2) vs CE candidates (8) vs Manager bindings (2).

## Reporting

- Observation protocol v1 hierarchical envelope ([protocol.md](../../../protocol.md)).
- Publish helper: `dbmap_ear_observation_publish` in `ear_observation.c`.
- Optional Hub-accepted product schemas (`assessment` / `evidence` / `debug`) —
  Hub ready; firmware emit of those blocks is not required for Hub compatibility
  ([sensor-platform-architecture.md](../../../sensor-platform-architecture.md)).

## Explicitly not implemented (see proposed ADR)

| Topic | Map status |
| --- | --- |
| Critical Observation Path | Lifecycle in source (`ear_critical_path`); auto-detect **disabled**. [CRITICAL_PATH.md](../../../../firmware/products/dbmap/ear/ear_observation/CRITICAL_PATH.md); ADR 0003 still **Proposed** |
| Critical capacity dimensions | Active Obs = 1 in source; overflow = drop+count (A); pending/B unresolved |
| Reporting origin (`context.origin`) | Hub + EAR payload accept `critical_path`; omit on normal reports |
| DoA / TDOA | Not implemented — pair ΔRMS ≠ direction; no configured DoA interval |
| Analysis Scheduler / Resource Manager / Tier policy | Not implemented (future Platform + product hooks) |
| PAUSE / RESUME Observation lifecycle | Not in protocol `emission_kind` set today; Critical must not depend on PAUSE |

Do not promote ADR proposals to `configured` in [capacities.md](./capacities.md) until source exists.
