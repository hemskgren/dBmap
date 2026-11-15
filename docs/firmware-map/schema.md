# Firmware Map schema

## Value classes

| Class | Meaning | Allowed as “limits we ship” |
| --- | --- | --- |
| `configured` | Explicitly set in current source, Kconfig default used by the build, compile-time `#define`, C default applied at init, or builder target metadata that the active build path uses | Yes — “what firmware uses” |
| `architectural_limit` | Structural bound the architecture permits or enforces (may equal configured, or be higher) | Yes — as architecture, not as measured performance |
| `proposed` | Design intent or unimplemented target | No — must not be treated as configured or verified |
| `measured` | Observed in a specific, cited test record | Yes — for that record only |
| `verified_operating` | Repeatable evidence under defined load (multiple records or explicit soak criteria met) | Yes — only after criteria are met |

**Rules**

1. Do not upgrade `proposed` → `configured` without an implementation and source citation.
2. Do not upgrade `measured` → `verified_operating` without repeatable criteria documented on the record(s).
3. Do not treat host (x86) timings as device (`esp32s3`) measurements.
4. Unknown / unverified: write `unknown` or `unresolved` rather than guessing.

## Required fields for capacity / limit rows

| Field | Purpose |
| --- | --- |
| Item | Human-readable name |
| Value | Number, string, or `n/a` / `not implemented` |
| Value class | One of the five classes above |
| Source | Path + symbol / Kconfig / builder key |
| Controls | One-line role |
| Binding | `compile-time` / `init-default` / `kconfig` / `builder-metadata` / `architectural` / `n/a` |

## Composition key

Comparable profiles and records should identify:

```text
product × hardware_profile × idf_version × firmware_version × platform_version
```

Builder target (`ear-esp32-s3`) is the build entry point for the EAR + N16R8 +
IDF composition documented here.

## Ownership

| Layer | May document | Must not absorb |
| --- | --- | --- |
| Platform | Slot model, seq, mic API, shared diag, future resource policy hooks | EAR RMS thresholds, CE create floor, product classification |
| Hardware | Chip, memory, pins, verified board facts | Product Observation policy |
| Product | EAR Manager, CE, AE product config, reporting intervals/schemas | Redefining Platform slot count as “EAR-only” without citing Platform |
| IDF | Toolchain pin and assessment status | Hardware flash size as if it were an IDF feature |
