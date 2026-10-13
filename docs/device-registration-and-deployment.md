# Device registration and firmware

This guide separates two different tasks:

1. Build and flash firmware directly to a device for a one-off local test.
2. Upload a reusable firmware ZIP to the Hub catalog for review and later use.

These are not currently one continuous browser deployment flow. The Firmware tab manages ZIP files in the catalog; it does not build or flash firmware. Browser-based flashing and post-flash provisioning are not implemented. For setting up the Hub itself, first follow [Local deployment](./local-deployment.md).

## How often do I do each part?

This guide applies to the first device and later devices. Reuse the same registered firmware for multiple devices when it is a reusable, compatible image; device registration and the current manual credential-in-firmware workflow are per physical device.

| Work | How often |
| --- | --- |
| Install ESP-IDF and Linux build prerequisites | Once per build computer. |
| Set up the ESP-IDF project for a hardware target | Once per hardware target on that build computer. |
| Build a new reusable, credential-free ZIP | Once per firmware release for that target. The same ZIP can be reused for many devices, subject to hardware compatibility. |
| Generate the ZIP's metadata, upload it, and promote it | Once per firmware ZIP/catalog entry. Do not upload a new copy for every device. |
| Create a Hub node and save its one-time bootstrap token | Once per physical device that needs a new Hub identity. |
| Enter Wi-Fi credentials and that device's bootstrap token in `menuconfig`, then build and flash using the manual flow in section 5 | For every device in this current manual workflow, because those device-specific values are compiled into the image. |
| Add the Linux user to `dialout` | Once per Linux user/computer, if needed for serial access. |

The reusable catalog image and the manual per-device image are intentionally different: the catalog image should not contain a device's Wi-Fi password or bootstrap token; the current manual test does. The catalog can hold a single firmware release for device 1 through device 100, but this release does not yet implement browser flashing or compatibility-based deployment.

## Contents

- [1. Check prerequisites](#1-check-prerequisites)
- [2. Understand the build outputs](#2-understand-the-build-outputs)
- [3. Upload and review a firmware catalog candidate](#3-upload-and-review-a-firmware-catalog-candidate)
  - [3.1. Upload with the web UI](#31-upload-with-the-web-ui)
  - [3.2. Upload with the CLI](#32-upload-with-the-cli)
  - [3.3. Review and promote](#33-review-and-promote)
- [4. Register the physical device](#4-register-the-physical-device)
  - [4.1. Register in the Devices tab](#41-register-in-the-devices-tab)
  - [4.2. Save the bootstrap token](#42-save-the-bootstrap-token)
  - [4.3. If the token is lost or the device is erased](#43-if-the-token-is-lost-or-the-device-is-erased)
- [5. Manually build, flash, and verify a one-off device](#5-manually-build-flash-and-verify-a-one-off-device)
  - [5.1. Install ESP-IDF](#51-install-esp-idf)
  - [5.2. Configure and build for this device](#52-configure-and-build-for-this-device)
  - [5.3. Flash and verify](#53-flash-and-verify)
- [6. Current limits and future deployment work](#6-current-limits-and-future-deployment-work)

## 1. Check prerequisites

From the repository root, confirm the normal Hub services are running and healthy:

```bash
docker compose ps
```

Expect `mosquitto`, `dbmap-web`, `opa`, `nginx`, and `hub-bootstrap` to be healthy. The optional ESP-IDF builder is not required for normal Hub operation. If a required service is missing or unhealthy, complete or troubleshoot [Local deployment](./local-deployment.md) first. That guide also covers the Hub's advertised MQTT address, which must be reachable by the device.

## 2. Understand the build outputs

Choose the path that matches what you want to do:

| Goal | Input/output | What it does |
| --- | --- | --- |
| Test firmware directly on a connected device | `idf.py build` creates separate `.bin` files under `firmware/esp-output/build/` | The later `idf.py flash` command writes those files to the device. It does **not** create a ZIP or add anything to the catalog. |
| Add reusable firmware to the Hub catalog | A validated `<firmware_id>.zip` plus `<firmware_id>.json` sidecar | Uploads the pair as an unregistered candidate. An admin must review and promote it before it is registered. Viewers can see and download candidates but cannot change them. This does **not** flash a device. |

The manual ESP-IDF build outputs separate components, including `build/bootloader/bootloader.bin`, `build/partition_table/partition-table.bin`, and `build/esp-output.bin`. Those raw build files are not accepted by the catalog uploader. The current catalog validator supports this ESP-Output layout: exactly those three components plus `manifest.json`, with the expected flash offsets, component hashes, and `nvs_preserved: true`.

The retained `firmware-builder` can produce a ZIP named `firmware.zip` at `/artifacts/<build-id>/firmware.zip` inside its Docker volume (`firmware-artifacts`). That file is inside Docker storage, not automatically in the repository or on your computer. `<build-id>` is generated for each build; do not type the placeholder literally. First list successful builds with an artifact in the persistent volume:

```bash
docker compose --profile firmware-build run --rm --no-deps \
  --entrypoint python firmware-builder -c '
import json
from pathlib import Path

root = Path("/artifacts")
jobs = root / ".jobs"
found = False
for job_file in sorted(jobs.glob("*.json"), key=lambda path: path.stat().st_mtime, reverse=True):
    job = json.loads(job_file.read_text())
    build_id = job.get("id", job_file.stem)
    bundle = root / build_id / "firmware.zip"
    if job.get("status") == "succeeded" and bundle.is_file():
        print("{}  {}  {} bytes".format(
            build_id, job.get("target", "unknown"), bundle.stat().st_size
        ))
        found = True
if not found:
    print("No successful firmware ZIP artifacts found in the firmware-artifacts volume.")
'
```

This temporary command runs only the builder image's Python interpreter against the existing artifact volume; it does not start a firmware build. The `--profile firmware-build` option is required because the builder is opt-in. Pick the build ID from the output, then copy that ZIP from the stopped builder service container:

```bash
mkdir -p artifacts
BUILD_ID="paste-the-build-id-from-the-list"
FIRMWARE_ID="choose-a-catalog-firmware-id"
docker compose cp \
  "firmware-builder:/artifacts/${BUILD_ID}/firmware.zip" \
  "artifacts/${FIRMWARE_ID}.zip"
```

Use the actual ID printed by the list command, not the example text. The copied file is now `artifacts/<firmware_id>.zip`; use that path for generating its metadata sidecar and uploading it. Listing and copying only access artifacts already present in the persistent volume; neither command starts a build. If you already have a ZIP from another build/export process, use that ZIP instead. Do not point the catalog upload at the raw ESP-IDF build directory.

## 3. Upload and review a firmware catalog candidate

You need an ID-matched pair: `<firmware_id>.zip` and `<firmware_id>.json`. The JSON records the actual build date, target, node type, platform, hardware revision, firmware and ESP-IDF versions, configuration version, and ZIP SHA-256 digest. The server also validates the metadata against the ZIP manifest and component hashes.

The helper can generate the sidecar from a valid ZIP manifest. It does **not** build the ZIP. Run these commands from the repository root, replacing the example ID, directory, and timestamp with the real values:

```bash
FIRMWARE_ID="replace-with-a-firmware-id"
BUILD_DATE_UTC="2026-10-13T08:00:00Z" # Use the actual build time and timezone.
ZIP="path/to/${FIRMWARE_ID}.zip"

uv run python scripts/hub_firmware.py metadata \
  "$ZIP" --build-date-utc "$BUILD_DATE_UTC"
```

This creates `path/to/<firmware_id>.json` beside the ZIP. The build timestamp must be when the image was built, not when it is uploaded.

### 3.1. Upload with the web UI

Sign in as a local admin, open **Firmware**, and select the matching ZIP and JSON files. The web form performs the upload and validation; it does not create the JSON sidecar for you, so run the `metadata` command above first if needed.

### 3.2. Upload with the CLI

The script reads Hub settings from the project-root `.env` file automatically. Upload the same ZIP/JSON pair with:

```bash
uv run python scripts/hub_firmware.py \
  upload "$ZIP" "path/to/${FIRMWARE_ID}.json"
```

The helper loads `DBMAP_LAN_IP`, `DBMAP_ADMIN_TOKEN`, `DBMAP_HTTPS_PORT`, and `DBMAP_PUBLIC_BASE_PATH` from the project-root `.env` automatically; values already exported in the shell take precedence. The port defaults to `8443`, and the CA defaults to `mqtt/certs/ca.crt`. You can omit connection options for both listing and upload. `--hub-host`, `--hub-port`, `--base-path`, and `--ca` remain available as overrides. Run `uv run python scripts/hub_firmware.py` with no arguments to display help. Listing includes candidates. Keep `.env`, Wi-Fi passwords, bootstrap tokens, and firmware credentials private.

List catalog entries without specifying the Hub host:

```bash
uv run python scripts/hub_firmware.py list
```

### 3.3. Review and promote

The uploaded item is an **unregistered candidate**, not yet an available catalog image. In the Firmware tab, inspect its target and metadata. Promote it only after verifying it is the expected build. Admins can see candidates and manage entries; viewers can see and download every entry, including candidates, without changing catalog state.

This upload and promotion is done once for that firmware ZIP, not once per device. The current catalog does not filter firmware by device compatibility. It is an admin-managed store, not yet a regular-user deployment selector.

## 4. Register the physical device

For the current manual one-off deployment in section 5, create a Hub node and save its one-time bootstrap token **before** configuring and flashing the firmware. The node/token is not needed just to upload a reusable firmware ZIP to the catalog.

### 4.1. Register in the Devices tab

The local admin can open **Devices → Device registration**, inspect the connected board over USB, choose an active owner, enter Wi-Fi details, and create the node. Compatible ESP-Output firmware can report its existing dBmap node ID from NVS. USB vendor/product IDs describe the USB serial adapter, not necessarily the ESP32 chip.

Alternatively, load `.env` and create an Output node using the API:

```bash
set -a
. ./.env
set +a
export CURL_CA_BUNDLE=mqtt/certs/ca.crt
curl -sS -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"node_type":"output","hardware_revision":"OUTPUT-DEV-V1"}' \
  "https://${DBMAP_LAN_IP}:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/api/v1/nodes"
```

The API response contains the new node ID and bootstrap token. The web registration flow can also assign the owner as part of node creation; the API example above creates an unassigned node.

### 4.2. Save the bootstrap token

The Hub stores only a hash of the token and cannot show it again. Save it immediately in a private password manager or other protected store. Do not put it in chat, source control, or logs. Do not reuse one device's token for another device.

### 4.3. If the token is lost or the device is erased

A bootstrap token cannot be recovered or reissued for its existing node. If a pending device's token is lost, or a provisioned board is fully erased, create a replacement node and save its new token before provisioning it again.

If the old device may still be operating, deactivate its Hub node before relying on a replacement. Deactivation suspends MQTT access while retaining its identity, observations, and history. See [Administer Hub nodes](./local-deployment.md#administer-hub-nodes).

If the device still has its NVS identity and credentials, reflashing without erasing NVS should preserve the same Hub node; a new registration is not needed. `erase-flash` clears that local state.

## 5. Manually build, flash, and verify a one-off device

This is the existing direct-to-device test flow. It builds firmware on your computer and flashes it over USB; it does not create a catalog ZIP. Complete section 4 first so you have the node's bootstrap token. The Wi-Fi credentials and token are compiled into this one-off firmware, so treat the resulting build files as sensitive.

ESPConnect is a separate power-user tool at `https://hub.local:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/espconnect/`. It can inspect USB board information in Chromium with a data-capable cable and browser permission, but it does not receive the Hub token, register the dBmap device, or flash this firmware. Its upstream license and source release are recorded under `web/espconnect/`.

### 5.1. Install ESP-IDF

`idf.py` comes from Espressif's ESP-IDF toolchain; it is not a Python project dependency. Install the Linux prerequisites and ESP-IDF v5.4.2 once per build computer. You do not need to repeat this setup for each device:

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

Activate ESP-IDF in each terminal used to build:

```bash
. "$HOME/esp/esp-idf/export.sh"
idf.py --version
```

Install `mqtt/certs/ca.crt` in the client trust store as described in [Local deployment](./local-deployment.md). Copy it into the firmware project so the device can verify Hub and MQTT certificates:

```bash
cp mqtt/certs/ca.crt firmware/esp-output/main/root_ca.pem
```

On Linux, if the serial device is owned by group `dialout`, add your user to that group once, then fully log out and back in:

```bash
sudo usermod -aG dialout "$USER"
id -nG
test -r /dev/ttyACM0 && echo "serial port readable"
```

### 5.2. Configure and build for this device

Repeat this configuration and build for each physical device when using this manual flow. The bootstrap token is unique to the device, and the values below are compiled into its image.

In `idf.py menuconfig`, open **dBmap node** and set:

- **Wi-Fi SSID** and **Wi-Fi password** for the network the device will join.
- **Hub bootstrap base URL** to `https://<mini-PC-LAN-IP>:8443`, replacing the placeholder with the Hub's actual numeric LAN IP and using the configured HTTPS port if it differs. Do not use `localhost`; that would refer to the device itself.
- **One-time bootstrap token** to the token saved in section 4.
- **Hardware revision** to `OUTPUT-DEV-V1` for this ESP32-WROOM-32 test.

The web registration form's Wi-Fi fields do not get sent to the Hub or injected into the firmware. Enter them here for this manual test. Never commit the generated `sdkconfig` or share the built firmware while it contains credentials.

Build from the repository root:

```bash
. "$HOME/esp/esp-idf/export.sh"
cd firmware/esp-output
idf.py set-target esp32
idf.py menuconfig
idf.py build
```

After success, the files to flash are in `firmware/esp-output/build/`: `bootloader/bootloader.bin`, `partition_table/partition-table.bin`, and `esp-output.bin`. These are separate `.bin` files, **not a ZIP**. `idf.py flash` reads the generated ESP-IDF flash manifest and writes the components at their configured offsets. It does not create a firmware-catalog entry.

### 5.3. Flash and verify

Connect the device and use its actual serial port (for example, `/dev/ttyACM0` or `/dev/ttyUSB0`):

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Flashing these components preserves NVS. Only run `idf.py -p /dev/ttyACM0 erase-flash` when intentionally clearing NVS; an erased device needs a newly created Hub node/token before it can provision again.

Expect Wi-Fi connection, a `provisioned as OUT-...` message, and a verified TLS MQTT connection. Press `Ctrl+]` to leave the serial monitor. To confirm the Hub knows about the node, from the repository root run:

```bash
set -a
. ./.env
set +a
export CURL_CA_BUNDLE=mqtt/certs/ca.crt
curl -sS -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" \
  "https://${DBMAP_LAN_IP}:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/api/v1/nodes"
```

## 6. Current limits and future deployment work

The Devices tab currently supports local-admin USB identity inspection and node registration. The Firmware tab supports upload, review, promotion, listing, and download of catalog ZIPs. These are separate workflows: catalog upload does not flash, and the manual `idf.py` flow does not upload to the catalog.

Browser flashing, secure device-specific post-flash provisioning, end-to-end deployment verification, and regular-user compatibility-filtered firmware selection are not implemented yet. The optional builder does not inject Wi-Fi or bootstrap credentials into reusable artifacts. For the planned implementation phases, see [Codex implementation instruction: Steps 4–6](./codex-device-deployment-steps-4-6.md).
