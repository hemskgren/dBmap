# Operations Service

Operations is dBmap's optional infrastructure administration service and the
sole active owner of Core Hub, public endpoint, service health, and container
runtime monitoring. It does not manage Devices or Observation reports.

Operations is a Compose profile and can be absent. Hub-bootstrap, MQTT,
dbmap-web, Nginx, EAR communication, and Observation processing do not depend
on it. Device Monitor remains active in Hub-bootstrap when Operations is
disabled.

## Responsibilities

Operations directly monitors:

- Hub API reachability
- Nginx-local HTTPS availability
- Hub API reachability through Nginx (the existing end-to-end `/health` path)
- dbmap-web reachability
- Mosquitto MQTT reachability
- OPA dependency health
- Optional public API reachability
- Optional public MQTT reachability
- Operations itself, including its database and worker threads

Operations records monitoring samples, state/event transitions, sparse
coverage, checkpoints, and 24-hour, 7-day, and 30-day availability windows.
The service does not extract or alter the existing Hub Device Monitor.
Hub database status is obtained from the Hub-owned diagnostic API. Operations
does not mount or open the Hub database.

## Architecture

The implementation is in [operations/app/](../operations/app/).

- `core_monitor.py` runs the independent Core Hub polling worker.
- `public_monitor.py` runs the Public API and Public MQTT workers.
- `probes.py` implements direct HTTPS, MQTT, and runtime probes.
- `container_runtime.py` owns bounded Docker runtime inspection and log tails.
- `monitoring_cycle.py`, `monitoring_availability.py`, and
  `monitoring_math.py` implement Operations-owned monitoring calculations.
- `models.py` owns the Operations database schema.
- `api.py` implements authenticated API routes.
- `policy.py` provides the existing OPA authorization pattern.
- `db.py` separates database setup and sessions from domain logic.

Operations mounts the Docker socket and directly owns allowlisted container
runtime inspection, restart detection, and bounded log tails. It filters by
Compose service and project, returns selected runtime metadata rather than full
inspect results, and caps Docker responses at 1,000,000 bytes. Runtime
checkpoints and inferred restart events are persisted in the Operations
database. Operations still uses authenticated Hub-bootstrap APIs for
Hub-owned database and service diagnostics; direct HTTPS probes remain
independent of those APIs. Hub-bootstrap no longer mounts the socket or runs
the legacy Hub/Public infrastructure monitoring workers. Operations has no
access to the Hub database.

The Docker socket grants broad control over the host Docker engine. A
read-only bind mount does not make Docker API operations read-only. This direct
socket model is accepted for current development and standalone deployment,
but remains a significant security limitation; no socket proxy or additional
runtime infrastructure is introduced. Operations is the only Hub service with
Docker socket access.

The implementation does not use an external database or native extensions.

## Deployment

Operations is enabled with the `operations` Compose profile:

```bash
docker compose --profile operations up -d --build hub-bootstrap nginx opa operations dbmap-web mosquitto
```

The service is bound only to the internal Compose network. It is not host-published.
Nginx routes `/api/v1/operations/*` to `operations:8443` with verified TLS.
When Operations is absent, Nginx returns an explicit 503 JSON response.
OPA is included because Operations uses the existing authorization service.
If OPA was already running with an older policy that does not contain the
`operations.*.read` actions, restart it after updating the policy:

```bash
docker compose restart opa
```

Viewer access is allowed for `operations.status.read`,
`operations.events.read`, `operations.details.read`, and `operations.logs.read`.
The deployed policy must include the same rules as
[`policy/authz.rego`](../policy/authz.rego).

Operations uses a dedicated `operations-data` volume. Hub-bootstrap does not
run infrastructure monitoring and does not access the Operations database.
New installations do not create pre-cutover Hub/Public monitoring tables.
Existing installations remove the allowlisted tables during Hub schema
initialization, after preserving Device Monitor checkpoint continuity.

### Configuration

| Environment variable | Default | Purpose |
| --- | --- | --- |
| `DBMAP_ADMIN_TOKEN` | Required | Service-to-service authentication for Hub-owned diagnostics |
| `DBMAP_HUB_ID` | `local-01` | Logical Hub identity |
| `DBMAP_HUB_API_HOST` | `hub-bootstrap` | Hub-owned diagnostics host |
| `DBMAP_HUB_API_PORT` | `8443` | Hub-bootstrap HTTPS port |
| `DBMAP_DOCKER_SOCKET` | `/var/run/docker.sock` | Docker Engine Unix socket path |
| `DBMAP_COMPOSE_PROJECT` | Auto-detected | Compose project filter for runtime access |
| `DBMAP_PUBLIC_BASE_PATH` | Empty | Public URL prefix used for Nginx health probes |
| `DBMAP_CORE_MONITOR_ENABLED` | `true` | Enable the Core worker |
| `DBMAP_CORE_MONITOR_INTERVAL_S` | `60` | Core polling interval |
| `DBMAP_GAP_THRESHOLD_S` | `120` | Monitoring gap threshold |
| `DBMAP_RETENTION_DAYS` | `30` | Monitoring history retention |
| `DBMAP_PUBLIC_API_MONITOR_ENABLED` | `false` | Enable Operations Public API polling |
| `DBMAP_PUBLIC_MQTT_MONITOR_ENABLED` | `false` | Enable Operations Public MQTT polling |
| `DBMAP_PUBLIC_MONITOR_INTERVAL_S` | `60` | Public polling interval |
| `DBMAP_PROBE_TIMEOUT_S` | `2` | Individual probe timeout |

Retention accepts 30–90 days. Public endpoints remain disabled unless explicitly
enabled.

## API

The unauthenticated technical health routes are:

- `GET /health`
- `GET /api/v1/operations/health`

The internal `GET /health` route is used by Docker and does not expose
sensitive diagnostics. `/api/v1/operations/health` is the same lightweight
technical health contract through Nginx.

Authenticated routes are:

- `GET /api/v1/operations/status`
- `GET /api/v1/operations/services`
- `GET /api/v1/operations/services/{service}/details`
- `GET /api/v1/operations/services/{service}/logs`
- `GET /api/v1/operations/services/mosquitto/connection-failures`
- `GET /api/v1/operations/events`

Runtime inspection and log retrieval run directly in Operations and allow only
`operations`, `hub-api`, `nginx`, `web`, `mqtt`, and `opa`. Runtime responses
contain a sanitized container identity, running/health state, start time, and
restart count. Logs are an on-demand tail of at most 30 lines. Hub-owned
Details and database diagnostic interfaces remain owned by Hub-bootstrap.

`/status` separates Operations process health from Hub monitoring health and
availability. `/services` returns the logical Monitored Services inventory.
Public API and Public MQTT checks are endpoint-reachability probes, not separate
services, and are shown in the public endpoint availability section of `/status`
instead.

The service inventory contains Operations, Hub API, Nginx, Web, MQTT broker,
and OPA. The Operations database is grouped under Operations; the Hub database
is grouped under Hub API. The Nginx service state comes from the direct Nginx
health check. Hub API reachability and Hub API-through-Nginx reachability are
separate signals.

Example status shape:

```json
{
  "hub": {"hub_id": "local-01"},
  "service": {
    "service_id": "operations",
    "application": "dBmap Operations",
    "version": "0.1.0",
    "uptime_seconds": 123,
    "health": "healthy",
    "worker_errors": []
  },
  "database": {
    "status": "ok",
    "engine": "sqlite",
    "path_owned": "operations.db"
  },
  "monitoring": {
    "core": {
      "enabled": true,
      "running": true,
      "stale": false,
      "last_successful_check": "2026-11-26T21:53:37+00:00"
    },
    "public": {
      "public_api": {"enabled": false, "running": false, "state": "unknown"},
      "public_mqtt": {"enabled": false, "running": false, "state": "unknown"}
    }
  }
}
```

Example health result:

```json
{"status": "ok", "service": "operations"}
```

Example service details result:

```json
{
  "service": "Nginx",
  "instance": "nginx",
  "status": "ok",
  "reason": null,
  "source": "Operations direct probe",
  "hub_api_via_nginx": {
    "status": "offline",
    "reason": "HTTP 503",
    "source": "Operations end-to-end probe through Nginx"
  },
  "runtime_source": "Operations Docker runtime"
}
```

`GET /api/v1/operations/services/{service}/details` provides direct service
reachability and diagnostic data. Hub-owned diagnostic details are included
with an explicit `hub-bootstrap` source. Runtime details are returned only for
allowlisted services: `operations`, `hub-api`, `nginx`, `web`, `mqtt`, and
`opa`. Hub database diagnostics appear within the Hub API details; Operations
database diagnostics appear within Operations details.

`GET /api/v1/operations/services/{service}/logs` retrieves an on-demand tail
for those same allowlisted services. It returns at most 30 lines, is not
historical log pagination, and identifies `Operations Docker runtime` as its
source. For example:

```json
{"service":"Operations","lines":["Operations service started"],"line_limit":30,"source":"Operations Docker runtime"}
```

The Docker reader caps response bodies at 1,000,000 bytes. It selects only the
newest container belonging to the configured Compose project and allowlisted
Compose service. MQTT failed-connection diagnostics are also Operations-owned
and require `operations.logs.read`. The endpoint reads the read-only mounted
Mosquitto log and preserves the existing three-hour time window, two-megabyte
scan bound, 200-line result limit, filtering, and response fields:

```json
{
  "service": "MQTT broker",
  "compose_service": "mosquitto",
  "window_hours": 3,
  "source": "file",
  "path": "/mosquitto/log/mosquitto.log",
  "lines": [],
  "count": 0,
  "notices": ["Host fail2ban ..."]
}
```

`path` is omitted when the log is unavailable. The Hub no longer mounts or reads
the broker log.

| Monitored service | Direct Details signal | Supplementary diagnostic source | Logs |
| --- | --- | --- | --- |
| Operations | Own worker and SQLite status | Operations Docker runtime | Allowlisted 30-line tail |
| Hub API | Direct HTTPS health | Hub-bootstrap API and database diagnostics; Operations Docker runtime | Allowlisted 30-line tail |
| Nginx | Nginx-local HTTPS health and separate Hub API-through-Nginx result | Hub-bootstrap Nginx details; Operations Docker runtime | Allowlisted 30-line tail |
| Web | Direct HTTPS health | Hub-bootstrap Web details; Operations Docker runtime | Allowlisted 30-line tail |
| MQTT broker | Direct MQTT TLS connection | Hub-bootstrap broker metrics; Operations Docker runtime | Allowlisted 30-line tail |
| OPA | Direct OPA health | Hub-bootstrap policy health summary; Operations Docker runtime | Allowlisted 30-line tail |
| Hub database (under Hub API) | Hub-owned SQLite diagnostic | Hub-bootstrap | Not supported |
| Operations database (under Operations) | Operations-owned SQLite diagnostic | Operations | Not supported |

When a worker or SQLite database is unhealthy, health is `degraded` or
`503`; the response includes bounded worker/database information. Failure of
a monitored Hub service does not itself make Operations unhealthy.

## Monitoring semantics

Operations distinguishes:

- Confirmed available
- Confirmed unavailable
- Unobserved time
- Unknown or missing diagnostics

Unobserved time is excluded from availability denominators. Operations stopped
or the whole host stopped is not automatically classified as confirmed Hub
downtime.

Operations calculates Core reachability, MQTT, and OPA availability:

- Reachability failure is unhealthy/unavailable.
- MQTT failure with reachability intact is degraded/unavailable for Core Hub
  availability.
- OPA-only failure is degraded but available for Core Hub availability.
- Unknown or missing evidence remains unobserved.

Public API and Public MQTT samples have independent workers and checkpoints.
Core and public MQTT clients use separate unique clean-session client identities.

The existing Core Hub aggregate retains its previous reachability semantics:
`hub_api_via_nginx` is the end-to-end Hub `/health` request through Nginx and
continues to affect overall Hub availability. `nginx_local` records Nginx's
independent local health and drives the Nginx service state and its
component-availability calculation. Nginx serves
`GET /api/v1/nginx/health` directly with a JSON `200`; the existing `/health`
endpoint still proxies to Hub-bootstrap. The Docker healthcheck uses the local
endpoint so a Hub API restart does not mark the Nginx container unhealthy.
Existing `nginx_local` continuity is preserved because that component key is
unchanged. Historical Operations rows whose `nginx` key represented the
upstream Hub API path are read as `hub_api_via_nginx`; stored samples/events are
not rewritten, and the rename does not emit a false state-change event.
`nginx_local` history begins when that independent probe was introduced; older
`nginx` history cannot establish local Nginx health, so its availability window
has an expected history discontinuity before that point. Nginx-local state is
not derived from Hub API availability.

Operations inspects the allowlisted Compose containers during each Core
monitoring cycle. It persists each service's last-seen start time and restart
count in `operations_runtime_checkpoints`; a changed runtime signature creates
an inferred `service_restart_detected` event in the Operations database.
Initial signatures establish a baseline and do not create events. A detected
restart does not prove downtime. This path does not read or import historical
Hub Monitor samples or events.

## Database and persistence

Operations owns its independent:

- `operations_monitor_samples`
- `operations_monitor_events`
- `operations_monitor_checkpoints`
- `operations_runtime_checkpoints`
- `operations_public_monitor_samples`
- `operations_public_monitor_events`
- `operations_public_monitor_checkpoints`

The database is initialized empty. There is no historical import or migration.
Sparse samples and checkpoints are committed by each worker and survive
Operations restarts. Read-time stale boundaries prevent stale workers from
appearing as confirmed coverage.

Core cleanup runs every ten cycles. Public cleanup runs every ten minutes per
worker. Rows outside the retention window are pruned, with the existing sparse
carry-in boundary retained where applicable.

Pre-cutover Hub infrastructure history is separate from the tables above.
Older `bootstrap.db` files can contain:

| Historical table | Purpose | Historical indexes |
| --- | --- | --- |
| `hub_monitor_samples` | Hub infrastructure samples | `ix_hub_monitor_samples_hub_id`, `ix_hub_monitor_samples_observed_at`, `ix_hub_monitor_samples_hub_observed` |
| `hub_monitor_events` | Hub infrastructure events | `ix_hub_monitor_events_hub_id`, `ix_hub_monitor_events_occurred_at`, `ix_hub_monitor_events_hub_occurred` |
| `hub_monitor_checkpoints` | Last Hub infrastructure check | Primary key on `hub_id` |
| `public_monitor_samples` | Public API/MQTT samples | `ix_public_monitor_samples_hub_id`, `ix_public_monitor_samples_observed_at`, `ix_public_monitor_samples_scope_observed` |
| `public_monitor_events` | Public API/MQTT events | `ix_public_monitor_events_hub_id`, `ix_public_monitor_events_occurred_at`, `ix_public_monitor_events_scope_occurred` |
| `public_monitor_checkpoints` | Per-component public check checkpoints | Composite primary key on `(hub_id, component)` |

These tables are absent from Hub ORM metadata, so new installations do not
create them. Existing installations are cleaned by an idempotent allowlisted
migration during Hub database initialization. It also removes
`public_monitor_transitions` if present. SQLite drops indexes owned by those
tables with them; no index or table outside the seven-name allowlist is touched.

Before removing `hub_monitor_checkpoints`, the migration checks for a valid
Device Monitor checkpoint for the configured Hub ID. If it is missing or
invalid but that Hub's legacy value is valid, it copies the value first. A
valid Device checkpoint is never overwritten; legacy values for other Hub IDs
are not needed by this installation. If continuity cannot yet be verified,
the migration removes the other legacy tables but retains
`hub_monitor_checkpoints`; after Device Monitor commits its first valid
checkpoint, it retries and removes the remaining table. Thus a disabled Device
Monitor may leave that one legacy checkpoint table until it is enabled.

The legacy checkpoint read in the Device Monitor cycle has been removed. The
handoff is performed before the worker starts; when startup cannot establish a
valid checkpoint, the worker uses its normal first-cycle baseline and the
checkpoint table remains until that baseline is committed.

Before deploying this migration to an existing installation, schedule Hub
maintenance and make a protected, verified SQLite backup. The migration runs
automatically on Hub startup; do not start or recreate Hub against a production
database until the reviewed deployment window. Verify the migration logs and
Hub/Device Monitor APIs afterward. Preserve all `device_monitor_*`,
`device_diag_*`, Observation, node, and unrelated tables. No live database was
used to implement or test this migration.

## Pagination

Operations collection endpoints use the same names and bounds as the existing
Hub list APIs:

- `page`, default `1`
- `page_size`, default `200`
- maximum `500`
- `order=newest|oldest`
- `snapshot_id` for changing historical collections
- UTC `start` and `end` filters on monitoring events

Example events page:

```json
{
  "events": [],
  "pagination": {
    "total_count": 0,
    "page": 1,
    "page_size": 200,
    "total_pages": 0,
    "order": "newest",
    "snapshot_id": null
  },
  "time_range": {"start": null, "end": null}
}
```

Invalid `page`, `page_size`, `order`, or naive/non-ordered time ranges return
HTTP 422. `snapshot_id` freezes event rows at an SQLite rowid boundary and
clamps a requested boundary past the live maximum.

## Authentication and authorization

Operations uses the existing local admin/viewer bearer tokens and OPA.
Service health may be inspected without credentials. Status, service details,
and events require:

- `operations.status.read`
- `operations.details.read`
- `operations.events.read`

Log retrieval and MQTT failed-connection summaries require
`operations.logs.read`. Runtime diagnostics require
`operations.details.read`. When Operations requests Hub-owned diagnostic
details, it uses its configured admin bearer token to call the existing Hub
APIs, which continue to enforce `hub.status.read`. Runtime metadata, restart
detection, container logs, and broker-log summaries are read directly by
Operations under the Operations authorization boundary.

For authorization troubleshooting, distinguish API responses: HTTP 401 means
the bearer token was not accepted by Operations; HTTP 403 means authentication
succeeded but OPA denied the action. The checked-in policy allows viewer reads
for the Operations actions above and allows any action for an authenticated
admin. If both roles receive 403, verify OPA loaded the current
[`policy/authz.rego`](../policy/authz.rego), then run `docker compose restart opa`.
If Operations rejects valid local tokens with 401, check that its container has
the current `.env` token configuration; recreate Operations after changing
those values:

```bash
docker compose --profile operations up -d --force-recreate operations
```

Compose reads `.env` for container configuration but does not export its values
to the host shell. A host-side `curl` using `$DBMAP_ADMIN_TOKEN` sends an empty
token unless that variable was separately exported into the shell.

## Logging

Operations writes bounded application logs to container stdout/stderr using the
existing Docker `json-file` logging convention:

- maximum size: 10 MB
- maximum files: 3
- application database retention is separate from log retention

No file-based Operations application log or centralized Log service is used.
Passwords, bearer tokens, private keys, and sensitive configuration values are
not logged. Probe failures use short bounded reasons.

Compose retains the project's Docker `json-file` rotation settings for the
container logs used by Operations. Hub-bootstrap does not mount Docker socket.
The Hub component container-logs endpoint is retired; Operations owns the
allowlisted container log tails. Firmware Builder build status and complete
build logs continue through its dedicated API and are not part of Operations
monitoring.

## Operations CLI

[`scripts/hub_operations.py`](../scripts/hub_operations.py) is the read-only
Operations monitoring and diagnostics CLI. It follows the shared Hub CLI
configuration, TLS, CA, and bearer-token conventions and calls only
`/api/v1/operations/*`. It does not fall back to Hub endpoints. Its text output
is a concise summary; details and JSON output retain the diagnostic fields
returned by Operations.

Run from the repository root:

```bash
uv run python scripts/hub_operations.py status
uv run python scripts/hub_operations.py status --watch --interval-seconds 30
uv run python scripts/hub_operations.py status --output json
uv run python scripts/hub_operations.py services
uv run python scripts/hub_operations.py details mqtt
uv run python scripts/hub_operations.py logs nginx
uv run python scripts/hub_operations.py events --page-size 50
uv run python scripts/hub_operations.py events --page 2 --page-size 50 --snapshot-id 123
uv run python scripts/hub_operations.py failed-connections
```

Connection settings come from the project `.env` and the established CLI
options (`--hub-host`, `--hub-port`, `--base-path`, and `--ca`). `--output
json` is available on every command. Status watch emits successive JSON
objects as newline-delimited JSON.

Events and services use the Hub pagination names and bounds: `page` (1),
`page_size` (200, max 500), `order` (`newest` or `oldest`), and
`snapshot_id`. Events also accept timezone-aware ISO-8601 `--start` and `--end`.
Each collection result includes `result_complete`; text output states whether
the response contains the full collection. For multi-page event history, use
the first page's snapshot ID with later page requests to keep the view frozen.
The CLI does not fetch hidden extra pages.

`failed-connections` uses the existing API defaults without overriding them:
the server's three-hour time window and maximum of 200 log lines. `logs`
displays the API's bounded 30-line tail.

Errors are distinguished: HTTP 503 reports that optional Operations is
disabled or unavailable; 401/403 reports authentication or authorization
failure; transport and TLS errors report a connection failure. Other HTTP
errors are surfaced without trying Hub endpoints.

Representative text output (values vary by Hub and time):

```text
$ uv run python scripts/hub_operations.py status
Operations: healthy (own health: healthy)
Database: ok
Core Hub health: degraded (score: 70)
  Reason: Policy engine is offline
Core Hub availability: degraded
  Core Hub coverage
    24h: 98.50%, state=degraded, eligible=30000s, available=29550s, unavailable=450s, unobserved=400s (low coverage)
  Components
    MQTT broker: ok; last success=2026-11-28 00:55:00 CET
Monitoring workers
  Core: enabled=True, running=True, stale=False, last success=2026-11-28 00:55:00 CET
```

```text
$ uv run python scripts/hub_operations.py services
Services: 6 returned of 6 (page 1/1, complete=true)
  operations: healthy — Operations
  hub-api: ok — Hub API
    Hub database: ok
  nginx: ok — Nginx
```

```text
$ uv run python scripts/hub_operations.py details mqtt
MQTT broker details
{
  "instance": "mqtt",
  "reason": null,
  "service": "MQTT broker",
  "status": "ok",
  "source": "Operations direct probe"
}
```

```text
$ uv run python scripts/hub_operations.py logs nginx
Nginx logs (limit: 30 lines)
nginx started
```

```text
$ uv run python scripts/hub_operations.py events --page-size 1
Events: 1 returned of 42 (page 1/42, order=newest, complete=false, snapshot=123)
  Partial history; pass the snapshot ID with --snapshot-id when requesting another page.
  2026-11-28 00:54:00 CET mqtt/state_changed ok → offline — broker unavailable [id=event-123, source=operations_monitor, certainty=confirmed]; observed=2026-11-28 00:54:00 CET
```

```text
$ uv run python scripts/hub_operations.py failed-connections
MQTT broker failed connections: 1 in the last 3 hours (source: file)
  28/Nov/2026 00:51:00: Client connection from 192.0.2.20 failed: not authorised
  Note: Host fail2ban (if enabled for MQTT) drops banned clients before Mosquitto, so those attempts never appear in this list.
```

[`scripts/hub_summary.py`](../scripts/hub_summary.py) remains the Hub overview
CLI for Hub-owned component status and device/Observation aggregates. It does
not report Operations availability or event history.

## Known limitations

- Local manual verification observed temporary Hub API unavailability during
  multi-service rebuilds. The cause and recovery behavior remain an
  investigation item; no startup behavior or monitoring algorithms were
  changed in response.
- Hub database details and supplementary service diagnostics depend on
  authenticated Hub-bootstrap APIs. If Hub-bootstrap is unavailable, those
  details are unavailable; direct Operations probes and Docker runtime reads
  continue.
- Operations has direct Docker socket access, which is not suitable as a final
  least-privilege production architecture.
- Public API/Public MQTT monitoring is optional and disabled by default.
- Public and Core workers have independent schedules and need comparison rather
  than exact timestamp equality.
- An existing installation can temporarily retain only
  `hub_monitor_checkpoints` if the Device Monitor checkpoint is not yet valid;
  startup migration retries after the first successful Device Monitor cycle.
- Infrastructure status, details, logs, and MQTT failed-connection diagnostics
  are available only when optional Operations is enabled. Hub-owned status and
  component diagnostics remain available without Operations.

## Local deployment and verification

These are local manual checks; they are not run automatically by the test
suite. Review with the operator before restarting any running local service.
Compose validation itself is read-only:

```bash
docker compose --profile operations config --quiet
COMPOSE_PROFILES= docker compose config --quiet
```

With Operations enabled, source the local `.env` into the shell, then start
the Hub dependencies and Operations profile:

```bash
set -a
. ./.env
set +a
docker compose --profile operations up -d --build hub-bootstrap nginx opa operations dbmap-web mosquitto
docker compose --profile operations ps
curl -fsS --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/operations/status
curl -fsS --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/operations/services/hub-api/details
curl -fsS --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/operations/services/hub-api/logs
curl -fsS --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" "https://hub.local:8443/api/v1/operations/events?page_size=50"
curl -fsS --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/nodes
```

Open the Web Operations tab and verify Core/Public worker status, Service
Details, Logs, monitoring events, and graceful handling in the Hub tab. Public
workers remain disabled unless enabled in `.env`. To verify both public workers,
configure their target URLs/hosts and set
`DBMAP_PUBLIC_API_MONITOR_ENABLED=true` and
`DBMAP_PUBLIC_MQTT_MONITOR_ENABLED=true` in `.env`, then recreate Operations:

```bash
docker compose --profile operations up -d --force-recreate operations
curl -fsS --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/operations/status
```

Confirm Core and Public worker states in the response. For a local
restart-detection check (this interrupts the Hub API briefly), restart the Hub
and wait at least one Core polling interval:

```bash
docker compose --profile operations restart hub-bootstrap
sleep 70
curl -fsS --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/operations/services/nginx/details
curl -fsS --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" "https://hub.local:8443/api/v1/operations/events?page_size=50"
```

Nginx Details report its direct local health separately from
`hub_api_via_nginx`; Hub API restart state must not be used as Nginx-local
availability. To verify the separation across a complete polling cycle, during
a planned local maintenance window, stop Hub-bootstrap for at least 70 seconds,
request Nginx Details while it is stopped, then start Hub-bootstrap again. This
temporarily makes the Hub API unavailable; Nginx-local health should remain
healthy while `hub_api_via_nginx` is unavailable:

```bash
docker compose --profile operations stop hub-bootstrap
sleep 70
curl -fsS --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/operations/services/nginx/details
docker compose --profile operations start hub-bootstrap
```

Inspect a node's Device Monitor summary through
`GET /api/v1/nodes/{node_id}` and events through
`GET /api/v1/nodes/{node_id}/monitoring/events`. Device sampling cadence,
continuity, and bounded per-device retention remain Hub-owned.

For Operations-disabled verification:

```bash
docker compose --profile operations stop operations
COMPOSE_PROFILES= docker compose up -d --build hub-bootstrap nginx opa dbmap-web mosquitto
COMPOSE_PROFILES= docker compose ps
curl -fsS --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/hub/status
curl -fsS --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/nodes
curl -i --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/operations/status
```

Confirm the Hub and Devices UI/API remain available and the Operations route
returns the configured optional-service 503. Check a device detail and its
monitoring events to verify Device Monitor continues. No Hub database access by
Operations is needed.

Firmware Builder compatibility is independent: when the optional builder is
running, its dedicated status/build and log API remains available through the
Firmware tab (`GET /api/v1/firmware/builds` and
`GET /api/v1/firmware/builds/{build_id}/log`). Operations does not include
Firmware Builder in its allowlist and the retired Hub container-log route is
not used for build logs.

The Hub API continues to expose authenticated `/api/v1/hub/status` and
`/api/v1/hub/components/{component}/details`. The old
`/api/v1/hub/monitoring/events`,
`/api/v1/hub/components/{component}/logs`, and
`/api/v1/hub/components/{component}/connection-failures` endpoints are retired.
MQTT failures are available at
`/api/v1/operations/services/mosquitto/connection-failures` when Operations is
enabled. The status response no longer includes legacy Hub monitoring history.
These changes do not affect Device Monitor APIs or Firmware Builder APIs/logs.

### Historical database cleanup

Hub startup removes the allowlisted legacy tables after preserving the active
Device Monitor checkpoint when possible, as described above. No legacy history
is imported into Operations. Recover historical rows only by restoring a
pre-migration backup; Operations keeps its own independent history.
