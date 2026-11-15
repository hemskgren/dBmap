# Firmware Map performance records

Filled records belong here beside
[TEMPLATE-perf-record.md](./TEMPLATE-perf-record.md).

## M3 migration status

**No records migrated in M0–M3.**

Existing diagnostics were inspected and **do not** meet the bar for a
traceable device performance record (product × hardware × IDF × firmware ×
git × date/duration).

| Candidate source | Why not migrated |
| --- | --- |
| `docs/diagnostics/ear-l1-perf-optimization.md` | States physical EAR baseline / A/B **not measured** in that environment; device L0/L1 tables are **TBD**. Host x86 timings (~839 µs → ~258 µs) are explicitly **not** S3 evidence. |
| `docs/diagnostics/ear-l2-acoustic-dynamics-contract.md` (§ baseline) | User-reported EAR-001 @ 160 MHz L0/L1 last/max figures (~62/67 ms L0, ~40/45 ms L1) lack `git_sha`, `firmware_version`, `platform_version`, `date_utc`, soak `duration_s` / `analyze_n`, and IDF pin on the table itself. L2 cost remains **estimate / unmeasured**. Insufficient for a Map record without inventing identity fields. |
| `docs/diagnostics/ear-l2-acoustic-features-investigation.md` | Design/investigation notes; incremental L2 cost not locked as device measurement for Map purposes. |

Keep using those diagnostics docs as engineering notes. When a soak has full
identity fields, copy the template and classify metrics as `measured` (or later
`verified_operating`).

## Instrumentation to reuse (no new firmware in M0–M3)

- `sp_diag_log_resources` — CPU/heap/SPIRAM/stack HWM
- `ear_obs: L0/L1/L2 perf` serial lines when `CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG`
- `dbmap_ear_mic_get_l0_perf` / `_l1_perf` / `_l2_perf`
- `sp_mic` window / capture_diag counters
