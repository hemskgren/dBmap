# dBmap

Distributed acoustic sensing. This repository starts from **hub-bootstrap** and **ESP-Output**, not from a Raspberry Pi Ear prototype.

The architecture document still says “Pi first” and “V0-A single Ear”. Hardware and deployment here are different:

- Initial ESP-Output bootstrap test: ESP32-WROOM-32 (classic ESP32, 4 MB flash, no PSRAM).
- Planned ESP-Ear hardware: ESP32-S3 with 16 MB flash. Ear firmware will reuse `firmware/components/node_common`; it is separate from this initial Output test.
- First hub profile: **LOCAL_MEDIUM** — Intel mini-PC, `linux/amd64` containers.
- V0/V1 do **not** target Raspberry Pi.
- The mini-PC is expected to be used as a **Cursor remote / SSH host**.

## Current slice

`hub-bootstrap` owns node identity, one-time bootstrap tokens, desired/reported state, and initial MQTT telemetry ingestion.

ESP-Output firmware supports NVS identity/configuration, Wi-Fi STA, HTTPS provisioning, MQTT hello/health/command/ack, and relay/audio stubs (no pin map yet).

The broker is TLS-only on port `8883`; the web UI and API are served through Nginx over HTTPS on port `8443` by default. `DBMAP_HTTPS_PORT` can change that listener and host port together. Hub bootstrap, web, and OPA listeners are internal to Compose. Each provisioned node gets a unique MQTT login and topic-scoped permissions. Hub services use a separate broker identity. The local CA is trusted by firmware and clients; keep its private key and `.env` secrets private.

The local deployment now includes a minimal edge proxy and web frontend as part of the Phase A architecture refactor:

- `https://<hub-host>:8443/` serves the lightweight dBmap web UI
- `https://<hub-host>:8443/api/...` proxies to the Hub bootstrap API
- Hub bootstrap is reachable only on the internal Compose network; Nginx proxies to it using verified HTTPS
- Nginx also verifies HTTPS to the web service and OPA using separate certificates
- Set `DBMAP_PUBLIC_BASE_PATH=/dbmap` (or `/hub`) to serve the UI and API below that path; leave it empty for root deployment. The browser UI uses the configured prefix, and CLI/simulator tools read it from the environment or accept `--base-path`.
- authorization is still enforced by the backend API; the browser UI is not a trust boundary
- Nginx verifies the Hub's upstream TLS certificate using the stable `hub.local` certificate name, regardless of whether clients connect to the proxy by IP or hostname

The Hub API delegates role and action decisions to OPA. OPA is attached to a private Compose network shared only with Hub bootstrap and has no host-published port. Hub-to-OPA calls use HTTPS and validate OPA's certificate against the local CA. This is TLS server authentication, not mutual TLS. The default decision URL is `https://opa:8181/v1/data/dbmap/authz/allow`; deployments can override it with `DBMAP_OPA_DECISION_URL`, which must use HTTPS. If OPA is unavailable or returns no boolean decision, protected API requests return HTTP 503 rather than being allowed. The Rego policy is mounted read-only from `policy/`.

Run the policy unit tests from the repository root with:

```bash
sh scripts/test-policy.sh
```

See the [security policy](../SECURITY.md) for private vulnerability reporting and secret-handling guidance.

## Mini-PC bring-up

Do this on the Intel host. Choose its stable LAN IPv4 address (a DHCP reservation is recommended). `hub.local` is the hostname that devices and clients use; it resolves to this address through mDNS. The numeric address is still needed by Docker to bind the published ports specifically to the LAN interface, and is included in the TLS certificate for clients that connect by IP. It is not a replacement for `hub.local`.

### If `.env` already exists

Do **not** run `init-local-secrets.sh`; it creates `.env` only when the file does not exist and deliberately refuses to overwrite an existing one. Edit the existing `.env` and make sure it contains:

```dotenv
DBMAP_LAN_IP=<mini-PC-LAN-IP>
DBMAP_HTTPS_PORT=8443
DBMAP_PUBLIC_BASE_PATH=
DBMAP_ADMIN_TOKEN=<random-secret-1>
DBMAP_VIEWER_TOKEN=<random-secret-2>
DBMAP_MQTT_HUB_PASSWORD=<random-secret-3>
DBMAP_DYNSEC_ADMIN_PASSWORD=<random-secret-4>
DBMAP_ADVERTISED_MQTT_HOST=hub.local
DBMAP_ADVERTISED_MQTT_PORT=8883
```

Replace the angle-bracket values with real values (do not type the brackets). Set `DBMAP_LAN_IP` to the NUC's address. Generate a separate secret for each of the four token/password entries by running `openssl rand -hex 32` four times. Keep admin and viewer tokens distinct; do not keep the earlier `change-me-admin-token` value or reuse the old shared MQTT password. Keep `.env` private; it should have mode `0600`:

```bash
chmod 600 .env
```

### If `.env` does not exist

Create it once with the script; this generates four independent secrets and sets restrictive file permissions:

```bash
./scripts/init-local-secrets.sh <mini-PC-LAN-IP>
```

### Generate certificates and start the services

Compose reads `.env` automatically when you run it from the project directory. The following `source` step is separate: it loads the values into this terminal so the certificate script can use `DBMAP_LAN_IP`.

```bash
set -a
. ./.env
set +a
./scripts/gen-mqtt-certs.sh
docker compose up --build -d
```

The certificate generator preserves an existing local CA and refreshes the Hub/Nginx/broker certificate for `hub.local`, the broker's internal `mosquitto` name, and the NUC's current LAN IP. It also creates separate `dbmap-web` and `opa` certificates for verified service-to-service HTTPS. OPA uses HTTPS only and remains unpublished to the host. If it reports that an existing CA is unsuitable, run `./scripts/gen-mqtt-certs.sh --rotate-ca` to replace it. CA rotation invalidates trust on previously flashed devices; copy the new CA and erase/reflash each device.

The local CA is not automatically trusted by browsers or operating systems. Install `mqtt/certs/ca.crt` in the trusted root store on each client using that platform's documented process, then browse to `hub.local` or the certificate-covered LAN IP. For command-line requests, pass `--cacert mqtt/certs/ca.crt`. The server certificate covers `hub.local`, `localhost`, `127.0.0.1`, and the configured LAN IP. Do not treat a browser certificate warning as successful verification.

Optional mDNS publishing can be started on a host with Avahi installed:

```bash
./scripts/publish-hub-mdns.sh "$DBMAP_LAN_IP"
```

Keep the publisher running while you need the name; it stops advertising when the process exits. Publishing is optional and a missing/unavailable Avahi installation does not affect the Hub. Compose publishes Nginx on `DBMAP_HTTPS_PORT` (default `8443`) and Mosquitto on `8883` at `DBMAP_LAN_IP`. The Hub API, web, and OPA ports remain internal to Compose. An existing Nginx can terminate public HTTPS on 443 and proxy to `https://<hub-host>:8443`, verifying the local CA.

For the first ESP32 test, use the hub's numeric LAN IP in the firmware. `hub.local` works for host tools via mDNS, but this firmware does not include an explicit mDNS resolver. The IP is already included in the generated server certificate. Set the firmware's Hub base URL to `https://<mini-PC-LAN-IP>:8443` by default (or the configured listener port). Set `DBMAP_ADVERTISED_MQTT_HOST` in `.env` to the same LAN IP as `DBMAP_LAN_IP` and apply the setting so the provisioning response gives that reachable address to the ESP32:

```bash
sudo docker compose up -d
```

Create a **new** Output node/token for this secure firmware test. Load the private settings into the current shell before the request:

```bash
set -a
. ./.env
set +a
export CURL_CA_BUNDLE=mqtt/certs/ca.crt
curl -sS -H "Authorization: Bearer ${DBMAP_ADMIN_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"node_type":"output","hardware_revision":"OUTPUT-DEV-V1"}' \
  "https://${DBMAP_LAN_IP}:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/api/v1/nodes"
```

The browser UI at `https://hub.local:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/` accepts either `DBMAP_ADMIN_TOKEN` or `DBMAP_VIEWER_TOKEN`. The Hub tab shows local mode and component status. Click **Details** beside Hub API, Database, Policy engine, or MQTT broker to fetch that component's summary; those detail requests do not run until clicked. Database details include the SQLite version, storage use, and table row counts. Policy details show OPA HTTPS health and plugin readiness. MQTT details show the broker metrics received by the Hub over its existing TLS connection. The Devices tab summarizes device counts, lifecycle, availability, and the device list. The Observations tab shows up to the latest 100 observations with expandable JSON, plus Ear installation distances as site context only. It lists all pairwise distances when there are fewer than 10 registered Ears; for 10 or more, it shows only the shortest and longest valid distances and points to `scripts/hub_event_track.py` for detailed site analysis. The viewer token grants read-only access to device, Hub, and observation data; the admin token additionally enables device creation and lifecycle actions. Tokens are held in browser memory only and cleared on sign-out or page reload. The browser is not the security boundary: the API independently checks the token and role. Device status is online when the Hub received a keepalive within the previous 90 seconds; otherwise it is offline.

The ESPConnect tab opens the checked-in ESPConnect v1.1.23 static build in a separate browser window at `https://hub.local:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/espconnect/`. It needs no internet connection for its app assets or USB board inspection; its help and release-note links are external. The downloaded release is MIT-licensed; its upstream license and source release are recorded under `web/espconnect/`. Use Chromium 89+ and a data-capable USB cable, accept the browser's device permission prompt, and install the local CA so the Hub is a trusted HTTPS origin. ESPConnect only inspects the USB device in the browser; it does not receive the Hub token. Reading device information into the dBmap registration form is not wired yet.

The bootstrap token is returned only once; the hub stores its hash, so it cannot be displayed again. Save the returned token privately before continuing. If it is lost, repeat the node-creation request to get a new node ID and token; the old pending node can be left unused. Do not post the token in chat or logs. Before building firmware, embed the local CA so the ESP32 can verify HTTPS and MQTT server certificates:

```bash
cp mqtt/certs/ca.crt firmware/esp-output/main/root_ca.pem
```

In firmware menuconfig, under **dBmap node**, set the Wi-Fi SSID and password, hub bootstrap base URL to `https://<mini-PC-LAN-IP>:8443` (actual IP, without angle brackets; use your configured listener port), the one-time bootstrap token returned by the node-creation request, and hardware revision to `OUTPUT-DEV-V1`. The token field must not be left empty on the board's first provisioning boot: NVS is empty after `erase-flash`, and the token is not saved there before provisioning. Do not use `localhost` on the board; it refers to the ESP32 itself. The ESP32-WROOM-32 test uses the `esp32` target and 4 MB flash, not the planned ESP32-S3/16 MB Ear target.

## Install ESP-IDF (Linux)

`idf.py` is part of Espressif's ESP-IDF toolchain, not a Python project dependency. Install the Linux prerequisites and pinned ESP-IDF release once:

```bash
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

mkdir -p ~/esp
git clone --recursive --branch v5.4.2 \
  https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh esp32
```

Activate ESP-IDF in each terminal used to build firmware:

```bash
. "$HOME/esp/esp-idf/export.sh"
idf.py --version
```

On Linux, if `/dev/ttyACM0` is owned by group `dialout`, add your user to that group once, then fully log out and back in:

```bash
sudo usermod -aG dialout "$USER"
```

Verify serial access after reconnecting:

```bash
id -nG
test -r /dev/ttyACM0 && echo "serial port readable"
```

Build, flash, and monitor using the serial port present on your machine (`/dev/ttyACM0` or `/dev/ttyUSB0` are common):

```bash
. "$HOME/esp/esp-idf/export.sh"
cd firmware/esp-output
idf.py set-target esp32
idf.py menuconfig
idf.py -p /dev/ttyACM0 erase-flash
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Erasing is important when reusing a board that has old lab credentials in NVS. Change `/dev/ttyACM0` to the port shown on your system. To leave the serial monitor, press `Ctrl+]`.

After flashing, expect Wi-Fi connection, a `provisioned as OUT-...` message, and a verified TLS MQTT connection. In a terminal at the repository root, load `.env` and list provisioned nodes with:

```bash
set -a
. ./.env
set +a
export CURL_CA_BUNDLE=mqtt/certs/ca.crt
curl -sS -H "Authorization: Bearer ${DBMAP_ADMIN_TOKEN}" \
  "https://${DBMAP_LAN_IP}:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/api/v1/nodes"
```

### Record installation metadata

Installation metadata is stored by the hub, not reported by ESP firmware. Each record must include the node's geographic position as WGS84 latitude and longitude in decimal degrees; other placement details are optional. Replace the example coordinates below with the actual installation coordinates:

```bash
curl -sS --cacert mqtt/certs/ca.crt \
  -H "Authorization: Bearer ${DBMAP_ADMIN_TOKEN}" \
  -H "Content-Type: application/json" \
  -X PUT \
  -d '{"latitude":59.91,"longitude":10.75,"floor":7,"height_m":21,"height_accuracy_m":2,"mount_type":"balcony","environment":"urban","orientation_deg":180}' \
  "https://${DBMAP_LAN_IP}:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/api/v1/nodes/<node-id>/installation"
```

The response includes the saved `installation` object. `latitude` must be between -90 and 90, `longitude` between -180 and 180, and optional `orientation_deg` between 0 (inclusive) and 360 (exclusive). Update a node's record by repeating the PUT with its complete installation object; geographic coordinates remain required.

### Simulate an ESP-Ear observation

The simulator is kept separate from device firmware under `simulator/ear/`. It registers with a `SIM-EAR-*` identity (for example, `SIM-EAR-001`), so simulator records are distinguishable from physical `EAR-*` nodes. Its observation payload uses protocol version 1 and the `observation` message type. The hub validates that envelope and the observation fields before storing anything. The existing Output firmware protocol is unchanged.

The hub must be running the current repository version; changing local source files does not update an already running container. After pulling or editing hub code, rebuild and recreate the hub service on the host:

```bash
sudo docker compose up --build -d hub-bootstrap
sudo docker compose ps
```

The simulator creates and provisions a test Ear using the hub API, saves its one-time MQTT credentials in a mode-`0600` file, then publishes one sample observation to that Ear's TLS-protected topic. Run from the repository root after loading `.env`; `DBMAP_LAN_IP` must be reachable and included in the local TLS certificate:

```bash
uv run --project hub/bootstrap python simulator/ear/simulate_observation.py \
  --hub-host "$DBMAP_LAN_IP"
```

The simulator reports a node ID and observation ID, not the credentials. Query the authenticated observations API:

```bash
curl -sS --cacert mqtt/certs/ca.crt \
  -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" \
  "https://${DBMAP_LAN_IP}:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/api/v1/observations?node_id=<node-id-from-simulator>"
```

Replace `<node-id-from-simulator>` with the `SIM-EAR-*` node ID printed by the simulator. The response contains the validated protocol envelope, observation and hub `received_time_utc`. Classification and bearing have separate confidence values. Bearing is relative to the node reference axis, not a compass direction; the hub does not yet convert it using installation orientation. `signal_level_dbfs` is digital full-scale, not dB SPL. Each observation also includes a UUID, sequence number, timezone-aware event timestamp, monotonic capture timestamp, and timing-quality and uncertainty fields. Repeating publication with the same observation ID is deduplicated. To retry a publish after an error without provisioning another test node, run the command again with `--credentials-file /tmp/dbmap-ear-simulator-credentials.json --reuse-credentials`. Keep that file private; delete it when the simulator credentials are no longer needed.

To publish a keepalive for that already provisioned simulated Ear, use the saved credentials file. This does not call the Hub API or create another device; it publishes one TLS keepalive, which refreshes the device's reported state and online status:

```bash
uv run --project hub/bootstrap python simulator/ear/simulate_keepalive.py \
  --credentials-file /tmp/dbmap-ear-simulator-credentials.json
```

Add `--watch` to publish every 30 seconds until `Ctrl+C`, or set `--interval-seconds 15` to change the cadence. The simulator verifies that the credentials file is private and the keepalive topic belongs to its `SIM-EAR-*` node. The local CA is used to validate the broker certificate.

To simulate several observations from the same synthetic vehicle, use a finite count. Each observation gets a new UUID and increasing sequence number; all share the supplied synthetic source ID. The default delay between publications is 15 seconds:

```bash
uv run --project hub/bootstrap python simulator/ear/simulate_observation.py \
  --hub-host "$DBMAP_LAN_IP" \
  --credentials-file /tmp/dbmap-ear-simulator-credentials.json \
  --reuse-credentials \
  --count 4 \
  --interval-seconds 15 \
  --vehicle-id SIM-VEHICLE-001
```

The single-observation default remains an idempotent retry using the saved observation ID. Multi-observation mode advances and persists its sequence number after each successful publish, so rerunning it starts new observations. The synthetic source ID is ground truth for later evaluation only; it is not a real acoustic identity and the tracker must not use it for association.

### Run a time-ordered Ear scenario

For repeatable tests, `simulator/ear/simulate_scenario.py` loads a **Scenario** describing one ground-truth source moving along a route, Ear site coordinates/orientations, and explicit observation times/Ear aliases. The source follows a constant-speed great-circle path between start and end. For each scheduled detection, the simulator calculates source position, source-to-Ear distance, and node-relative bearing; no acoustic propagation or audibility model is implied. Classification/bearing confidence and signal level remain explicit per-observation scenario inputs. Each generated message uses the same version-1 Observation envelope as an Ear and has its own ID and per-Ear sequence number. `ground_truth_id` is included in the observation only as a test hint; the tracker must not read it or use it for candidate grouping. It is available for a separate after-the-fact comparison once a tracker produces predicted associations. `--time-scale` changes only wall-clock delays; logical observation times still follow the scenario timeline.

The example `vehicle_pass_01` has a 500 m route at 12 m/s and observations at t=0, 2, 4, 6, and 15 seconds. Its two scenario Ear positions should match the registered nodes' installation coordinates and orientations in the Hub; set those installation records before using spatial tracker analysis. The aliases `ear_a` and `ear_b` map to credentials files. First, create or reuse one provisioned simulator credentials file per alias. If you do not already have two, create them with distinct paths (each command registers/provisions an Ear and publishes one initial test observation):

```bash
uv run --project hub/bootstrap python simulator/ear/simulate_observation.py \
  --hub-host "$DBMAP_LAN_IP" \
  --credentials-file /tmp/dbmap-ear-a.json

uv run --project hub/bootstrap python simulator/ear/simulate_observation.py \
  --hub-host "$DBMAP_LAN_IP" \
  --credentials-file /tmp/dbmap-ear-b.json
```

The MQTT host is saved in each credentials file when that Ear is provisioned. Set `DBMAP_ADVERTISED_MQTT_HOST` in `.env` to a host/IP reachable from the machine running this scenario before provisioning; existing credentials are not updated when `.env` changes.

Validate the scenario and credentials aliases locally before publishing. Dry-run does not need the Hub token, CA, broker, or credentials file contents:

```bash
uv run --project hub/bootstrap python simulator/ear/simulate_scenario.py \
  --scenario-file simulator/ear/scenarios/vehicle_passes.json \
  --ear ear_a=/tmp/dbmap-ear-a.json \
  --ear ear_b=/tmp/dbmap-ear-b.json \
  --dry-run
```

Publish it to the already-running Hub over TLS MQTT, accelerated to 10% of wall-clock time:

```bash
uv run --project hub/bootstrap python simulator/ear/simulate_scenario.py \
  --scenario-file simulator/ear/scenarios/vehicle_passes.json \
  --ear ear_a=/tmp/dbmap-ear-a.json \
  --ear ear_b=/tmp/dbmap-ear-b.json \
  --time-scale 0.1
```

If a publish fails or the run is interrupted, the credentials file preserves the exact in-flight observation. Resolve that observation with the same scenario and alias mappings before starting another run:

```bash
uv run --project hub/bootstrap python simulator/ear/simulate_scenario.py \
  --scenario-file simulator/ear/scenarios/vehicle_passes.json \
  --ear ear_a=/tmp/dbmap-ear-a.json \
  --ear ear_b=/tmp/dbmap-ear-b.json \
  --retry-pending
```

Inspect the resulting records with the existing read-only explorer:

```bash
uv run --project hub/bootstrap python scripts/hub_event_track.py \
  --hub-host "$DBMAP_LAN_IP" \
  --watch
```

This lets us exercise observation ingestion, review the independent tracker input, and compare it with scenario ground truth only after the tracker has produced predictions. It does not change or require rebuilding the Hub container.

Before deploying the revised version-1 observation shape, clear the local hub database. This removes all node registrations, installation metadata, desired/reported state, bootstrap tokens, hub-side MQTT credentials, and observations. It does not remove Mosquitto's dynamic-security users or the TLS certificates. Run these commands from a shell with Docker access (for example, after `newgrp docker`); the one-off container only deletes rows from the existing `bootstrap-data` volume:

```bash
docker compose stop hub-bootstrap
docker compose run --rm --no-deps --entrypoint python hub-bootstrap -c 'import os,sqlite3; p="/data/bootstrap.db"; assert os.path.isfile(p), "Hub database not found"; db=sqlite3.connect(p); db.executescript("BEGIN IMMEDIATE; DELETE FROM observations; DELETE FROM installation_metadata; DELETE FROM bootstrap_tokens; DELETE FROM node_credentials; DELETE FROM desired_state; DELETE FROM reported_state; DELETE FROM nodes; COMMIT;"); tables=("observations","installation_metadata","bootstrap_tokens","node_credentials","desired_state","reported_state","nodes"); print({table: db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0] for table in tables}); db.close()'
docker compose up --build -d hub-bootstrap
```

Do this only when you intend to discard all local hub test data. Then use a new credentials file to create and provision a fresh simulated Ear; the first observation will use the revised structure:

```bash
uv run --project hub/bootstrap python simulator/ear/simulate_observation.py \
  --hub-host "$DBMAP_LAN_IP" \
  --credentials-file /tmp/dbmap-ear-simulator-v1.json
```

For a concise, authenticated list of Hub service statuses, run:

```bash
uv run --project hub/bootstrap python scripts/hub_summary.py \
  --hub-host "$DBMAP_LAN_IP"
```

The default output shows the Hub mode and ID, one status line each for Hub API, Database, Policy engine, and MQTT broker, and a concise device summary with counts by type, lifecycle, availability, and installation state. Use `--output json` for machine-readable output from the Hub status endpoint. Add `--watch` to poll service statuses every 15 seconds; text mode reports service changes, while JSON mode emits one JSON object per poll. Use `--interval-seconds 30` to change the interval. Component details such as broker metrics remain available on demand in the web UI. Stop watch mode with `Ctrl+C`.

### Explore observation source hints

`scripts/hub_event_track.py` is a read-only observation/site report, not an event/track engine or API. It fetches node installation metadata, prints each observation independently, and reports pairwise Ear site distances as context. It deliberately does **not** read `classification.hints.simulated_source_id` for grouping or correlation; candidate output currently keeps each observation separate. Per-Ear inactivity episodes are only time summaries and do not assert common source identity. The tracker does not yet produce predicted cross-Ear associations or Tracks.

The default 15-second inactivity window closes a per-Ear activity summary only after that much silence following an observation interval (`event_time_utc + duration_ms`). This is a time boundary, not evidence that observations inside the episode came from the same source; simultaneous sources can still be distinct. The summary is only printed, never persisted or used to control a node. The tool does not create Events/Tracks or infer geographic source positions from bearing. Watch mode re-fetches observations and node metadata, then reprints the analysis when new observations arrive.

```bash
uv run --project hub/bootstrap python scripts/hub_event_track.py \
  --hub-host "$DBMAP_LAN_IP"
```

To monitor and recalculate the report every 15 seconds, add `--watch`; customize the polling interval with `--interval-seconds 30`. Change the per-Ear inactivity summary window with `--event-gap-seconds 20`. To inspect one Ear only, add `--node-id SIM-EAR-001`. The script fetches at most the latest 500 observations per poll.

### Administer Hub nodes

The authenticated admin CLI lists and inspects nodes, updates desired state or installation metadata, and reversibly deactivates/reactivates a node without deleting its observations or MQTT account. While deactivated, the Hub rejects new observations and refuses provisioning, and the node's MQTT role ACLs are suspended; reactivation restores the role ACLs without changing the account or password. A broker ACL failure returns HTTP 503. If reactivation reports that Hub state is active while broker access remains suspended, retry the `reactivate` command after fixing broker access.

Load the admin token from the private `.env` file, then use `scripts/hub_admin.py`:

```bash
set -a
. ./.env
set +a

uv run --project hub/bootstrap python scripts/hub_admin.py \
  --hub-host "$DBMAP_LAN_IP" list
uv run --project hub/bootstrap python scripts/hub_admin.py \
  --hub-host "$DBMAP_LAN_IP" show SIM-EAR-001
uv run --project hub/bootstrap python scripts/hub_admin.py \
  --hub-host "$DBMAP_LAN_IP" set-desired SIM-EAR-001 --config-version 2
uv run --project hub/bootstrap python scripts/hub_admin.py \
  --hub-host "$DBMAP_LAN_IP" set-installation SIM-EAR-001 \
  --latitude 59.91 --longitude 10.75 --orientation-deg 180
uv run --project hub/bootstrap python scripts/hub_admin.py \
  --hub-host "$DBMAP_LAN_IP" deactivate SIM-EAR-001
uv run --project hub/bootstrap python scripts/hub_admin.py \
  --hub-host "$DBMAP_LAN_IP" reactivate SIM-EAR-001
```

Installation updates preserve fields not supplied on the command line; latitude and longitude must already exist or be supplied together. Desired-state updates likewise preserve fields that are not explicitly changed. Both commands validate the resulting complete object through the Hub API. Keep `.env` and the local CA private.

To send an Output command from the hub, use the authenticated TLS broker account:

Replace `out-xxx` with the actual node ID from the API response, written in lowercase.

```bash
mosquitto_pub --cafile mqtt/certs/ca.crt -h "$DBMAP_LAN_IP" -p 8883 \
  -u hub-bootstrap -P "$DBMAP_MQTT_HUB_PASSWORD" \
  -t esp-output/local/out-xxx/command -q 1 \
  -m '{"action":"RELAY_1_ON","duration_s":5}'
```

The relay remains a firmware stub. This verifies command delivery, not physical switching.

## Moving from the earlier lab setup

The earlier firmware used HTTP provisioning and a shared plaintext MQTT login. It cannot safely migrate those settings in place. Create a fresh Output node/token, copy the new CA certificate, then erase and reflash the board so it performs HTTPS provisioning and receives its own MQTT credentials:

```bash
idf.py erase-flash
```

Run the normal build/flash steps with the new token in menuconfig. Existing node records remain in the hub database; the reflashed board will register under the new node ID.

This baseline secures network transport and broker authorization on the trusted LAN. ESP32 flash encryption, secure boot, mutual TLS, and certificate rotation remain later work.

## Layout

```text
hub/bootstrap                         Local Hub identity + desired/reported state
mqtt                                  Mosquitto config, Dynamic Security, local TLS
firmware/components/node_common      Shared Ear/Output client
firmware/esp-output                  First L0 firmware
scripts                               Local Hub summary, admin, and observation exploration tools
tests                                 Tests for repository scripts and simulator
compose.yaml                         LOCAL_MEDIUM stack
```

Not in this slice: `hub-event`, rules, Ear DSP, Home Assistant, Regional Hub.

## Tests

```bash
cd hub/bootstrap
uv sync --extra dev
uv run ruff check app
uv run pytest tests ../../tests
```

The hub container currently uses Python 3.12, the runtime used by the deployed local hub and the verified test environment. Although the package metadata allows Python 3.11 and later, a test run on Python 3.14.4 currently fails during SQLAlchemy 2.0.36 ORM model initialization, before test collection completes. Keep the container on 3.12 until the ORM dependency is upgraded and the full suite and container build pass on a newer runtime.
