# EAR Test Runner

Host-side reusable tool for controlled tests against one or more physical dBmap Ears.

It **collects and describes** what the Hub received during intentional test periods. It does **not** interpret detector emit kinds (START/PERIODIC/END), acoustic episodes, or environmental conditions.

Constraints: no firmware changes, no Observation protocol changes, no Hub ingest changes, no detector threshold changes.

## Quick start

```bash
# Manual 4×30s periods with 8s inter-period pauses + 8s final collection (defaults)
uv run python scripts/ear_test_runner.py run \
  --node-id EAR-001 \
  --period-seconds 30 \
  --periods 4 \
  --progression manual \
  --test-name "5A-real-world-detector-baseline" \
  --environment "Workroom, fans running" \
  --operator-notes "P1: fans only; P2: keyboard; P3: speech; P4: quiet room"

# Automatic short smoke (explicit short pauses)
uv run python scripts/ear_test_runner.py run \
  --node-id EAR-001 \
  --period-seconds 10 \
  --periods 2 \
  --pause-seconds 2 \
  --final-collection-pause-seconds 2 \
  --progression automatic \
  --test-name "smoke"
```

Connection settings come from project `.env` via `scripts/lib/hub_cli.py` (same as other Hub CLIs). Use `uv run python scripts/ear_test_runner.py` like the other ops scripts.

## Timing defaults

| Setting | CLI flag | Default |
| --- | --- | ---: |
| Active period duration | `--period-seconds` | (required) |
| Inter-period pause | `--pause-seconds` | **8** |
| Final collection pause | `--final-collection-pause-seconds` | **8** |

Sequence (automatic progression):

1. Run active period  
2. Wait for the inter-period pause (not after the last period)  
3. Start the next active period  
4. Repeat until all active periods complete  
5. Wait for the **final collection pause** (trailing Hub poll window)  
6. Finalize collection and generate reports  

The final collection pause is **not** an active test period. It exists so delayed Observation publication/reception after the last active period can still be polled. An Observation received then may still have `event_time_utc` from an earlier interval.

CLI transitions are explicit, for example:

```text
P1/4 (period-000) COMPLETE
INTER-PERIOD PAUSE — 8 seconds
Stop the previous activity.
Prepare for the next period.
  … 7s remaining
P2/4 STARTING (period-001)
…
P4/4 (period-003) COMPLETE
FINAL COLLECTION PAUSE — 8 seconds
Collecting trailing Observations before finalization (not an active test period).
TEST COMPLETE
```

Manual progression still prompts **Enter before each active period**; pauses remain auto-timed (no prompt). Ctrl+C during a period or either pause marks the job interrupted and preserves artifacts.

## Commands

| Command | Purpose |
| --- | --- |
| `run` | Start a job (fails if `ACTIVE` lock exists) |
| `list` | List jobs under `shared/results/ear-tests/` |
| `status [--job-id]` | Show manifest / ACTIVE |
| `report <job-id>` | Regenerate `summary.json` + `report.html` |
| `metadata <job-id>` | Edit optional metadata after the run |
| `recover` | Safe recovery for leftover `ACTIVE` lock |

### Recovery

If the process crashes, `shared/results/ear-tests/ACTIVE` may remain.

```bash
uv run python scripts/ear_test_runner.py recover
uv run python scripts/ear_test_runner.py recover --finalize-interrupted
```

`--finalize-interrupted` marks a still-`running` job interrupted, reconciles, writes the report, and clears `ACTIVE`. Artifacts are never deleted. An interrupted final-collection or inter-period pause is recorded with status `interrupted` and actual end time.

## Artifacts

```
shared/results/ear-tests/<job-id>/
  manifest.json
  periods.json      # periods[] + pauses[] (inter_period / final_collection)
  observations.jsonl
  summary.json      # machine-readable stats
  report.html       # human visual report
  analysis.md       # AI-friendly narrative (same data as summary/html)
```

`summary.json`, `report.html`, and `analysis.md` are always regenerated together from the same persisted job data (one shared summary snapshot). They are not independent sources of truth.

`shared/results/ear-tests/**` is gitignored (`.gitkeep` retained).

Example timing layout (illustrative): [`docs/examples/ear-test-timing-report.md`](examples/ear-test-timing-report.md).

### Sharing with an AI assistant

Prefer attaching `analysis.md` + `summary.json`. Add `observations.jsonl` when deeper payload-level review is needed. Keep `report.html` for interactive human browsing.

## Collection

1. **HTTP poll** of `GET /api/v1/observations?node_id=&limit=500` during periods, inter-period pauses, and the final collection pause.
2. **Read-only SQLite reconcile** via `docker compose exec hub-bootstrap` at period end and job end.

Deduplication uses `observation_id` and **never overwrites** the first stored payload.

`manifest.collection` records:

- `sqlite_reconcile_attempted` / `succeeded` / `error`
- `completeness`: `complete` or `potentially_incomplete`
- human-readable `completeness_notes`

If SQLite is unavailable the job still runs; results are marked potentially incomplete.

## Period association (policy v2)

| Concept | Meaning |
| --- | --- |
| Test period | Intentional experiment window (`period_id`) |
| Inter-period pause | Wall time between consecutive periods (phase `pause`) |
| Final collection | Trailing poll window after the last active period (phase `final_collection`) |
| Observation | Ear payload + Hub `received_time_utc` |

Primary association clock: **`received_time_utc`**. Secondary: `event_time_utc`. Both are stored. Disagreements are highlighted in the report (latency / interval-start semantics) and are not treated as errors.

Reports distinguish Observations received during active periods, inter-period pauses, final collection, and outside windows (`job_idle` / `unassigned`) when applicable.

The runner does **not** assume Observations align with period boundaries or detector episodes.

## Metadata

Optional, operator-supplied only (never inferred):

- `test_name`, `description`, `environment`, `operator_notes`
- `firmware_version_by_ear` via `--firmware-version EAR-001=0.5.20` (repeatable)

Editable later with `metadata`.

## Shutdown

Ctrl+C / early quit:

- Job → `interrupted` if not completed
- Active period → `interrupted`
- Active pause (inter-period or final collection) → `interrupted`
- Remaining planned periods / pauses → `skipped`
- All collected observations preserved
- Final SQLite reconcile attempted
- `summary.json` + `report.html` + `analysis.md` always generated
