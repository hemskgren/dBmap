# Agent working instructions (dBmap)

This file is the **canonical** repository-wide instruction set for AI coding
agents. Do not add parallel copies under `.cursor/rules` or other instruction
files unless the maintainers explicitly ask for them.

Human operators still follow `README.md` and `docs/`. Agents must follow both
this file and those docs.

Sensor families (Ear, Output, future products) and Hub / Analysis / web /
firmware all share these rules.

## Where to learn the system

Start here, then go deeper only as needed:

1. `README.md` — purpose, layout, tests
2. `docs/local-deployment.md` — bring-up, `.env`, Hub host tools, destructive ops
3. `docs/connectivity.md` — HTTPS/MQTT topology; SQLite has **no** published port
4. `docs/protocol.md` — message and Hub list/pagination contracts
5. `docs/hub-api.openapi.yaml` — HTTP API shapes
6. `docs/authorization.md`, `docs/identity-and-operating-modes.md`, `SECURITY.md`
7. Domain docs as relevant (`docs/ear-test-runner.md`, `docs/hub-map-analysis.md`,
   `docs/simulator.md`, `docs/health-and-availability.md`, ADRs under `docs/adr/`,
   `docs/sensor-platform-architecture.md`)

Do not invent endpoints, auth, or DB paths. Discover them from the docs and code.
Do not weaken existing security, authorization, or secret-handling requirements.

## Secrets and configuration

- Load local Hub settings from the project `.env` via established helpers
  (`scripts/lib/hub_cli.py` `load_project_env`, Compose env, documented CLI flags).
- Prefer `.env.example` as the key catalog. **Never** print, commit, paste, or
  log values from `.env` (tokens, MQTT passwords, CA keys).
- Default Hub client trust: `shared/trust/ca.crt`.
- Human datetime display may use `DBMAP_DISPLAY_TIMEZONE` (presentation only).
  APIs, MQTT, DB storage, and `--json` stay UTC ISO unless a tool documents otherwise.

## Operational safety (live systems)

Do **not** independently perform actions that affect live devices, physical
hardware, or running services unless the user explicitly requested or approved
them. Examples:

- OTA / firmware flash, erase, or promotion to Desired
- Device reboot, recovery, bootstrap-token replace, or credential rotation
- Desired/config/installation writes and Output commands (relay, audio)
- Home Assistant media playback or volume changes
- Destructive history or database operations (clear / prune / wipe)
- Stopping, recreating, or reconfiguring Compose services beyond what the user asked

Read-only inspection via documented APIs/CLIs is fine when the task needs it.
When unsure whether an action is live-affecting, ask first.

## Data-access decision procedure (mandatory)

When a task needs project data (observations, nodes, health, users, firmware,
analysis scratch, etc.), choose the **lowest** numbered option that works:

### 1. Documented APIs and supported application interfaces

- Hub HTTPS API via Nginx (`/api/v1/...`), as in OpenAPI + `docs/protocol.md`.
- Analysis API when the `analysis` Compose profile is enabled (`/api/v1/analysis/...`).
- Hub Web UI only when the task is explicitly UI work; for data investigation prefer API/CLI.
- MQTT is for devices/simulators and Hub ingest — not a substitute for admin data queries
  unless the task is about messaging itself.

Use existing auth: bearer admin/viewer tokens from `.env`, device tokens only for
device-scoped routes. Preserve OPA and Hub authorization behavior; do not bypass them.

### 2. Existing project scripts and tools (API-backed)

Prefer `scripts/*.py` and `scripts/lib/` that already speak the API with project config, e.g.:

- `hub_summary.py`, `hub_admin.py` — Hub/node status
- `hub_event_track.py` — observation list/watch/**summary** (retained-history lifecycle stats)
- `hub_map_analysis.py`, `map_demo_helicopter.py` — analysis/map scratch flows
- `ear_audio_compare.py`, `ear_test_runner.py` (HTTP collection paths), `homeassistant.py`
- `hub_firmware.py` — firmware catalog/ops via API

Run them the way docs show (`uv run python scripts/...`), with `.env` + CA.

Inspect the selected execution path before running a tool. A script that supports
API-backed and DB-backed modes must use the API-backed mode unless DB access has
been explicitly approved.

### 3. Extend existing scripts/tools when they almost fit

If an API exists but the CLI/UI helper is missing a flag, summary, filter, or export:

1. Extend the **existing** script or shared helper (especially `scripts/lib/`).
2. Add/adjust tests under `tests/scripts/` (or the relevant package tests).
3. Use the extended tool to finish the original task.
4. Do **not** leave behind a one-off SQL snippet or disposable query when a
   reusable API-backed tool should own the capability.

If the API lacks a capability, investigate whether the API itself can reasonably
be extended before considering direct DB access. Do not assume that an API
limitation authorizes a DB workaround.

### 4. Direct database access — last resort only (approval required)

SQLite lives inside containers (`bootstrap.db` / `analysis.db`). There is no DB port.
Direct DB access means raw SQL, `sqlite3`, ad-hoc `docker compose exec ...`, **or**
running any in-repo wrapper that opens the database file, including:

- `scripts/hub_observations_db.py` (clear / prune / wipe-device-data)
- `scripts/lib/ear_test/collect.py` SQLite reconcile helpers
- Any new or existing script that reads or writes Hub/Analysis SQLite outside the HTTP API

**Existing database wrappers are not exempt.** Explicit user approval is still
required before using them, including for read-only access.

**Before any direct DB access (read or write), stop and ask the user for explicit approval.**
Approval applies only to the specific operation, target, and scope described in
the request. Any material change requires renewed approval.
In that ask, include:

1. Why API + existing **API-backed** tools (and reasonable extensions) are insufficient
2. Exactly what data is needed and why
3. The precise read/write operation, which tool or SQL, target DB/file, tables/rows scope, and blast radius
4. Whether the proposed path is an existing wrapper or new ad-hoc SQL (prefer a documented wrapper **after** approval, not silent new SQL)

**Direct DB writes always require explicit approval.** Never silently bypass application
APIs, OPA policy, or Hub business logic for mutations.

## Read vs write

| Kind | Default path | Extra rule |
| --- | --- | --- |
| Read via API/CLI | Allowed when credentials and docs support it | Prefer read-only CLIs; say which tool/endpoint you used |
| Write via API | Only when the task requires it **and** it is not a live-affecting action unless requested/approved | Use documented admin routes; no speculative mutations |
| Read via DB (including wrappers) | **Approval required** | Ask first even for established wrappers |
| Write via DB (including wrappers) | **Approval required** | Treat wipe/clear/prune as destructive; follow `docs/local-deployment.md` |

## Pagination, snapshots, completeness, identity, time, quality

When fetching Hub list resources (`observations`, `users`, `groups`, and any future
paginated collections that share the contract):

- Honor `page` / `page_size` (max 500) and `order`.
- Freeze multi-page reads with `pagination.snapshot_id` from page 1.
- Verify completeness (`fetched` vs `total_count`); **do not** present partial history
  as complete retained statistics.

Keep these identity concepts distinct — they are **not** interchangeable:

- **Report / delivery identity** — used to de-duplicate individual MQTT report
  records when fetching (CLI: typically `node_id` + `sequence_number` when both
  are present). This collapses duplicate deliveries of the same report row.
- **Lifecycle / observation identity** — used for aggregation and unique counts:
  group by `(node_id, observation_id)`. PERIODIC reports do not create a new
  unique observation; follow `hub_event_track.py summary` / API field semantics.

Do not treat `sequence_number` as a substitute for `observation_id`, or the reverse.
Follow the actual API contract (`docs/protocol.md`, OpenAPI) and existing CLI behavior.

Also:

- Distinguish **raw report/row counts** from **unique lifecycle counts**.
- For observation lifecycle stats, match `hub_event_track.py summary` semantics
  (unique by retained `(node_id, observation_id)` even if START was pruned;
  completed = retained END/COMPLETE; durations from END/COMPLETE
  `episode_duration_ms` only) unless the user asks for a different definition.
- Keep machine output in UTC ISO; format local display only at the edges.
- Surface data-quality notes (missing START, duplicate START/END, incomplete snapshot).

## Reporting which data source you used

When answering with data or changing data-facing code, briefly state:

- **Source:** API path / script name / (if approved) DB tool
- **Scope:** filters, time window, snapshot id, completeness
- **Semantics:** e.g. raw reports vs unique `(node_id, observation_id)` lifecycles

## Out of scope for shortcuts

- Do not use production-like DB files as a scratch pad for features the API should own.
- Do not add new standing DB entrypoints without documenting them next to Hub host tools
  and keeping the approval norms in this file.
- Do not create additional always-on AI instruction files that duplicate this document.
