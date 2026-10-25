# dBmap

Distributed acoustic sensing, built around **hub-bootstrap** and **ESP-Output**.

## Current slice

- The first hub profile is **LOCAL_MEDIUM** on an Intel mini-PC with `linux/amd64` containers; profile for Raspberry Pi is planed.
- `hub-bootstrap` owns node identity, one-time bootstrap tokens, desired/reported state, and initial MQTT telemetry ingestion.
- ESP-Output supports NVS identity/configuration, Wi-Fi STA, HTTPS provisioning, MQTT hello/health/command/ack, and relay (GPIO16)/audio stubs.
- The broker uses TLS on port `8883`; the web UI and API are reached through Nginx over HTTPS on port `8443` by default. Provisioned nodes receive unique MQTT credentials and topic-scoped permissions.
- The local stack includes Hub status, a dynamic device summary and list, a recent-observations tab with expandable JSON and Ear site distances, and an offline ESPConnect workspace at `/espconnect/`. Hub component details, including Nginx, Web, and the optional firmware builder, are fetched only when opened from the Hub overview. Administrators can also read the latest container log lines from that list. ESPConnect opens in a separate window and does not receive the Hub token. Separate local admin/viewer tokens control device-management actions and read-only status, device, and observation access; the API enforces those roles.
- The API sends authenticated actions to OPA for authorization. OPA is private to the Compose network; if it cannot return a decision, protected API requests fail closed.

## Documentation

- [Local deployment and operations](docs/local-deployment.md): mini-PC setup, certificates, ESP-IDF setup, node administration, simulators, and local workflows.
- [Connectivity map](docs/connectivity.md): clients, protocols, ports, and which functions share a process.
- [Protocol reference](docs/protocol.md): device and Hub API message formats.
- [Hub API](docs/hub-api.openapi.yaml): OpenAPI 3.0 description of the Hub HTTPS API.
- [Authorization policy](docs/authorization.md): OPA actions, current local roles, future owner/quota inputs, and policy test instructions.
- [User identity and operating modes](docs/identity-and-operating-modes.md): current local access, optional multi-user design, account lifecycle, and identity separation.
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

The hub container uses Python 3.14 with SQLAlchemy 2.0.54. The full Hub and repository test suite passes on Python 3.14.4. Package metadata continues to allow Python 3.11 and later.
