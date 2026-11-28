# dBmap

dBmap is a small, self-hosted system for listening to places — distributed acoustic sensing with a local Hub and ESP32 devices in the field.

Ears hear. Outputs can react. The Hub is the home those devices call: identity, configuration, telemetry, and a simple web console. The long-term idea is a map of sound and events across a site, not a single gadget demo.

This repository is **work in progress**. The local Hub stack runs today; the product around it is still taking shape.

## What works today

- A **local Hub** you run with Docker on a mini-PC (`linux/amd64`): MQTT, HTTPS API, web UI, and policy checks.
- **Ear** and **Output** firmware that share the same client path: Wi-Fi, TLS, provisioning, and MQTT call-home.
- USB-first registration and configuration from the Hub UI, plus a firmware catalog and optional on-Hub builder.
- Device connectivity and health history in the Hub; optional Operations infrastructure monitoring and availability history.
- Observation ingest and persistence in the Hub, with derived Event/Track analysis in scripts and optional Analysis scratch APIs.
- Local admin and viewer access; simulators and scripts for Hub and Ear flows without hardware.

Hub-persisted Event/Track services, richer map/rules, and regional Hub pieces are planned, not finished. Event/Track analysis is currently script-based; the optional Analysis API provides scratch/map-analysis endpoints, not separate Observation or Devices services. See the working/todo table in [Local deployment](docs/local-deployment.md#working-today--still-to-do) and [Sensor Platform architecture](docs/sensor-platform-architecture.md).

## What to do next

1. Stand up the local Hub — start with [Local deployment](docs/local-deployment.md).
2. Flash and register a device — wiring in [Device wiring and pinout](docs/device-wiring-and-pinout.md); registration and USB configure in the Hub UI.
3. Without Ear hardware, use the [Ear simulator](docs/simulator.md).
4. When you need ports, topics, or API shapes, use [Connectivity](docs/connectivity.md), [Protocol](docs/protocol.md), and [Hub API](docs/hub-api.openapi.yaml).

Other docs (authorization, identity, health, security) live under [`docs/`](docs/) and [`SECURITY.md`](SECURITY.md).

Long-term platform direction (shared Sensor Platform, products, Community/Enterprise profiles): [Sensor Platform architecture](docs/sensor-platform-architecture.md) and [ADR 0001](docs/adr/0001-sensor-platform-architecture.md).

AI coding agents: follow [`AGENTS.md`](AGENTS.md) for data-access, live-system safety, and related working rules (do not duplicate that file here).

## Repository layout

```text
hub/bootstrap          Hub API, device management, Observation ingest, Device Monitor
operations/            Optional infrastructure monitoring and diagnostics API
analysis/              Optional Analysis API and scratch persistence (profile `analysis`)
mqtt                   Broker config and TLS
nginx/                 HTTPS gateway and API routing
web/                   Hub UI, including optional Operations views
policy/                OPA authorization policy
firmware/              Shared client, Ear, and Output
firmware-builder/      Optional ESP-IDF build service
simulator/             Ear and scenario tools
scripts/               Hub/Operations and analysis CLIs (`lib/` = shared helpers)
docs/                  How to run and how it fits together
AGENTS.md              Canonical AI agent working instructions
compose.yaml           Local Hub stack
```

Observation and Device APIs are part of `hub-bootstrap`; Event/Track findings
are derived by `scripts/hub_event_track.py`, not persisted as Hub entities.
Analysis and Operations are optional Compose-profile services. Standalone
Observation and Devices containers are planned extractions and are not present
in the current deployment.

## Operations CLI

Use the read-only Operations CLI for infrastructure monitoring and diagnostics:

```bash
uv run python scripts/hub_operations.py status
uv run python scripts/hub_operations.py status --watch
uv run python scripts/hub_operations.py services
uv run python scripts/hub_operations.py details mqtt
uv run python scripts/hub_operations.py logs nginx
uv run python scripts/hub_operations.py events --page-size 50
uv run python scripts/hub_operations.py failed-connections
```

It uses the project `.env`, Hub CA, and authenticated Operations APIs.
Add `--output json` for machine-readable output. Events are paginated; the
CLI reports when a response is only a partial history. See
[Operations CLI documentation](docs/operations.md#operations-cli) for examples
and pagination details.

## Tests

Repo-wide lint (uses root `ruff.toml`; covers `scripts/`, `simulator/`, tests, etc.):

```bash
uv sync --group dev
uv run ruff check
```

From `hub/bootstrap`:

```bash
uv sync --extra dev
uv run ruff check app
uv run pytest tests ../../tests
```

From `analysis`:

```bash
uv sync --extra dev
uv run ruff check app
uv run pytest tests
```

From `operations`:

```bash
uv sync --extra dev
uv run ruff check app
uv run pytest tests
```

Policy tests from the repo root: `sh scripts/test-policy.sh`.
