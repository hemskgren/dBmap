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

The broker is TLS-only on port `8883`; the bootstrap API is HTTPS on port `8443`. Each provisioned node gets a unique MQTT login and topic-scoped permissions. Hub services use a separate broker identity. The local CA is trusted by firmware and clients; keep its private key and `.env` secrets private.

See the [security policy](SECURITY.md) for private vulnerability reporting and secret-handling guidance.

## Mini-PC bring-up

Do this on the Intel host. Choose its stable LAN IPv4 address (a DHCP reservation is recommended). `hub.local` is the hostname that devices and clients use; it resolves to this address through mDNS. The numeric address is still needed by Docker to bind the published ports specifically to the LAN interface, and is included in the TLS certificate for clients that connect by IP. It is not a replacement for `hub.local`.

### If `.env` already exists

Do **not** run `init-local-secrets.sh`; it creates `.env` only when the file does not exist and deliberately refuses to overwrite an existing one. Edit the existing `.env` and make sure it contains:

```dotenv
DBMAP_LAN_IP=<mini-PC-LAN-IP>
DBMAP_ADMIN_TOKEN=<random-secret-1>
DBMAP_MQTT_HUB_PASSWORD=<random-secret-2>
DBMAP_DYNSEC_ADMIN_PASSWORD=<random-secret-3>
DBMAP_ADVERTISED_MQTT_HOST=hub.local
DBMAP_ADVERTISED_MQTT_PORT=8883
```

Replace the angle-bracket values with real values (do not type the brackets). Set `DBMAP_LAN_IP` to the NUC's address. Generate a separate secret for each of the other three entries by running `openssl rand -hex 32` three times. In particular, do not keep the earlier `change-me-admin-token` value or reuse the old shared MQTT password. Keep `.env` private; it should have mode `0600`:

```bash
chmod 600 .env
```

### If `.env` does not exist

Create it once with the script; this generates the three secrets and sets restrictive file permissions:

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

The certificate generator creates a private local CA and a server certificate valid for `hub.local`, the broker's internal `mosquitto` name, and the NUC's LAN IP. If it reports that an existing CA is unsuitable, run `./scripts/gen-mqtt-certs.sh --rotate-ca` to replace it. CA rotation invalidates trust on previously flashed devices; copy the new CA and erase/reflash each device. If `hub.local` is not already advertised by your host/router, run this on the NUC host (not in Docker):

```bash
sudo apt-get install -y avahi-utils
sudo avahi-publish -a -R hub.local "$DBMAP_LAN_IP"
```

Keep the publisher running during tests; it stops advertising when the process exits. Compose binds the service ports to `DBMAP_LAN_IP` only. Restrict access to the trusted LAN with the router/host firewall as well.

For the first ESP32 test, use the hub's numeric LAN IP in the firmware. `hub.local` works for host tools via mDNS, but this firmware does not include an explicit mDNS resolver. The IP is already included in the generated server certificate. Set `DBMAP_ADVERTISED_MQTT_HOST` in `.env` to the same LAN IP as `DBMAP_LAN_IP` and apply the setting so the provisioning response gives that reachable address to the ESP32:

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
  "https://${DBMAP_LAN_IP}:8443/api/v1/nodes"
```

The bootstrap token is returned only once; the hub stores its hash, so it cannot be displayed again. Save the returned token privately before continuing. If it is lost, repeat the node-creation request to get a new node ID and token; the old pending node can be left unused. Do not post the token in chat or logs. Before building firmware, embed the local CA so the ESP32 can verify HTTPS and MQTT server certificates:

```bash
cp mqtt/certs/ca.crt firmware/esp-output/main/root_ca.pem
```

In firmware menuconfig, under **dBmap node**, set the Wi-Fi SSID and password, hub bootstrap base URL to `https://<mini-PC-LAN-IP>:8443` (actual IP, without angle brackets), the one-time bootstrap token returned by the node-creation request, and hardware revision to `OUTPUT-DEV-V1`. The token field must not be left empty on the board's first provisioning boot: NVS is empty after `erase-flash`, and the token is not saved there before provisioning. Do not use `localhost` on the board; it refers to the ESP32 itself. The ESP32-WROOM-32 test uses the `esp32` target and 4 MB flash, not the planned ESP32-S3/16 MB Ear target.

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
  "https://${DBMAP_LAN_IP}:8443/api/v1/nodes"
```

### Record installation metadata

Installation metadata is stored by the hub, not reported by ESP firmware. Each record must include the node's geographic position as WGS84 latitude and longitude in decimal degrees; other placement details are optional. Replace the example coordinates below with the actual installation coordinates:

```bash
curl -sS --cacert mqtt/certs/ca.crt \
  -H "Authorization: Bearer ${DBMAP_ADMIN_TOKEN}" \
  -H "Content-Type: application/json" \
  -X PUT \
  -d '{"latitude":59.91,"longitude":10.75,"floor":7,"height_m":21,"height_accuracy_m":2,"mount_type":"balcony","environment":"urban","orientation_deg":180}' \
  "https://${DBMAP_LAN_IP}:8443/api/v1/nodes/<node-id>/installation"
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
  "https://${DBMAP_LAN_IP}:8443/api/v1/observations?node_id=<node-id-from-simulator>"
```

Replace `<node-id-from-simulator>` with the `SIM-EAR-*` node ID printed by the simulator. The response contains the validated protocol envelope, observation and hub `received_time_utc`. Classification and bearing have separate confidence values. Bearing is relative to the node reference axis, not a compass direction; the hub does not yet convert it using installation orientation. `signal_level_dbfs` is digital full-scale, not dB SPL. Each observation also includes a UUID, sequence number, timezone-aware event timestamp, monotonic capture timestamp, and timing-quality and uncertainty fields. Repeating publication with the same observation ID is deduplicated. To retry a publish after an error without provisioning another test node, run the command again with `--credentials-file /tmp/dbmap-ear-simulator-credentials.json --reuse-credentials`. Keep that file private; delete it when the simulator credentials are no longer needed.

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

The single-observation default remains an idempotent retry using the saved observation ID. Multi-observation mode advances and persists its sequence number after each successful publish, so rerunning it starts new observations. The stable synthetic source ID is only a test correlation hint; it does not simulate or establish real acoustic identity.

### Run a time-ordered Ear scenario

For repeatable observation timing and multiple simulated Ears, use `simulator/ear/simulate_scenario.py`. The scenario file defines **only Ear observations**. It does not create Events or Tracks, and it does not assert that a simulator source hint is a real identity. Each observation has its own ID and per-Ear sequence number. Set `classification_confidence` on every observation and `bearing_confidence` when a `bearing_deg` is present; both values must be between 0 and 1 and are copied into that observation's protocol payload. `event_time_utc` and monotonic capture time follow the scenario's logical timeline; `received_time_utc` remains the Hub's actual receive time. `--time-scale` changes only wall-clock delays, so `0.1` runs a 15-second scenario gap in 1.5 seconds without compressing its event timestamps.

The example scenario has observations at t=0, 2, 4, 6, and 15 seconds, with no observation at t=9. It uses two aliases, `ear_a` and `ear_b`. First, create or reuse one provisioned simulator credentials file per alias. If you do not already have two, create them with distinct paths (each command registers/provisions an Ear and publishes one initial test observation):

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

This lets us exercise observation ingestion and inspect the existing simulator hint before developing Hub event/track logic. It does not change or require rebuilding the Hub container.

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

For an authenticated summary of nodes and the latest 500 observations, run:

```bash
uv run --project hub/bootstrap python scripts/hub_summary.py \
  --hub-host "$DBMAP_LAN_IP"
```

Add `--watch` to print a summary once and then report new observations and node-state changes every 15 seconds; use `--interval-seconds 30` to change the interval. Stop watch mode with `Ctrl+C`.

### Explore observation source hints

`scripts/hub_event_track.py` is a read-only exploration helper, not an event/track engine or API. It fetches node installation metadata as well as observations and prints each observed Ear's site coordinates, mount height, mount type, environment, and orientation when available. It prints Candidate Groups based only on the simulator's `classification.hints.simulated_source_id`; unhinted observations remain separate. That value is test metadata, not verified physical identity. Within a multi-Ear candidate, it reports pairwise installation distance. As an initial plausibility hint, `vehicle` and `animal` are treated as local ground sources; if their Ears are more than 500 m apart, one shared local source is marked less plausible. `aircraft` and `drone` are treated as airborne, for which distance alone does not rule out a shared source. This is only a provisional source-family mapping, not a conclusion about identity; unknown or mixed families remain neutral. The radius can be changed with `--local-source-radius-m`.

It also prints provisional Event-shaped time episodes. The default 15-second inactivity window closes an episode only after that much silence following an observation interval (`event_time_utc + duration_ms`). This is a time boundary, not evidence that observations inside the episode came from the same source; simultaneous sources can still be distinct. The episode is only printed, never persisted or used to control a node. The tool does not create Tracks or infer geographic source positions from bearing. Watch mode re-fetches observations and node metadata, then reprints the analysis when new observations arrive.

```bash
uv run --project hub/bootstrap python scripts/hub_event_track.py \
  --hub-host "$DBMAP_LAN_IP"
```

To monitor and recalculate the proposals every 15 seconds, add `--watch`; customize the polling interval with `--interval-seconds 30`. To experiment with a 20-second inactivity window or a 300 m local-source radius, add `--event-gap-seconds 20 --local-source-radius-m 300`. To inspect one Ear only, add `--node-id SIM-EAR-001`. The script fetches at most the latest 500 observations per poll.

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
uv run pytest tests ../../tests
```

The hub container currently uses Python 3.12, the runtime used by the deployed local hub and the verified test environment. Although the package metadata allows Python 3.11 and later, a test run on Python 3.14.4 currently fails during SQLAlchemy 2.0.36 ORM model initialization, before test collection completes. Keep the container on 3.12 until the ORM dependency is upgraded and the full suite and container build pass on a newer runtime.
