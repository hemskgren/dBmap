# Platform Firmware Map

Traceable index of **Sensor Platform**, **hardware**, **product**, and **ESP-IDF**
composition for dBmap firmware. The map does **not** replace Kconfig, compile-time
`#define`s, builder target metadata, or runtime defaults. Those remain the
authoritative configuration sources. This tree classifies and cites them.

**First documented composition**

| Axis | Value |
| --- | --- |
| Product | dBmap EAR |
| Hardware profile | `esp32-s3-n16r8` |
| Builder target | `ear-esp32-s3` |
| Primary ESP-IDF | `v5.4.4` |

## Document map

| Path | Role |
| --- | --- |
| [schema.md](./schema.md) | Value classes and documentation rules |
| [platform/capabilities.md](./platform/capabilities.md) | Sensor Platform capabilities (product-independent) |
| [hardware/esp32-s3-n16r8.md](./hardware/esp32-s3-n16r8.md) | Hardware profile |
| [idf/v5.4.4.md](./idf/v5.4.4.md) | Active IDF pin and authority |
| [products/dbmap-ear/profile.md](./products/dbmap-ear/profile.md) | EAR product profile |
| [products/dbmap-ear/capacities.md](./products/dbmap-ear/capacities.md) | Verified capacities and limits with sources |
| [records/TEMPLATE-perf-record.md](./records/TEMPLATE-perf-record.md) | Performance evidence template |
| [records/](./records/) | Filled performance records (none migrated yet; see below) |

Normative Platform architecture: [Sensor Platform architecture](../sensor-platform-architecture.md),
[ADR 0001](../adr/0001-sensor-platform-architecture.md).

## Three levels (do not collapse)

1. **Sensor Platform** — generic infrastructure and invariants (episode registry
   model, sequence allocator, mic acquisition APIs, shared diag).
2. **Hardware profile** — board/chip capabilities and constraints (flash, PSRAM,
   clocks, I2S/mic wiring facts supported by repo sources).
3. **Product profile** — EAR policies and product modules (Obs Manager, CE, AE,
   RMS thresholds, reporting schemas).

EAR-specific settings stay in the product profile even when they are numerically
interesting. Do not move them into Platform documentation to appear “generic.”

## Value classes

Every numeric or status claim must use exactly one class from
[schema.md](./schema.md): `configured`, `architectural_limit`, `proposed`,
`measured`, or `verified_operating`. These are not interchangeable.

## Proposed architecture (not configured)

See [ADR 0003](../adr/0003-critical-observation-path-and-shared-evidence.md) (**Proposed**).
Firmware Map product docs may list matching rows with value class **`proposed`**
only. They are **not** implemented, configured, measured, or verified.

| Topic | Map / ADR status |
| --- | --- |
| Critical Observation Path | Lifecycle in source (detection disabled); ADR 0003 still Proposed |
| Critical active capacity (1 per EAR) | `proposed` — not configured |
| Critical overflow (Obs vs evidence capacity) | `proposed` in ADR 0003 — sizes unresolved; prefer overflow evidence (B) |
| Critical trigger latency | Completed-window MVP; measure four intervals — not measured |
| `context.origin` / `critical_path` | Hub schema + EAR payload accept `critical_path`; auto-detect still disabled |
| DoA / TDOA | Not implemented; any interval (e.g. 3 s) is `proposed` lab intent only |
| Analysis Scheduler / Resource Manager | Future Platform — not implemented |
| ESP-IDF migration | Unchanged; primary remains v5.4.4 |

Stereo pair ΔRMS / Δpeak is **not** direction estimation
([AE types](../../firmware/products/dbmap/ear/audio_engine/include/ae_types.h)).

## Performance records

Use [records/TEMPLATE-perf-record.md](./records/TEMPLATE-perf-record.md). Reuse
existing UART/diag instrumentation; do not invent measurements.

**M3 migration status:** no existing L1/L2 diagnostic write-ups qualified for
migration into a filled record (missing git SHA, firmware/platform versions,
test date/duration, and/or device soak completeness). See
[records/README.md](./records/README.md).

## Maintenance rules

- Cite source file + symbol / Kconfig / builder key for every `configured` value.
- Prefer linking to diagnostics docs over copying large tables.
- When source and older docs disagree, record the discrepancy; do not silently
  “fix” it in the map.
- Keep ESP-IDF and hardware as separate comparison axes.
