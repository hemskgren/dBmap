# dBmap

Distributed acoustic sensing, built around **hub-bootstrap** and **ESP-Output**.

## Current slice

- The first hub profile is **LOCAL_MEDIUM** on an Intel mini-PC with `linux/amd64` containers; V0/V1 do not target Raspberry Pi.
- `hub-bootstrap` owns node identity, one-time bootstrap tokens, desired/reported state, and initial MQTT telemetry ingestion.
- ESP-Output supports NVS identity/configuration, Wi-Fi STA, HTTPS provisioning, MQTT hello/health/command/ack, and relay/audio stubs.
- The broker uses TLS on port `8883`; the bootstrap API uses HTTPS on port `8443`. Provisioned nodes receive unique MQTT credentials and topic-scoped permissions.
- The local stack includes a device overview at `/` behind Nginx. Separate local admin/viewer tokens control device-management actions and read-only device summaries; the API enforces those roles.
- The API sends authenticated actions to OPA for authorization. OPA is private to the Compose network; if it cannot return a decision, protected API requests fail closed.

## Documentation

- [Local deployment and operations](docs/local-deployment.md): mini-PC setup, certificates, ESP-IDF setup, node administration, simulators, and local workflows.
- [Protocol reference](docs/protocol.md): device and Hub API message formats.
- [Web architecture implementation brief](docs/dBmap_web_architecture_ai_instruction.md): draft direction for future web, identity, policy, and provisioning work.
- [Authorization policy](docs/authorization.md): OPA actions, current local roles, future owner/quota inputs, and policy test instructions.
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

Run Rego authorization tests from the repository root with `sh scripts/test-policy.sh` (requires OPA or Docker).

The hub container currently uses Python 3.12, the runtime used by the deployed local hub and verified test environment. Although the package metadata allows Python 3.11 and later, a test run on Python 3.14.4 currently fails during SQLAlchemy 2.0.36 ORM model initialization, before test collection completes. Keep the container on 3.12 until the ORM dependency is upgraded and the full suite and container build pass on a newer runtime.
