# Local Hub deployment

Run dBmap’s **local Hub** on an Intel mini-PC (`linux/amd64` Docker). Ears listen; Outputs can warn or switch in a place; the Hub is what those devices call home.

## Contents

1. [What it is for](#what-it-is-for)
2. [Working today / still to do](#working-today--still-to-do)
3. [Bring-up](#bring-up)
4. [Certificates and Compose](#certificates-and-compose)
5. [Host fail2ban](#host-fail2ban-https-api-and-mqtt)
6. [Using the Hub](#using-the-hub)
7. [Devices and firmware](#devices-and-firmware)
8. [Hub host tools](#hub-host-tools)
9. [Wipe Hub device data](#wipe-hub-device-data-destructive)
10. [Ear simulator](#ear-simulator)
11. [Layout and tests](#layout-and-tests)

Related: [Connectivity](./connectivity.md) · [Health and availability](./health-and-availability.md) · [Device registration](./device-registration-and-deployment.md) · [Wiring](./device-wiring-and-pinout.md) · [Protocol](./protocol.md) · [Hub API](./hub-api.openapi.yaml) · [Simulator](./simulator.md) · [SECURITY.md](../SECURITY.md)

## What it is for

| Role | Intent |
| --- | --- |
| **Hub** | Identity, config, telemetry, and a small web console for a site. Devices provision over HTTPS, then use MQTT over TLS. |
| **Ear** | Sense sound at a location and report observations (and health) to the Hub. |
| **Output** | Act in a **limited geographic area**: for example turn a relay on for *N* seconds, and/or play a short **info / warning** clip on a speaker so people nearby can hear it. |
| **Map / observations UI** | See where active devices sit and what recently arrived — not a finished event/track product yet. |

Hardware in this repo today: ESP32-based Ear and Output firmware (shared `node_common` client). Hub profile: mini-PC with Docker Compose — not a Raspberry Pi Hub image.

## Working today / still to do

| Area | Working | To do |
| --- | --- | --- |
| Hub stack | Compose: Mosquitto TLS, Nginx HTTPS, Hub API, web UI, OPA, SQLite | Regional Hub, planned maintenance windows |
| Security | Per-device MQTT logins, local CA, bearer admin/viewer tokens, optional host fail2ban | Device flash encryption / secure boot, mTLS, automated cert rotation |
| Ear | Wi-Fi, provision, MQTT hello/health/keepalive, mic path / reported levels, Hub observation ingest | On-device DSP/classification product path, cross-Ear event/track engine |
| Output | Provision, MQTT, relay on/off timed, HTTPS/file audio play/stop, Hub command API | Hub “rules” that choose Outputs by map area automatically; richer media library UX |
| Ops UI | Devices, observations, groups, firmware catalog, map popup, health/availability | Fuller product analytics |
| Simulator | Laptop `SIM-EAR-*` observation and scenario tools | Same as tracker/eval gaps above |

## Bring-up

On the Hub host, pick a stable LAN IPv4 (DHCP reservation recommended). `hub.local` is the usual name (mDNS); Docker still binds published ports to `DBMAP_LAN_IP`, and that IP is in the TLS certificate for IP clients.

### If `.env` already exists

Do **not** run `init-local-secrets.sh` (it will not overwrite). Ensure `.env` has at least:

```dotenv
DBMAP_LAN_IP=<mini-PC-LAN-IP>
DBMAP_HTTPS_PORT=8443
DBMAP_PUBLIC_BASE_PATH=
DBMAP_HUB_ID=local-01
DBMAP_ADMIN_TOKEN=<random-secret-1>
DBMAP_VIEWER_TOKEN=<random-secret-2>
DBMAP_MQTT_HUB_PASSWORD=<random-secret-3>
DBMAP_DYNSEC_ADMIN_PASSWORD=<random-secret-4>
DBMAP_PUBLIC_MQTT_HOST=hub.local
DBMAP_PUBLIC_MQTT_PORT=8883
DBMAP_LOCAL_MQTT_HOST=hub.local
DBMAP_LOCAL_MQTT_PORT=8883
DBMAP_PUBLIC_HUB_URL=
DBMAP_PUBLIC_HUB_HOST=
```

Generate each secret with `openssl rand -hex 32`. Keep admin and viewer tokens different. `chmod 600 .env`.

`DBMAP_HUB_ID` is the Hub id shown in the UI (default `local-01`). Change it in `.env` and recreate `hub-bootstrap` if you run several Hubs later.

### If `.env` does not exist

```bash
./scripts/init-local-secrets.sh <mini-PC-LAN-IP>
```

## Certificates and Compose

```bash
set -a
. ./.env
set +a
./scripts/gen-mqtt-certs.sh
docker compose up --build -d
```

Material lands under `shared/` (public trust at `shared/trust/ca.crt`; CA key only on the host). Each Compose service gets its own leaf under `shared/<service>/certs/`. Nginx publishes HTTPS on `DBMAP_HTTPS_PORT` (default `8443`); Mosquitto publishes `8883` on `DBMAP_LAN_IP`. Hub API, web, and OPA stay on the Compose network.

- Optional path prefix: `DBMAP_PUBLIC_BASE_PATH=/dbmap` (or empty for `/`).
- OPA is HTTPS-only on a private network; policy lives in `policy/` and reloads on change. Policy unit tests: `sh scripts/test-policy.sh`.
- Optional firmware builder: Compose profile `firmware-build` (not required for normal Hub use).
- Trust `shared/trust/ca.crt` in browsers/OS clients that talk to the Hub; CLI tools use `--cacert shared/trust/ca.crt`.
- Optional mDNS: `./scripts/publish-hub-mdns.sh "$DBMAP_LAN_IP"`.

Local vs external device call-home pairs come from `.env` (`DBMAP_LOCAL_*` / `DBMAP_PUBLIC_*`), not the browser URL. After changing hosts or certs: re-run `gen-mqtt-certs.sh`, rebuild firmware that embeds the CA, then `docker compose up -d`.

## Host fail2ban (HTTPS API and MQTT)

Optional host jails: after **10** bad API logins or MQTT auth failures in **10 minutes**, ban the IP for **1 hour** on **both** HTTPS and MQTT. Private LAN ranges are ignored. Install and operate with `sudo ./scripts/install-fail2ban-nginx.sh` (check/unban/stop commands are printed by the script). See also Nginx access log `shared/nginx/logs/access.log` and Mosquitto `shared/mosquitto/logs/mosquitto.log`.

API gateway failures return JSON `{"detail":"Hub temporarily unavailable"}`; the browser UI keeps a separate HTML page.

## Using the Hub

Open `https://hub.local:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/` with the admin or viewer token.

- **Hub** tab: mode, health/availability, component details/logs (and MQTT failed connections). Details: [Health and availability](./health-and-availability.md).
- **Devices** / **Groups** / **Observations** / firmware and USB tools: day-to-day site ops.
- **Map**: opens from the signed-in Hub window; needs installation coordinates on devices.

Viewer is read-mostly; admin can register devices and change lifecycle. The browser is not the security boundary — the API checks the token.

Installation coordinates (WGS84) are Hub-side metadata, not firmware-reported:

```bash
curl -sS --cacert shared/trust/ca.crt \
  -H "Authorization: Bearer ${DBMAP_ADMIN_TOKEN}" \
  -H "Content-Type: application/json" \
  -X PUT \
  -d '{"latitude":59.91,"longitude":10.75,"orientation_deg":180}' \
  "https://${DBMAP_LAN_IP}:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/api/v1/nodes/<node-id>/installation"
```

## Devices and firmware

Registration, USB configure, flash, and catalog flows: [Register and deploy a device](./device-registration-and-deployment.md). Pins: [Device wiring and pinout](./device-wiring-and-pinout.md).

**Output commands (implemented)** — prefer the Hub API (admin token). Intent: short local action (relay and/or speaker), not site-wide automation yet.

| Action | Purpose |
| --- | --- |
| `RELAY_1_ON` + `duration_s` | Energize relay for 1–3600 s |
| `RELAY_1_OFF` | Clear relay |
| `AUDIO_PLAY_HTTPS` + `url` | Play warning/info MP3 over HTTPS to the speaker amp |
| `AUDIO_PLAY_FILE` + `path` | Play a file from device LittleFS `/media` |
| `AUDIO_STOP` | Stop playback |

```bash
curl -sS --cacert shared/trust/ca.crt \
  -H "Authorization: Bearer ${DBMAP_ADMIN_TOKEN}" \
  -H "Content-Type: application/json" \
  -X POST \
  -d '{"action":"RELAY_1_ON","duration_s":5}' \
  "https://${DBMAP_LAN_IP}:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/api/v1/nodes/<node-id>/commands"
```

Desired firmware / catalog promote: use the Firmware tab or API (`firmware-selection` with a registered release). Candidate-only test flash needs `"allow_candidate":true`.

Older lab boards that used plaintext MQTT need erase + reflash onto the current HTTPS/TLS path (`idf.py erase-flash`, then normal flash with a new bootstrap token).

## Hub host tools

All `scripts/hub_*.py` tools read Hub address and `DBMAP_ADMIN_TOKEN` from `.env` (optional `--hub-host`, `--hub-port`, `--base-path`, `--ca`).

```bash
uv run python scripts/hub_summary.py list          # Hub / component snapshot
uv run python scripts/hub_admin.py list            # Nodes
uv run python scripts/hub_event_track.py list      # Read-only observation/site report (not a tracker)
uv run python scripts/ear_audio_compare.py list    # Ear mic levels (dBFS)
uv run python scripts/hub_observations_db.py prune # Drop old observation rows (SQLite; default >4h)
```

Regional weather (Open-Meteo) is Hub-owned and optional for ingest. Defaults use the SIM-Ear geographic centre; override with `DBMAP_WEATHER_REGION_LATITUDE` / `DBMAP_WEATHER_REGION_LONGITUDE`. Cache TTL defaults to 15 minutes (`DBMAP_WEATHER_CACHE_TTL_S`). Disable with `DBMAP_WEATHER_ENABLED=false`.

`hub_event_track.py watch` polls; it does not create Events/Tracks or group by simulator ground-truth hints. Health field meanings: [Health and availability](./health-and-availability.md).

## Wipe Hub device data (destructive)

Rare local reset. **`wipe-device-data` is not simulator-only** — it removes **every** registered node (`EAR-*`, `OUT-*`, `SIM-EAR-*`) and related rows from the Hub SQLite DB.

**Deletes:** observations, nodes, node credentials, installation metadata, desired/reported state, bootstrap tokens, registration/deployment rows, device ownership, device monitor/diag samples and events.

**Keeps:** local users/tokens in `.env`, firmware catalog, TLS material, Mosquitto DynSec (re-provision devices afterward), Hub monitor history tables that are not in the wipe list.

```bash
uv run python scripts/hub_observations_db.py wipe-device-data --yes
# If you used the Ear simulator:
rm -f shared/simulator/sim-ear-*.json
```

To clear **observations only** (keep nodes), use `clear` / `prune` — see [Ear simulator](./simulator.md#clear-observations-keep-sim-ears).

## Ear simulator

Laptop Ears under `simulator/ear/` (`SIM-EAR-*`). **How to run:** [Ear simulator](./simulator.md).
Start with Ear A:

```bash
./scripts/sim-ear-observation add
```

## Layout and tests

```text
hub/bootstrap          Local Hub API and monitor
mqtt                   Mosquitto + Dynamic Security
web                    Hub UI (+ /map)
firmware/              Ear / Output + shared client
firmware-builder/      Optional ESP-IDF builder profile
simulator/ear          Laptop Ear simulators
scripts/               Ops CLIs (`lib/` = shared helpers)
docs/                  This guide and related docs
compose.yaml           Local Hub stack
```

```bash
cd hub/bootstrap
uv sync --extra dev
uv run ruff check app
uv run pytest tests ../../tests
```

Hub image uses Python 3.14; package metadata still allows 3.11+.
