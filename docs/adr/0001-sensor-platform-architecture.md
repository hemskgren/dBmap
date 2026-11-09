# ADR 0001: Shared Sensor Platform architecture

- **Status:** Accepted
- **Date:** 2026-11-09
- **Branch context:** `sensor-platform-architecture` (from `main` @ `d475bf4`, PR #42)
- **Scope:** Documentation decision only. No runtime, schema, firmware, or deployment behaviour change in this ADR.

## Context

dBmap today is a working local Hub plus Ear/Output firmware for acoustic sensing. The repository already contains reusable foundations (device identity, provisioning, MQTT, OTA, health, Observation ingest). Architecture reviews concluded that the long-term goal is a **reusable Sensor Platform**, with dBmap as the first product—not a one-off acoustic stack that other products must fork.

Future products under consideration include **N.O.S.E.** and **MON** (industrial/building monitoring, including Modbus). Deployments may be **Community** or **Enterprise**. Those environments must share one codebase and versioned artifacts.

## Decision

### 1. Shared Sensor Platform

We will evolve this repository into a **shared Sensor Platform**: one codebase and a versioned component ecosystem.

| Product | Role |
| --- | --- |
| **dBmap** | First product: acoustic monitoring, warnings, aircraft/drone and related sound events |
| **N.O.S.E.** | Future sensor product (own sensors and workflows) |
| **MON** | Future industrial/building monitoring, including Modbus and infrastructure sensors |

Products **consume** shared platform capabilities. Products **own** domain-specific interpretation, event meaning, and presentation. Platform code must not encode dBmap-only meaning (for example VMA/Hesa Fredrik recognition or helicopter/drone hypotheses).

### 2. Community and Enterprise are deployment profiles

**Community** and **Enterprise** are deployment profiles of the same versioned application code and component artifacts. They are **not** separate codebases and **not** long-lived git branches.

Differences are limited to:

- configuration and feature/module enablement
- identity providers and policy packs (for example OPA bundles)
- branding / frontend shells
- deployment topology and optional services

**Private and Enterprise installations must operate without Community services.** Community participation and sharing are optional and never a mandatory dependency of a standalone Hub.

### 3. Sensor component categories

Reusable work is organized by dependency direction (lower layers must not import product meaning):

| Category | Intent | Examples (illustrative) |
| --- | --- | --- |
| Hardware / communication drivers | Bus and peripheral access | I2S, INMP441; future Modbus RTU/TCP |
| Sensor acquisition | Sampling, buffering, windowing | PCM windows; future register polls |
| Generic signal processing | Reusable transforms without product labels | Acoustic RMS/FFT/L0–L2 libs |
| Device connectivity and lifecycle | Shared node client | Wi-Fi, MQTT, provision, OTA, health (`node_common` pattern) |
| Observation transport and infrastructure | Episode/report identity, publish, Hub ingest | Observation envelope, sequence dedup |

**Product-specific interpretation** (warnings, asset class hypotheses, industrial workflows) lives in product modules only. Do not introduce abstractions without a concrete second reuse case. In particular, do **not** add Modbus frameworks until MON needs them.

### 4. Observation architecture

Canonical concepts:

| Term | Meaning |
| --- | --- |
| **Device** | A registered node (Ear, Output, or future node kinds) with Hub-scoped identity |
| **Observation** | One device’s report episode (START/PERIODIC/END/COMPLETE); Hub-persisted today |
| **Track** | Continuity of a physical or pattern source on one device across Observation episodes (`track_id` when present) |
| **Event** | Hub-correlated multi-device (or multi-track) occurrence; **not** a Hub first-class entity yet |

The generic Observation **envelope** (protocol version, identity, context, spatial, message type) must support **multiple product schema families**. Acoustic L0/L1/L2 and `ear_hyp_v1` / `ear_ev_v1` / `ear_dbg_v1` are **dBmap-specific schemas**, not universal platform requirements. Future products must not be forced to provide acoustic fields.

**Current Hub extension** (optional `identity.track_id`, optional acoustic `assessment` / `evidence` / `debug`, `termination_reason: report_rotation`) remains **protocol v1** and **backward compatible** with firmware that omits those fields. Hub acceptance does not imply firmware already emits them, and does not imply Hub product interpretation of assessment families.

### 5. Identity and isolation (direction only)

Future direction, building on existing `users` / `external_identities` / groups / ownership foundations:

- stable **internal** user identity (UUID), independent of login provider
- multiple **external** identity providers linked to one user
- organization membership
- roles and permissions (OPA-oriented)
- explicit data-sharing grants
- tenant isolation for Enterprise
- Community identity and policies remain **optional**

**Not implemented by this ADR.** Ordinary-user login, OIDC, tenancy, and sharing remain future work. Local admin/viewer bearer credentials remain the implemented access path.

### 6. Architecture evolution

Prefer **logical module boundaries** inside the current Hub monolith and firmware component tree before physical service splits. Avoid premature microservices, schema registries, plugin frameworks, message buses, or speculative Modbus abstractions.

## Consequences

### Positive

- Clear ownership: platform vs product vs deployment profile
- Preserves standalone private Hub operation
- Gives N.O.S.E. / MON a path that does not inherit acoustic dependencies
- Documents that existing `ear_*` Hub fields are dBmap schemas on a shared envelope

### Trade-offs / follow-through

- Naming (`DBMAP_*`, `esp-ear`, UI “dBmap”) remains product-branded until rename is justified by a second product
- Observation contract still requires L0 `measurements` for current Ear payloads; making the envelope fully schema-pluggable is later work
- Event/Track Hub APIs remain future; scripts continue as the verified correlation path today

## Decisions recorded vs unresolved

See [Sensor Platform architecture](../sensor-platform-architecture.md#decisions-versus-unresolved).

## References

- [Sensor Platform architecture](../sensor-platform-architecture.md) (diagram, terminology, next task)
- [Protocol](../protocol.md) (Observation v1; Track ID; reserved Event ID)
- [Identity and operating modes](../identity-and-operating-modes.md) (current identity behaviour)
- [Platform readiness review](../platform-readiness-review.md) (historical Hub/Output readiness checklist)
