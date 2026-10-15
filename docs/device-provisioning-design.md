# Browser device provisioning design

This document defines the next deployment capability after the current local-admin browser flashing flow: send device-specific configuration to the ESP over USB, let its firmware persist that configuration in NVS, then verify that it has connected to the Local Hub.

The existing admin and viewer experience remains the current local workflow. This design does not add ordinary-user sign-in or owner-scoped deployment. It also does not make ESPConnect part of device registration or deployment; ESPConnect remains an optional diagnostic tool for local users.

## Current device capabilities

The ESP-Output firmware already:

- Loads identity, network settings, and credentials from NVS.
- Stores the dBmap node ID, Wi-Fi settings, Hub URL, MQTT endpoint and credentials, and provisioning state in NVS.
- Uses a one-time bootstrap token to call `POST /api/v1/provision` over HTTPS on first boot.
- Persists the returned device identity and MQTT credentials, then connects to the Hub.
- Prints `DBMAP_DEVICE_ID=...` on its serial console for USB identity inspection.

The current browser flow does not send the Wi-Fi settings or bootstrap token to the device. Registration displays the one-time token, and the manual firmware workflow compiles the token and Wi-Fi settings into a device-specific image. The reusable catalog image and browser flash flow must remain free of device-specific credentials.

## Recommended provisioning boundary

Reuse existing, maintained open-source components wherever they already solve a part of the workflow. Keep the device-specific provisioning contract in dBmap firmware and the Devices UI: the browser sends configuration to the running dBmap firmware over the user-selected USB serial connection. The firmware validates and stores it in NVS, restarts, then performs its existing HTTPS bootstrap with the Hub. The browser must not write raw NVS pages, generate a raw NVS partition image, or receive MQTT credentials.

This keeps the responsibilities clear:

1. The Hub registers the logical device and creates its one-time bootstrap token.
2. The browser transports the Wi-Fi settings, Hub endpoint, and bootstrap token over the selected USB connection.
3. The device validates and stores the configuration in NVS.
4. The device redeems the one-time token with the Hub over HTTPS and stores the returned identity and MQTT credentials.
5. The Hub observes the device's authenticated MQTT connection and initial status before showing deployment as complete.

USB access is an explicit local physical action. The browser must use the dBmap protocol, request serial permission from the user, and never transmit the local Hub API bearer token to the device. Credentials must not appear in serial logs, browser logs, Hub API logs, deployment history, or ordinary device details.

## Serial protocol requirements

The first implementation uses newline-framed JSON at 115200 baud. The firmware emits `DBMAP_PROVISION_READY` while it waits, then accepts a `protocol: 1`, `operation: configure` request containing a request ID and the SSID, Wi-Fi password, HTTPS Hub URL, and bootstrap token. It replies with a matching `DBMAP_PROVISION_RESULT:` JSON line and a generic error code. The browser keeps the serial port open through the result.

The protocol must:

- Have bounded frame and field sizes, reject malformed or unsupported versions, and reject unknown operations.
- Validate SSID and password lengths and require an HTTPS Hub provisioning URL.
- Accept configuration only while the device is in an explicit provisioning state. Reconfiguration of an already provisioned device needs a separately authorized recovery flow.
- Return a request-matched result that identifies validation, NVS write, bootstrap, and connection failures without echoing secret values.
- Keep ordinary boot logs separate from protocol responses so the browser can parse both safely.
- Be interruptible and recoverable if USB disconnects or the device loses power.

The existing `DBMAP_DEVICE_ID` line remains useful for diagnostics and recognition, but it is not proof of physical identity. USB-to-serial vendor and product IDs identify the adapter, not the ESP board. Chip family can be checked during flashing; board revision still requires reliable board metadata or explicit local confirmation.

## NVS write and recovery requirements

The firmware must treat provisioning as a transaction. Validate the complete request before changing active configuration. Persist a versioned pending configuration, commit it successfully, and only then mark it active. On boot, recover deterministically from an interrupted update. A failed write must leave the prior usable configuration intact where one exists.

For a new device, do not mark the device deployed just because the USB write succeeded. The device should redeem the one-time bootstrap token, save the Hub-issued credentials, connect, and publish its normal hello/status. The Hub remains the authority for whether deployment is verified. The current serial configuration writes the identity and network namespaces with separate NVS commits; an interrupted write is retryable while the node remains pending, but this is not yet a fully atomic cross-namespace transaction.

For an existing device receiving a firmware upgrade, preserve identity, Wi-Fi settings, and Hub credentials by default. Do not ask ESP Web Tools to erase data as part of a routine upgrade. A lost or consumed bootstrap token must not silently create a duplicate registration; recovery needs an explicit admin operation that issues a replacement bootstrap credential or another device-authenticated reconfiguration mechanism.

## UI flow and state

When integrated into the Devices tab, expose a guided operation while retaining clear retry points:

1. Connect and inspect USB. Report adapter identifiers as diagnostic metadata and identify the chip during the supported flashing connection.
2. Register a new device or recognize an existing dBmap identity. Assign its owner using the current local-admin workflow.
3. Select a registered firmware release that matches the selected device record and detected chip family.
4. Flash without erasing NVS for upgrades. Keep the existing explicit admin confirmation where hardware compatibility remains unverified.
5. Send configuration over the dBmap serial protocol when the device is new or explicitly being reconfigured.
6. Wait for Hub-side confirmation of the expected node ID and authenticated connection before showing deployment as complete.

Keep firmware release state, device registration/ownership, flash attempts, and provisioning outcomes as separate records or stages. A flash failure must leave the device registration and owner intact. A provisioning failure should allow retrying configuration for the same node. Do not report a complete deployment based only on a browser's claim that a flash or serial write completed.

## Token lifecycle

The current Hub stores only a hash of the one-time bootstrap token and cannot recover the plaintext token after registration. A browser provisioning flow must therefore handle interruption after registration safely. Before relying on the flow, add an admin-only recovery operation that can revoke an unused bootstrap token and issue a replacement, while ensuring that already-consumed credentials cannot provision a second device. The UI should keep the token only in memory for the active operation and clear it after the device confirms receipt; it should never place credentials in local storage, session storage, URLs, or attempt history.

## ESPConnect boundary and license

ESPConnect v1.1.23 remains a separate, optional local troubleshooting page. Its locally included release identifies the upstream project and MIT license in `web/espconnect/UPSTREAM.txt`, and its MIT notice is retained in `web/espconnect/LICENSE.txt`. MIT permits reuse and modification when the required copyright and license notice is preserved.

Prefer reusing the existing ESP Web Tools integration for browser flashing. The vendored 10.4.0 package is Apache-2.0 licensed, pinned, and already used by the Firmware tab; keep its license and review bundled dependency notices when updating it. Reuse its supported manifest/flashing behavior rather than implementing an ESP ROM flasher in dBmap.

ESPConnect can be a source of reusable USB/chip-identification or diagnostic code if its actual source and dependencies fit the need. The packaged release in this repo is a separate troubleshooting UI; importing all of it into the deployment path would couple those concerns unnecessarily. Retain the MIT notice for any adapted code and check the licenses of bundled dependencies. The upstream project describes its NVS inspector as experimental and read-only, so use it to inspect devices or learn from its parser, not as a writer for provisioning. For NVS changes, the dBmap firmware should use ESP-IDF's NVS API behind its own validated, versioned serial protocol.

## Current implementation boundary

The initial USB setup implementation covers the pending-device path only. It persists browser-supplied network settings and token, retries configuration after Wi-Fi failure, and waits for Hub state `active` plus `online`. It does not provide an in-place recovery flow for an already-provisioned device or for the narrow failure window where the Hub consumes a bootstrap token but the device cannot save the returned MQTT credentials. A firmware build and physical USB/NVS test are still required before treating the serial contract as hardware-verified.

In short, reuse the mature open-source flasher and suitable USB helpers; keep dBmap registration, secret handling, provisioning authorization, and NVS writes in dBmap's own flow.

## Acceptance criteria

- A new compatible device can be registered and assigned an owner, flashed with a reusable release, configured over USB, and verified online without compiling device credentials into the firmware image.
- Wi-Fi passwords and bootstrap tokens do not reach Hub storage or logs and do not appear in deployment history.
- A firmware upgrade preserves active NVS configuration unless an administrator explicitly chooses a documented recovery/reset action.
- USB disconnect, malformed requests, failed NVS commits, invalid Wi-Fi credentials, and failed Hub bootstrap each leave a clear recoverable state.
- The Hub marks the device deployed only after it observes the expected device identity and authenticated connection.
- ESPConnect remains optional and its diagnostic actions do not mutate dBmap registration, ownership, firmware approval, or deployment state.
