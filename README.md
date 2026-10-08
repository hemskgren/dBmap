# dBmap

Distributed acoustic sensing, built around **hub-bootstrap** and **ESP-Output**.

## Current slice

- The first hub profile is **LOCAL_MEDIUM** on an Intel mini-PC with `linux/amd64` containers; V0/V1 do not target Raspberry Pi.
- `hub-bootstrap` owns node identity, one-time bootstrap tokens, desired/reported state, and initial MQTT telemetry ingestion.
- ESP-Output supports NVS identity/configuration, Wi-Fi STA, HTTPS provisioning, MQTT hello/health/command/ack, and relay/audio stubs.
- The broker uses TLS on port `8883`; the bootstrap API uses HTTPS on port `8443`. Provisioned nodes receive unique MQTT credentials and topic-scoped permissions.
- The local stack includes a minimal web UI at `/` and an Nginx edge proxy that forwards `/api/` to the existing API. Backend authorization remains authoritative.

## Documentation

- [Local deployment and operations](docs/local-deployment.md): mini-PC setup, certificates, ESP-IDF setup, node administration, simulators, and local workflows.
- [Protocol reference](docs/protocol.md): device and Hub API message formats.
- [Web architecture implementation brief](docs/dBmap_web_architecture_ai_instruction.md): draft direction for future web, identity, policy, and provisioning work.
- [Security policy](SECURITY.md): secret handling and private vulnerability reporting.

## Repository layout

```text
hub/bootstrap                         Local Hub identity + desired/reported state
mqtt                                  Mosquitto config, Dynamic Security, local TLS
firmware/components/node_common      Shared Ear/Output client
firmware/esp-output                  First L0 firmware
simulator/ear                         Ear observation and scenario simulators
scripts                               Hub summary, admin, and observation tools
docs                                  Deployment, protocol, and architecture documentation
tests                                 Tests for scripts and simulator
compose.yaml                         LOCAL_MEDIUM stack
```

Not in this slice: `hub-event`, rules, Ear DSP, Home Assistant, Regional Hub.

## Tests and lint

From `hub/bootstrap`:

```bash
uv sync --extra dev
uv run ruff check app
uv run pytest tests ../../tests
```

The hub container currently uses Python 3.12, the runtime used by the deployed local hub and verified test environment. Although the package metadata allows Python 3.11 and later, a test run on Python 3.14.4 currently fails during SQLAlchemy 2.0.36 ORM model initialization, before test collection completes. Keep the container on 3.12 until the ORM dependency is upgraded and the full suite and container build pass on a newer runtime.
