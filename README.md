# dBmap

dBmap is a small, self-hosted system for listening to places — distributed acoustic sensing with a local Hub and ESP32 devices in the field.

Ears hear. Outputs can react. The Hub is the home those devices call: identity, configuration, telemetry, and a simple web console. The long-term idea is a map of sound and events across a site, not a single gadget demo.

This repository is **work in progress**. The local Hub stack runs today; the product around it is still taking shape.

## What works today

- A **local Hub** you run with Docker on a mini-PC (`linux/amd64`): MQTT, HTTPS API, web UI, and policy checks.
- **Ear** and **Output** firmware that share the same client path: Wi-Fi, TLS, provisioning, and MQTT call-home.
- USB-first registration and configuration from the Hub UI, plus a firmware catalog and optional on-Hub builder.
- Device and Hub **health / availability** history so you can see when things were actually reachable.
- Local admin and viewer access; simulators and scripts for Hub and Ear flows without hardware.

Richer map/rules, cross-Ear tracking, and regional Hub pieces are planned, not finished. See the working/todo table in [Local deployment](docs/local-deployment.md#working-today--still-to-do).

## What to do next

1. Stand up the local Hub — start with [Local deployment](docs/local-deployment.md).
2. Flash and register a device — wiring in [Device wiring and pinout](docs/device-wiring-and-pinout.md); registration and USB configure in the Hub UI.
3. Without Ear hardware, use the [Ear simulator](docs/simulator.md).
4. When you need ports, topics, or API shapes, use [Connectivity](docs/connectivity.md), [Protocol](docs/protocol.md), and [Hub API](docs/hub-api.openapi.yaml).

Other docs (authorization, identity, health, security) live under [`docs/`](docs/) and [`SECURITY.md`](SECURITY.md).

## Repository layout

```text
hub/bootstrap          Local Hub service
analysis/              Optional Analysis API (Compose profile `analysis`)
mqtt                   Broker config and TLS
web                    Hub UI
firmware/              Shared client, Ear, and Output
firmware-builder/      Optional ESP-IDF build service
simulator/             Ear and scenario tools
scripts/               Ops CLIs (`lib/` = shared helpers; `ear_test_runner.py` for physical EAR period tests — see `docs/ear-test-runner.md`; `hub_map_analysis.py` for Hub→analyze→map scratch — see `docs/hub-map-analysis.md`)
docs/                  How to run and how it fits together
compose.yaml           Local Hub stack
```

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

Policy tests from the repo root: `sh scripts/test-policy.sh`.
