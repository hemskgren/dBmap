# Register and deploy a device

This guide covers the current manual flow for registering an Output node and flashing the ESP32-WROOM-32 test firmware. It assumes the local Hub is already running; follow [Local deployment](./local-deployment.md) first.

## Prerequisites

Before creating or flashing a device, verify the Hub is ready from the repository root:

```bash
docker compose ps
```

Expect all five services (`mosquitto`, `dbmap-web`, `opa`, `nginx`, and `hub-bootstrap`) to be running and report `healthy`. If any service is missing, stopped, or unhealthy, complete or troubleshoot [Local deployment](./local-deployment.md) before continuing. That guide also covers setting the Hub's advertised MQTT address, which must be reachable by the device.

## Create a node and save its bootstrap token

The current local web UI and `POST /api/v1/nodes` endpoint require the admin token to create a node. Node creation returns a node ID and a one-time bootstrap token. The Hub stores only a hash of that token, so copy it to a private password manager or other protected store immediately. Do not put it in chat, source control, or logs. Create a separate node and token for each new device.

Load the private settings into the current shell and create an Output node with the hardware revision the firmware reports:

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

The admin Devices view can also create a node, assign an active owner as part of creation, and display its token once. It currently uses `unknown` for hardware revision; use the API request above when the firmware needs a specific revision.

### If the token is lost or the board needs a clean redeployment

A bootstrap token cannot be recovered or reissued for its existing node. If the node is still pending, or if the board has been erased after the token was consumed, create a replacement node and save its new token before flashing. A consumed token cannot provision the old node again.

If the old device might still be operating, deactivate it using the admin Devices view or the `deactivate` action in `scripts/hub_admin.py` before relying on its replacement. Deactivation suspends its MQTT access and blocks provisioning while retaining its identity, observations, and history. Deactivate a pending node whose token was lost as well, rather than leaving it available for accidental use. See [Administer Hub nodes](./local-deployment.md#administer-hub-nodes) for CLI examples.

If a provisioned device still has its NVS identity and credentials, reflashing without erasing NVS should keep it as the same Hub node; a new registration is not needed. A full `erase-flash` clears that local state. When starting clean after an erase, create a replacement Hub node and token.

## Build and flash the current Output firmware

The ESPConnect tab opens the checked-in ESPConnect v1.1.23 static build at `https://hub.local:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/espconnect/`. Its MIT-licensed app can inspect USB board information in Chromium 89+ with a data-capable USB cable and browser permission. It does not receive the Hub token, prefill dBmap registration, or flash the dBmap firmware. Its upstream license and source release are recorded under `web/espconnect/`.

Install the local CA into the client trust store as described in [Local deployment](./local-deployment.md), then copy it into the firmware project so the ESP32 can verify Hub and MQTT certificates:

```bash
cp mqtt/certs/ca.crt firmware/esp-output/main/root_ca.pem
```

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

On Linux, if `/dev/ttyACM0` is owned by group `dialout`, add your user to that group once, then fully log out and back in. Verify serial access after reconnecting:

```bash
sudo usermod -aG dialout "$USER"
id -nG
test -r /dev/ttyACM0 && echo "serial port readable"
```

In `idf.py menuconfig`, open **dBmap node** and set each required value:

- **Wi-Fi SSID** — the network name the device will join.
- **Wi-Fi password** — the credential for that network.
- **Hub bootstrap base URL** — `https://<mini-PC-LAN-IP>:8443`, replacing the placeholder with the Hub's actual numeric LAN IP and using the configured HTTPS port if it differs.
- **One-time bootstrap token** — the token returned when you created this device's node. A token is required on the first provisioning boot and cannot be recovered after creation.
- **Hardware revision** — `OUTPUT-DEV-V1` for this ESP32-WROOM-32 test.

Do not use `localhost` in the Hub URL; it refers to the ESP32 itself. The web registration form collects the SSID and password locally, but does not send them to the Hub or inject them into this firmware build. Enter them in the firmware build configuration for this deployment.

Build and flash using the serial port present on your machine (`/dev/ttyACM0` or `/dev/ttyUSB0` are common). This preserves NVS and the identity of an already provisioned device:

```bash
. "$HOME/esp/esp-idf/export.sh"
cd firmware/esp-output
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Only run `idf.py -p /dev/ttyACM0 erase-flash` when intentionally clearing NVS (for example, to remove old lab credentials). If the board was already provisioned, create a replacement Hub node and save its new token before erasing, then use that token for the clean provisioning boot.

After flashing, expect Wi-Fi connection, a `provisioned as OUT-...` message, and a verified TLS MQTT connection. Press `Ctrl+]` to leave the serial monitor. To confirm the Hub has the node, load `.env` from the repository root and list nodes:

```bash
set -a
. ./.env
set +a
export CURL_CA_BUNDLE=mqtt/certs/ca.crt
curl -sS -H "Authorization: Bearer ${DBMAP_ADMIN_TOKEN}" \
  "https://${DBMAP_LAN_IP}:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/api/v1/nodes"
```

## Browser-based provisioning: current step and direction

**Step 1 is available in the Devices tab under Device registration.** The admin selects a USB serial port in a supported Chromium-based browser and presses the board's reset button when prompted. Compatible ESP-Output firmware reads the existing `identity/node_id` value from NVS at boot and emits only that ID on the serial console; Wi-Fi credentials, bootstrap tokens, and MQTT credentials are not printed or read by the browser. This requires rebuilding and flashing firmware containing the identity marker. Older firmware, unsupported boards, or a board that does not boot may not report an identity; in that case, the admin must explicitly confirm the device is new before registration. USB vendor/product IDs are shown only as serial-adapter metadata (for example, `0x1a86` commonly identifies a USB-to-UART bridge, not the ESP32).

If the board reports `UNPROVISIONED`, select an active owner and enter the Wi-Fi SSID (maximum 32 UTF-8 bytes) and password, then create the device. If it reports a node ID already present in this Hub, the page shows that registration and blocks duplicate creation by default. An intentional replacement requires a separate confirmation; deactivate the old Hub node first if it may still be operating. A node ID not present in this Hub is also shown and requires explicit replacement confirmation. Creation and owner assignment are one atomic Hub API request, protected by an idempotency key so a retry cannot create a second node. Creation and ownership remain admin-only because ordinary-user sign-in and owner-scoped authorization are not implemented.

The result shows the new node ID, assigned owner, SSID, and one-time bootstrap token. Save the token immediately; it cannot be recovered. If a request may have succeeded but its response was lost, a retry reports the existing node but cannot return its token. The Wi-Fi password remains in the browser form until success, is never sent to the Hub or included in the build handoff, and is cleared after successful registration. Enter it separately in firmware `menuconfig` for this deployment.

**Step 2 is future work.** The page displays a redacted, allowlisted build-handoff preview containing the registered device, owner, Hub ID, SSID, whether a password was supplied, previous node ID if one was read, and generic USB IDs. It excludes Wi-Fi credentials, the bootstrap token, and Hub access token. It does not build, inject configuration, or flash firmware. Future work can add a staged build timeline and browser flashing after a secure device-secret provisioning mechanism is designed. ESPConnect remains a separate power-user tool exposed in the web UI to local admin/viewer accounts.

Device blocking/deactivation remains an admin lifecycle action and is not part of the registration form. There is no supported way in this repository to inject menuconfig values into an already-built image. Supplying device-specific values at build time means producing a per-device build and artifact. A future workflow should avoid embedding Wi-Fi passwords or bootstrap tokens in a reusable image; a post-flash provisioning mechanism that writes secrets to the device needs to be designed before the browser can safely build and flash devices.
