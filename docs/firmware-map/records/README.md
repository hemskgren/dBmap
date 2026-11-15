# Firmware Map performance records

Filled records belong here beside
[TEMPLATE-perf-record.md](./TEMPLATE-perf-record.md).

## R0 status

| Artifact | Class | Purpose |
| --- | --- | --- |
| [R0-source-verified-config.md](./R0-source-verified-config.md) | source-verified / configured | Capacities and PCM math from source |
| [R0-historical-unqualified-2026-11-02.md](./R0-historical-unqualified-2026-11-02.md) | historical / unqualified | Explains old L0/heap citations; not current limits |
| [R0-TEMPLATE-ear-002-mono-mic.md](./R0-TEMPLATE-ear-002-mono-mic.md) | template | Operator fill after EAR-002 soak |
| [R0-TEMPLATE-ear-001-dual-mic.md](./R0-TEMPLATE-ear-001-dual-mic.md) | template | Operator fill after EAR-001 soak |
| Procedure | — | [`docs/diagnostics/r0-esp32-s3-measurement-baseline.md`](../../diagnostics/r0-esp32-s3-measurement-baseline.md) |

**No device `measured` R0 soak rows exist until operators complete the procedure.**

## Earlier M3 note (still valid for pre-R0 candidates)

| Candidate source | Why not migrated as current measured |
| --- | --- |
| `docs/diagnostics/ear-l1-perf-optimization.md` | Device tables TBD; host timings not S3 |
| `docs/diagnostics/ear-l2-acoustic-dynamics-contract.md` | Incomplete Map identity |
| Pre-R0 empty migration | Superseded by R0 templates above |

## Instrumentation to reuse (R0)

- `sp_diag_log_resources` — CPU, combined heap, internal, SPIRAM, largest blocks, registered task stack HWM
- `ear_obs: L0/L1/L2 perf` + `R0 stage` (SERIAL_DIAG)
- `ae: timing` analyze / post_dsp (AE_DIAG)
- `sp_mic_pcm_*`, Manager `evq_dropped`, CE / Critical drop counters
