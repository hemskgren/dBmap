# Example: active periods, inter-period pauses, and final collection

Illustrative layout for a four-period automatic job (30 s active, 8 s inter-period, 8 s final collection). Values are examples; real jobs write `summary.json` / `report.html` / `analysis.md` under `shared/results/ear-tests/<job-id>/`.

## Configured timing

| Setting | Value |
| --- | ---: |
| Active period duration | 30 s |
| Inter-period pause | 8 s |
| Final collection pause | 8 s |
| Periods | 4 |

## Timeline (wall clock)

```text
P1 active          |██████████████████████████████| 30s
inter-period pause |........|                       8s
P2 active          |██████████████████████████████| 30s
inter-period pause |........|                       8s
P3 active          |██████████████████████████████| 30s
inter-period pause |........|                       8s
P4 active          |██████████████████████████████| 30s
final collection   |........|                       8s  ← not an active period
```

## Persisted pause records (`periods.json` → `pauses[]`)

| pause_id | kind | planned_s | role |
| --- | --- | ---: | --- |
| `inter-period-000` | `inter_period` | 8 | After P1, before P2 |
| `inter-period-001` | `inter_period` | 8 | After P2, before P3 |
| `inter-period-002` | `inter_period` | 8 | After P3, before P4 |
| `final-collection` | `final_collection` | 8 | After P4 only |

There is **no** inter-period pause after P4.

## Observation phases (`received_time_utc`)

| phase_by_received | Meaning |
| --- | --- |
| `active` | Received during an intentional experiment window (`period_id` set) |
| `pause` | Received during an inter-period pause |
| `final_collection` | Received during the trailing collection window |
| `job_idle` / `unassigned` | Outside the above windows (if any) |

`event_time_utc` may still map to an earlier period even when `received_time_utc` falls in `pause` or `final_collection` — that disagreement is reported, not treated as an error.
