# Device registration and firmware

This guide separates two different tasks:

1. Build and flash firmware directly to a device for a one-off local test.
2. Upload a reusable firmware ZIP to the Hub catalog for review and later use.

The Hub supports two device paths: a manual per-device ESP-IDF build, and browser flashing of a previously built, registered catalog image. The browser flow does not build firmware or inject Wi-Fi credentials. For setting up the Hub itself, first follow [Local deployment](./local-deployment.md).

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

The reusable catalog image and the manual per-device image are intentionally different: the catalog image should not contain a device's Wi-Fi password or bootstrap token; the current manual test does. A registered release can be reused for compatible devices, but the current browser flow does not provision Wi-Fi or bootstrap credentials.

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
- [6. Browser flash a registered release](#6-browser-flash-a-registered-release)
- [7. Current limits and future deployment work](#7-current-limits-and-future-deployment-work)

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

The manual ESP-IDF build outputs separate components, including `build/bootloader/bootloader.bin`, `build/partition_table/partition-table.bin`, and `build/esp-output.bin`. Those raw build files are not accepted by the catalog uploader. The current Output target packages those three components plus `manifest.json`; each target defines its flash component map, and the builder and Hub catalog validate the component list, offsets, hashes, and `nvs_preserved: true` against the target's registered layout.

### 2.1. Build a reusable image from the Firmware tab

The local builder is optional and currently supports the `output-esp32` target (ESP32, generic hardware profile) with the pinned ESP-IDF v5.4.4 toolchain. The target list is data-driven; each target supplies its own project, chip, node type, hardware profile, toolchain, and flash component map. A future target such as `ear-esp32-s3` can be added with its own project and component map in both the builder and Hub target registries. The builder clears Wi-Fi and bootstrap-token settings before compilation, so the artifact contains no per-device credentials. A generic hardware profile can match a device whose board revision is unknown, while board-specific images can continue to require an exact revision. Build a release once, upload and approve it once, then reuse that catalog release across compatible devices; do not build once per device. It does not flash or configure a device. The browser controls and downloads artifacts; compilation runs in the local `firmware-builder` container.

Start the builder profile and recreate Nginx to attach it to the private builder network:

```bash
docker compose --profile firmware-build up -d firmware-builder nginx
```

Sign in to the Hub with the local admin token and open **Firmware → Build reusable firmware**. Select the supported target and start the build. The panel reports the builder's persisted stage and result. Each build log is collapsed by default; expand it to load the full output, including successful builds. While a build is running, use **Refresh log** to load newer output. Logs are retained privately with the build and deleted when its artifact is deleted. Builds started before full-log support do not have a retained full log. Successful builds offer **Download firmware ZIP** and **Download matching metadata JSON**; filenames share the ID format `dbmap-output-esp32-<version>-<8 SHA-256 hex characters>`, where the suffix is derived from the exact ZIP artifact. The JSON sidecar uses the same ID and contains the ZIP checksum and build metadata. Select both files in the catalog upload form. Failed builds can be removed from history. Build artifacts remain in the `firmware-artifacts` Docker volume until deleted. The same build controls are unavailable to viewers, and the builder independently checks the bearer token.

Upload the matching pair as an unregistered firmware candidate, then review and promote it before browser flashing. The current image is for the Output ESP32 target only; other board families are not supported by this builder path.

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

This upload and promotion is done once for that firmware ZIP, not once per device. Viewers can see candidates, but only registered releases are considered for normal compatibility selection. Administrators can also select an exact-compatible candidate in the Browser flashing panel to test it before promotion; it is clearly marked and requires explicit confirmation. Testing does not promote the candidate. A registered release can be returned to candidate status from its catalog row if a problem is found; the image and deployment history remain, and the action is blocked while a deployment attempt is active.

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
  -d '{"node_type":"output"}' \
  "https://${DBMAP_LAN_IP}:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/api/v1/nodes"
```

The API response contains the new node ID and bootstrap token. The web registration flow can also assign the owner as part of node creation; the API example above creates an unassigned node.

### 4.2. Save the bootstrap token

The Hub stores only a hash of the token and cannot show it again. Save it immediately in a private password manager or other protected store. Do not put it in chat, source control, or logs. Do not reuse one device's token for another device. An administrator can issue a replacement for a node that is still `pending` from its Devices detail view; this invalidates any earlier unused token, and the replacement is shown only once in the current page.

### 4.3. If the token is lost or the device is erased

If a pending device's token is lost, an administrator can issue a replacement from the node's Devices detail view. The previous unused token is invalidated immediately. A token cannot be rotated after the node has been provisioned; this prevents an already consumed token from creating another provisioning session. A fully erased, already provisioned board needs a separate recovery procedure; do not create a duplicate node as a workaround.

If the old device may still be operating, deactivate its Hub node before relying on a replacement. Deactivation suspends MQTT access while retaining its identity, observations, and history. See [Administer Hub nodes](./local-deployment.md#administer-hub-nodes).

If the device still has its NVS identity and credentials, reflashing without erasing NVS should preserve the same Hub node; a new registration is not needed. `erase-flash` clears that local state.

### 4.4. Check firmware compatibility for a registered device

The Hub provides admin-only compatibility and selection-validation endpoints:

- `GET /api/v1/nodes/{node_id}/compatible-firmware` lists registered releases with a matching node type and either a generic hardware profile or an exact hardware revision. Candidates are excluded.
- `POST /api/v1/nodes/{node_id}/firmware-selection` validates a selected registered release again on the server. This does not start flashing or save deployment state.

The node record does not independently store a verified ESP chip platform. The API compares node type and hardware profile with the registered device record (not a live USB reading), and marks platform compatibility as `unknown`; an administrator must explicitly set `acknowledge_unverified_platform` to `true` in the selection-validation request. This is a documented acceptance of unverified chip compatibility, not proof that the connected chip matches. A release marked `generic` can be selected when the node's board revision is unknown; board-specific releases still require the recorded revision to match exactly. Viewer tokens cannot change the device record or access these device-specific endpoints.

### 4.5. Record deployment attempts

The Hub now records admin-started attempts against an existing node and registered firmware release:

- `POST /api/v1/nodes/{node_id}/deployment-attempts` starts an attempt only after the server rechecks release compatibility, requires explicit acknowledgment of the unverified platform, and verifies the stored ZIP checksum. Include the selected `firmware_id`, `acknowledge_unverified_platform: true`, and optionally the browser flasher version.
- `GET /api/v1/nodes/{node_id}/deployment-attempts` lists the device's recent attempts; `GET /api/v1/nodes/{node_id}/deployment-attempts/{attempt_id}` returns one attempt and its persisted progress events.
- `PATCH /api/v1/nodes/{node_id}/deployment-attempts/{attempt_id}` records an in-progress stage (`connecting`, `erasing`, `writing`, or `verifying`) and monotonically increasing percentage, or a terminal outcome (`succeeded`, `failed`, or `unknown`). Terminal failure codes are `device_not_connected`, `serial_connection_lost`, `flash_failed`, `cancelled`, `browser_interrupted`, `integrity_failed`, and `other`. The API rejects free-text diagnostics so credentials and raw serial logs are not stored.

Only one attempt can be in progress for a node. After a failed or unknown outcome, retry by starting another attempt for the same node; registration and ownership remain unchanged. A terminal result cannot be overwritten. If the browser disappears before it can report an outcome, the attempt remains `in_progress` until an administrator records `unknown`; the server does not yet infer a result from a lost browser connection. The attempt API is an audit/state layer only: it does not connect to USB, flash firmware, or independently prove that the browser's reported result is true. Web flashing and reporting real progress are still to be implemented. Viewer tokens cannot read or change attempts, and regular-user deployment authorization remains unavailable until ordinary-user sessions exist.

## 5. Manually build, flash, and verify a one-off device

This is the existing direct-to-device test flow. It builds firmware on your computer and flashes it over USB; it does not create a catalog ZIP. Complete section 4 first so you have the node's bootstrap token. The Wi-Fi credentials and token are compiled into this one-off firmware, so treat the resulting build files as sensitive.

ESPConnect is a separate power-user tool at `https://hub.local:${DBMAP_HTTPS_PORT:-8443}${DBMAP_PUBLIC_BASE_PATH:-}/espconnect/`. It can inspect USB board information in Chromium with a data-capable cable and browser permission, but it does not receive the Hub token, register the dBmap device, or flash this firmware. Its upstream license and source release are recorded under `web/espconnect/`.

### 5.1. Install ESP-IDF

`idf.py` comes from Espressif's ESP-IDF toolchain; it is not a Python project dependency. Install the Linux prerequisites and ESP-IDF v5.4.4 once per build computer. You do not need to repeat this setup for each device:

```bash
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

mkdir -p ~/esp
git clone --recursive --branch v5.4.4 \
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
- Leave **Hardware compatibility profile** at `generic` for this reusable ESP32 test image. Use a specific profile only when the board design has a verified revision that requires a distinct image.

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

## 6. Browser flash a registered release

Browser flashing is available to local administrators from the Firmware tab. It uses the locally hosted ESP Web Tools 10.4.0 installer; it does not send the Hub access token to a third-party flasher service.

1. To deploy a release for reuse, make sure it is promoted to **registered** status. An admin may instead select a marked **candidate** for pre-promotion hardware verification. Candidate flashing is limited to the administrator panel, requires a confirmation, and leaves the catalog status unchanged. A generic-profile release can be selected when a device's board revision is `unknown`; board-specific releases require a verified exact revision. Use a release intended for the device's node type and ESP chip family.
2. Open **Firmware** and select the active Hub device and compatible firmware release. The Hub cannot independently verify the physical chip platform, so confirm the board's ESP32 family and acknowledge the limitation.
3. Prefer **Verify USB identity and prepare flash**: select the board's serial port and let the Hub check whether it reports a matching `DBMAP_DEVICE_ID`. This check may require the app firmware to be running and is separate from ESP Web Tools' bootloader connection.
4. If the Hub cannot read the ID, an administrator can check the explicit override confirmation and choose **Continue without ID verification**. This skips only the dBmap node-ID check; it still validates the registered firmware and image hashes. The administrator must physically confirm the board is the selected Hub device. ESP Web Tools detects chip family and MAC address, not the dBmap node ID, so this override is less certain and is recorded in the attempt's flasher-version field.
5. Click **Open ESP Web Tools installer** and select the board's serial port in the ESP Web Tools chooser. Its dialog owns the serial connection and reports its own connection and flash progress.
6. Do **not** select erase-data in the ESP Web Tools prompt. Erasing clears NVS, including the dBmap identity and configuration. If the browser is interrupted or the outcome is unclear, choose **Outcome unknown**. The saved attempt history also lets an administrator resolve an attempt that remained in progress.
7. After ESP Web Tools reports completion, record success or failure in the Hub page. ESP Web Tools does not provide this page with a supported public completion/progress callback, so the result is an explicit administrator confirmation, not an automatic verification.

The registered image is flashed as-is. This browser flow does not inject Wi-Fi SSID/password, create a new bootstrap token, build firmware, or prove that the device subsequently connected to the Hub. It is therefore suitable only when the image's existing configuration is appropriate. If device-specific credentials need to be embedded, use the manual flow in section 5 and treat its build files as sensitive. Do not erase an existing device merely because its token was lost: create a replacement Hub registration/token and intentionally reconfigure the device instead.

## 7. Current limits and future deployment work

The Devices tab supports USB identity inspection and node registration. The Firmware tab supports upload, review, promotion, withdrawal to candidate status, listing, download, and administrator-only browser flashing of registered images or explicitly selected candidates for verification. The browser can check device identity before an attempt or proceed only after an explicit admin-confirmed identity override; it verifies downloaded image hashes and stores attempt history, but it cannot observe ESP Web Tools' internal progress or determine success automatically.

Secure device-specific post-flash provisioning, Wi-Fi credential injection, automatic end-to-end deployment verification, and regular-user compatibility-filtered deployment are not implemented. The optional builder does not inject Wi-Fi or bootstrap credentials into reusable artifacts. An administrator must confirm the chip family and manually record the final flashing result. The proposed serial-to-NVS provisioning boundary and recovery requirements are documented in [Browser device provisioning design](./device-provisioning-design.md).
