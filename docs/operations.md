# Operations Service

Operations is dBmap's optional monitoring service. It owns Core Hub,
public endpoint, service health, and monitoring history responsibilities.
It does not manage Devices or Observation reports.

Operations is a Compose profile and can be absent. Hub-bootstrap, MQTT,
dbmap-web, Nginx, EAR communication, and Observation processing do not depend
on it.

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
- `probes.py` implements direct HTTPS and MQTT probe clients.
- `monitoring_cycle.py`, `monitoring_availability.py`, and
  `monitoring_math.py` reuse the tested Hub monitoring algorithms.
- `models.py` owns the Operations database schema.
- `api.py` implements authenticated API routes.
- `policy.py` provides the existing OPA authorization pattern.
- `db.py` separates database setup and sessions from domain logic.

Operations has no Docker socket mount. It obtains supplementary Hub service
diagnostics and allowlisted container runtime/log data through narrow,
authenticated Hub-bootstrap APIs. Direct network probes remain independent:
they continue when the runtime bridge or Hub API is unavailable. Operations
does not receive full Docker inspection data, arbitrary Docker API access,
container environment variables, or internal filesystem paths.

The Hub-bootstrap runtime bridge is a temporary lift-and-shift dependency.
Hub-bootstrap retains the existing Docker socket mount and allowlisted
Compose-project filtering. A read-only Docker socket mount would not constrain
Docker API operations; the current socket access is therefore a significant
security limitation and is not the final least-privilege production design.
Operations introduces no Docker Socket Proxy or additional privileged
infrastructure.

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

Operations uses a dedicated `operations-data` volume. Legacy monitoring remains
owned by `hub-bootstrap` and `bootstrap.db`.

### Configuration

| Environment variable | Default | Purpose |
| --- | --- | --- |
| `DBMAP_ADMIN_TOKEN` | Required | Service-to-service authentication to Hub-bootstrap |
| `DBMAP_HUB_ID` | `local-01` | Logical Hub identity |
| `DBMAP_HUB_API_HOST` | `hub-bootstrap` | Hub-owned diagnostics/runtime bridge host |
| `DBMAP_HUB_API_PORT` | `8443` | Hub-bootstrap HTTPS port |
| `DBMAP_PUBLIC_BASE_PATH` | Empty | Public URL prefix used for Nginx health probes |
| `DBMAP_CORE_MONITOR_ENABLED` | `true` | Enable the Core worker |
| `DBMAP_CORE_MONITOR_INTERVAL_S` | `60` | Core polling interval |
| `DBMAP_GAP_THRESHOLD_S` | `120` | Monitoring gap threshold |
| `DBMAP_RETENTION_DAYS` | `30` | Monitoring history retention |
| `DBMAP_PUBLIC_API_MONITOR_ENABLED` | `false` | Enable Public API polling |
| `DBMAP_PUBLIC_MQTT_MONITOR_ENABLED` | `false` | Enable Public MQTT polling |
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
- `GET /api/v1/operations/events`

Hub-bootstrap exposes the following narrow internal interfaces for Operations:

- `GET /api/v1/internal/operations/services/{service}/runtime`
- `GET /api/v1/internal/operations/services/{service}/logs`

They allow only `operations`, `hub-api`, `nginx`, `web`, `mqtt`, and `opa`.
Runtime responses contain a sanitized container identity, running/health
state, start time, and restart count. The log interface is read-only and
bounded; it does not expose Docker API operations or full inspection data.
The existing Hub Details and database diagnostic interfaces remain owned by
Hub-bootstrap.

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
  "runtime_source": "hub-bootstrap Docker runtime interface"
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
historical log pagination, and explicitly identifies the
`hub-bootstrap Docker runtime interface` as its source. For example:

```json
{"service":"Operations","lines":["Operations service started"],"line_limit":30,"source":"hub-bootstrap Docker runtime interface"}
```

The shared Docker log reader caps the response body at 1,000,000 bytes. It
selects only the newest container belonging to the configured Compose project
and allowlisted Compose service. Hub MQTT failed-connection parsing remains
Hub-owned and is not migrated by Operations.

| Monitored service | Direct Details signal | Supplementary diagnostic source | Logs |
| --- | --- | --- | --- |
| Operations | Own worker and SQLite status | Hub-bootstrap runtime bridge | Allowlisted 30-line tail |
| Hub API | Direct HTTPS health | Hub-bootstrap API and database diagnostics; runtime bridge | Allowlisted 30-line tail |
| Nginx | Nginx-local HTTPS health and separate Hub API-through-Nginx result | Hub-bootstrap Nginx details and runtime bridge | Allowlisted 30-line tail |
| Web | Direct HTTPS health | Hub-bootstrap Web details and runtime bridge | Allowlisted 30-line tail |
| MQTT broker | Direct MQTT TLS connection | Hub-bootstrap broker metrics and runtime bridge | Allowlisted 30-line tail |
| OPA | Direct OPA health | Hub-bootstrap policy health summary and runtime bridge | Allowlisted 30-line tail |
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

Core reachability, MQTT, and OPA rules reuse the existing Hub monitoring
calculations:

- Reachability failure is unhealthy/unavailable.
- MQTT failure with reachability intact is degraded/unavailable for Core Hub
  availability.
- OPA-only failure is degraded but available for Core Hub availability.
- Unknown or missing evidence remains unobserved.

Public API and Public MQTT samples have independent workers and checkpoints.
Core and public MQTT clients use separate unique clean-session client identities.

The existing Core Hub aggregate retains its previous reachability semantics:
the `nginx` sample is the end-to-end Hub `/health` request through Nginx.
`nginx_local` is sampled separately and shown in component details, but does
not alter that aggregate. Nginx serves `GET /api/v1/nginx/health` directly with
a JSON `200`; the existing `/health` endpoint still proxies to Hub-bootstrap.

Restart detection remains owned by Hub Monitor. Operations periodically reads
the latest 50 Hub monitoring events and mirrors `service_restart_detected`
events, preserving event identity, timestamps, state, source, and inferred
certainty. Duplicate event IDs are ignored. A detected restart does not prove
downtime. If more than 50 restart events occur between successful syncs,
older events may not be mirrored.

## Database and persistence

Operations owns its independent:

- `operations_monitor_samples`
- `operations_monitor_events`
- `operations_monitor_checkpoints`
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

Log retrieval requires `operations.logs.read`. Runtime diagnostics require
`operations.details.read`. When Operations requests delegated diagnostics,
runtime metadata, restart events, or logs, it uses its configured admin bearer
token to call the Hub-owned endpoints. Hub-bootstrap continues to enforce
`hub.status.read` for diagnostics/runtime/event reads and `hub.logs.read` for
container logs. These internal interfaces expose only allowlisted diagnostics
and bounded log tails.

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
container logs used by Operations. Operations does not mount the socket;
hub-bootstrap remains the runtime/log access owner during this side-by-side
stage.

## Known limitations

- Hub database details, delegated service diagnostics, runtime metadata,
  restart-event mirroring, and logs depend on authenticated Hub-bootstrap APIs.
  If Hub-bootstrap is unavailable, those details are unavailable; direct
  Operations probes continue.
- Hub-bootstrap retains Docker socket access, which is not suitable as a final
  least-privilege production architecture.
- Restart event mirroring reads a bounded latest-50 event list and can miss
  older events if the feed advances by more than 50 events between syncs.
- Public API/Public MQTT monitoring is optional and disabled by default.
- Public and Core workers have independent schedules and need comparison rather
  than exact timestamp equality.
- Legacy Hub monitoring and Device Monitor remain active in hub-bootstrap.
- Web and CLI consumers are not switched to Operations by default.

## Local side-by-side verification

Build and start the optional Operations service and the services it calls:

```bash
docker compose --profile operations up -d --build hub-bootstrap nginx opa operations dbmap-web
docker compose --profile operations ps
```

Compare the legacy Hub monitoring status with the Operations status using the
same local CA and bearer token:

```bash
curl --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/hub/status
curl --cacert shared/trust/ca.crt -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" https://hub.local:8443/api/v1/operations/status
```

Open the Web Operations tab to compare the independent availability signals,
Details, and on-demand Logs with the unchanged Hub tab. To compare direct Nginx
availability with the existing upstream-dependent health route:

```bash
curl --cacert shared/trust/ca.crt https://hub.local:8443/api/v1/nginx/health
curl --cacert shared/trust/ca.crt https://hub.local:8443/health
```
