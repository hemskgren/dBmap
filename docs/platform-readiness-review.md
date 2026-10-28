# Platform readiness review (Output + Local Hub)

**Purpose:** Make the Local Hub and ESP-Output path solid so later sensor work can focus on hardware, calibration, and tracking — not on Hub, MQTT, identity, or deployment basics.

**Scope of this document:** the **whole git repository** on `main` (including merged PRs through **#16**), compared with the six instruction files in `.ruff_cache/`. This is not a PR-16-only diff.

**Out of scope here (do not reply on these in this file):** sensor-node DSP/firmware, microphone experiments, calibrated direction estimation, event/track engine, Acoustic Monitors, raw-audio capture, Home Assistant, Regional Hub federation. Those stay for later hardware-focused work.

**How to reply:** copy one item ID and one of:

```text
P-01  do
P-01  skip   (reason optional)
P-01  later
P-01  question: ...
```

Items marked **Doing in this branch** are already being implemented unless you say `skip`.

---

## Contents

1. [Sources](#1-sources)
2. [What the repo already does](#2-what-the-repo-already-does)
3. [Instruction files that pull in different directions](#3-instruction-files-that-pull-in-different-directions)
4. [Open questions](#4-open-questions)
5. [Action items](#5-action-items)
   - [A. Stale documentation](#a-stale-documentation)
   - [B. Output device loop (you have this hardware)](#b-output-device-loop-you-have-this-hardware)
   - [C. Deployment completeness](#c-deployment-completeness)
   - [D. Hub API / MQTT platform](#d-hub-api--mqtt-platform)
   - [E. Identity, policy, multi-user](#e-identity-policy-multi-user)
   - [F. Web / HTTPS / Compose](#f-web--https--compose)
   - [G. Intentionally not following (until you say otherwise)](#g-intentionally-not-following-until-you-say-otherwise)
6. [Suggested order](#6-suggested-order)

---

## 1. Sources

Instruction files (gitignored; line numbers refer to those files):

| ID | File | Role |
| --- | --- | --- |
| S1 | `.ruff_cache/dBmap_project_description_EN.md` | Project vision and architecture |
| S2 | `.ruff_cache/dBmap_web_architecture_ai_instruction.md` | Web/Hub architecture v2 |
| S3 | `.ruff_cache/dBmap_local_web_codex_instructions.md` | Users, identity, multi-user |
| S4 | `.ruff_cache/dBmap_device_deployment_phased_codex_instructions.md` | Deployment steps 1–3 (+ placeholder 4) |
| S5 | `.ruff_cache/dBmap_instructions.md` | Design notes for steps 4–6 |
| S6 | `.ruff_cache/dBmap_device_deployment_phased_codex_instructionst-steps-4-6.md` | Implementation of steps 4–6 |

In-repo docs used as ground truth of **current** behavior: `README.md`, `docs/protocol.md`, `docs/local-deployment.md`, `docs/device-registration-and-deployment.md`, `docs/device-provisioning-design.md`, `docs/identity-and-operating-modes.md`, `docs/authorization.md`.

---

## 2. What the repo already does

This is the actual platform, not the instruction-file wishlist.

| Area | Current code |
| --- | --- |
| Compose LOCAL_MEDIUM | Nginx HTTPS `:8443`, Hub TLS, Mosquitto TLS `:8883`, OPA private, optional `firmware-build` profile (`compose.yaml`) |
| Hub identity | One-time bootstrap token, `POST /api/v1/provision`, unique MQTT user + topic ACL |
| Desired vs reported | Stored separately; `pending_configuration_change` is computed; **config is not delivered** to devices |
| Output firmware | NVS, Wi-Fi, HTTPS provision, MQTT hello/health/command/ack, GPIO16 relay + GPIO2 LED (`firmware/esp-output/main/output_command.c`) |
| USB deploy | Devices tab: inspect USB, register, catalog flash via ESP Web Tools, serial NVS provision |
| Firmware catalog | Upload candidate → inspect → promote/withdraw; builder produces reusable ZIP without device secrets |
| Web UI | Hub / Devices (list + detail + installation) / Firmware / Users / Groups / Observations / ESPConnect |
| Auth | Local admin + viewer bearer tokens; OPA fail-closed; users/groups/ownership tables exist but **do not log in** |
| Observations | Versioned MQTT envelope persisted; simulator path exists; no Hub correlation |
| Output commands | Firmware accepts `RELAY_1_ON` / `RELAY_1_OFF`; operator must `mosquitto_pub` — **no Hub API/UI** |

---

## 3. Instruction files that pull in different directions

Reply on the **Q-** items in section 4. These are conflicts, not missing code.

| Conflict | Files | What the repo did |
| --- | --- | --- |
| Rebuild Devices UI vs freeze it | S2 §3 vs S3 §1 / §11 / §15 | Devices list + detail already exist; S3 “do not rebuild tabs” is historical |
| Step 4 is a JSON placeholder vs real builder/catalog | S4 §Step 4 vs S6 Step 4 | Catalog + optional builder + browser flash exist; Devices tab still shows a leftover “build handoff preview (not implemented)” |
| Browser must not send Wi-Fi to Hub vs provision via Hub | S4 Wi-Fi secrets vs S6 Step 6 | Wi-Fi stays in the browser; USB write to NVS; Hub never stores the password — this is the intended split |
| Deployment success = NVS write vs Hub-verified connection | S5 §7, S6 Step 6 vs current UI copy | UI **skips** Hub online check after NVS (`web/index.html` registration provision step) |
| Separate hub-ingest / hub-event / hub-rules services | S1 §§23–27 vs README “not in this slice” | Single `hub-bootstrap` process; service *boundaries* are not separate containers |
| OPA vs “do not introduce OPA” | S3 §7 vs current `policy/` | OPA is already in the stack; keep it |
| GitHub OAuth for ordinary users | S2 §5, S3 §§3–5 vs current tokens | Tables exist; no OAuth routes; local admin path works offline |

---

## 4. Open questions

### Q-01 — Treat Hub-verified online as “deployed”?

**Sources:** S5 L216–229; S6 L62–65; `docs/device-provisioning-design.md` L89–93; UI currently skips this (`web/index.html` ~L303, L415, L2538).

Should a successful USB NVS write be followed by polling the Hub until `provisioning_state=active` and `status=online`?

**Reply:** `Q-01 skip -is not practical in comunity project`

**This branch:** **skip** (per user reply: not practical in community project; keep USB NVS write as deployment completion, do not block or poll Hub online status; resolves P-10 as skip).

---

### Q-02 — Hub API for Output commands (instead of `mosquitto_pub`)?

**Sources:** S1 §10 / §21 / §28 `/api/outputs`; `docs/local-deployment.md` L313–324; `docs/protocol.md` L164–204.

Should admins send `RELAY_1_ON` / `OFF` from the device-detail page through the Hub API so CLI MQTT is not a platform prerequisite?

**Reply:** `Q-02 do` (user note: already present uncommitted in this branch)

**This branch:** **do** (Hub API implemented in working tree via `POST /api/v1/nodes/{node_id}/commands` in `hub/bootstrap/app/api.py`, `service.py`, `schemas.py`, `mqtt_listener.py`; UI integration tracked in P-07).

---

### Q-03 — Firmware must honor `expires_at_utc` before acting?

**Sources:** `docs/protocol.md` L182–231; firmware `output_command.c` now includes expiry handling.

**Reply:** `Q-03 do`

**This branch:** Hub may *send* `expires_at_utc`; firmware now validates the timestamp and will ignore expired commands.

---

### Q-04 — Local audio files / `PLAY_LOCAL` on Output?

**Sources:** S1 L736–753; firmware has GPIO only, no `/audio/` playback.

Needs a pin/file layout and storage plan. Not required for Hub readiness.

**Reply:** `Q-04 do` / `skip` / `later` / `question:`

---

### Q-05 — Deliver desired config over MQTT `config` topic?

**Sources:** S1 §8–9; `docs/protocol.md` L54, L265–272. ACL exists; no publisher; firmware does not apply revisions.

Output is push-capable; sensor nodes were specified as pull. Mixing those models needs an explicit rule.

**Reply:** `Q-05 do` / `skip` / `later` / `question:`

**This branch:** **later** (do not pretend desired state is applied).

---

### Q-06 — Ordinary-user GitHub login before sensor hardware?

**Sources:** S3; `docs/identity-and-operating-modes.md`. Local admin/viewer is enough to flash Output and later sensors.

**Reply:** `Q-06 do` / `skip` / `later` / `question:`

**This branch:** **later**.

---

### Q-07 — Alembic migrations instead of `create_all`?

**Sources:** S3; `docs/identity-and-operating-modes.md` L50.

**Reply:** `Q-07 do` / `skip` / `later` / `question:`

**This branch:** **later**.

---

### Q-08 — Split Hub into ingest/event/rules containers now?

**Sources:** S1 §§23–27.

**Reply:** `Q-08 do` / `skip` / `later` / `question:`

**This branch:** **skip** (keep one `hub-bootstrap`; clear module boundaries are enough until load requires a split).

---

## 5. Action items

Each item has **Status**, **Sources**, **Code**, and **Ask**.

### A. Stale documentation

Documentation that no longer matches the repo is a platform bug: it sends you down the wrong test path.

#### P-01 — README / local-deployment still call Output relay an unimplemented stub

**Status:** updated  
**Sources:** S1 §20–21 (actuators are first-class)  
**Code:** `README.md` L9; `docs/local-deployment.md` L17, L324; actual GPIO in `firmware/esp-output/main/output_command.c` L20–21, L155–167  
**Ask:** Update docs to GPIO16 relay + GPIO2 LED, bounded duration, ack ≠ physical proof.  
**This branch:** **do**  
**Reply:** `P-01`

---

#### P-02 — Registration guide §4.5 now correctly states browser flashing is implemented

**Status:** updated  
**Sources:** S6 Step 5; `docs/device-registration-and-deployment.md` L218 vs L309–327  
**Code:** Firmware tab + Devices Step 5 in `web/index.html`  
**Ask:** Rewrite §4.5 so it matches the live attempt API and Devices-tab flash.  
**This branch:** **do**  
**Reply:** `P-02`

---

#### P-03 — Provisioning design still says the browser does not send Wi-Fi/token

**Status:** updated  
**Sources:** `docs/device-provisioning-design.md` L7–17 vs L81–83 and `web/index.html` `provisionDeviceOverUsb`  
**Ask:** Lead with the implemented USB protocol; keep remaining gaps (atomic NVS, already-provisioned recovery) as remaining work.  
**This branch:** **do**  
**Reply:** `P-03`

---

#### P-04 — Devices tab leftover “Step 2 build handoff preview (not implemented)”

**Status:** leftover UI from S4 placeholder  
**Sources:** S4 L167–183; `web/index.html` L309–313  
**Ask:** Remove or relabel so it does not look like firmware build is missing from the Hub. Catalog build lives under **Firmware**.  
**This branch:** **do** (hide/remove the placeholder)  
**Reply:** `P-04`

---

#### P-05 — local-deployment says firmware APIs do not start flashing

**Status:** updated  
**Code:** `docs/local-deployment.md` L36 vs catalog flash-manifest endpoints  
**This branch:** **do**  
**Reply:** `P-05`

---

### B. Output device loop (you have this hardware)

#### P-06 — Admin Hub API to publish Output commands

**Status:** implemented in working tree (uncommitted in `hub/bootstrap/app/`)  
**Sources:** S1 §21, §28; `docs/protocol.md` L164–204  
**Code:** Hub MQTT client publish helper (`publish_json`) + `POST /api/v1/nodes/{id}/commands` in `hub/bootstrap/app/api.py`, `service.py`, `schemas.py`, `mqtt_listener.py`  
**Ask:** `POST /api/v1/nodes/{id}/commands` admin-only, Output + `lifecycle=active`, no retain, no secrets in logs.  
**This branch:** **do** (API code added uncommitted; UI in P-07)  
**Reply:** `P-06 do` (via Q-02)

---

#### P-07 — Device-detail UI to fire the pilot relay / output-test pattern

**Status:** implemented in working tree (`web/index.html` `renderDeviceDetail` Output commands panel)  
**Sources:** same as P-06  
**Code:** `web/index.html` `renderDeviceDetail` includes "Test Output" button opening a preview dialog (payload JSON with `expires_at_utc = now + 30s` + "Send Command" confirmation button)  
**This branch:** **do** (implemented: "Test Output" opens confirmation popup showing MQTT payload and 30s expiry window; real "Send Command" button dispatches to Hub API)  
**Reply:** `P-07 do (5s on, output-test, expires_at_utc + 30s popup)`

---

#### P-08 — Viewer must not be able to invoke command API directly

**Status:** implemented (`policy/authz.rego`, `api.py` `require_policy("nodes.send_command", "nodes")`, test in `test_service.py`)  
**Code:** `policy/authz.rego` (admin-only; viewer denied 403 verified by unit tests)  
**This branch:** **do** (implemented and tested)  
**Reply:** `P-08 do` (resolved with P-06 / P-07)

---

#### P-09 — Document GPIO polarity / test procedure next to the UI

**Status:** implemented in UI  
**Sources:** `docs/protocol.md` L170–171  
**Ask:** Put the same warning on the device-detail command panel.  
**This branch:** **do** (warning banner included directly on device-detail Output command panel in `web/index.html`)  
**Reply:** `P-09 do` (resolved with P-07)

---

### C. Deployment completeness

#### P-10 — After USB provision, wait for Hub hello (do not stop at NVS)

**Status:** unimplemented in UI (firmware already provisions over HTTPS)  
**Sources:** S5 L216–229; S6 L62–65  
**Code:** `web/index.html` L2538  
**This branch:** **skip** (per user reply on Q-01: not practical in community project; leave USB NVS write as completion)  
**Reply:** `P-10 skip` (via Q-01)

---

#### P-11 — Already-provisioned Output reconfigure / recovery path

**Status:** partial  
**Sources:** S6 L62; `docs/device-provisioning-design.md` L81–83  
**Code:** firmware opens a 10s USB window if already provisioned (`firmware/esp-output/main/app_main.c` L33–42); Hub cannot rotate token after `provisioned`/`active`  
**Ask:** Keep as documented recovery (reflash without erase vs new node after erase). Full in-place recovery UI is larger.  
**This branch:** **later** unless you say `do`  
**Reply:** `P-11`

---

#### P-12 — Atomic cross-namespace NVS commit

**Status:** unimplemented  
**Sources:** `docs/device-provisioning-design.md` L52  
**Ask:** Firmware change; useful before many boards, not blocking one Output test.  
**This branch:** **later**  
**Reply:** `P-12`

---

#### P-13 — Deployment-attempt records for Devices-tab flash (not only Firmware tab)

**Status:** split behavior  
**Code:** `docs/device-registration-and-deployment.md` L172, L319  
**Ask:** Unify audit, or document two UIs as intentional.  
**This branch:** **later** (document the split in P-02)  
**Reply:** `P-13`

---

### D. Hub API / MQTT platform

#### P-14 — Versioned Output command / ack / hello envelope

**Status:** implemented — command, ack, and hello carry `protocol_version: 1`  
**Sources:** `docs/protocol.md` L13–18, L158–166, L169–211  
**Code:**
- Web → device command: the device-detail Test Output body in `web/index.html` sends `protocol_version: 1` and `message_type: "output-test"`. Hub `OutputCommandBody` (`hub/bootstrap/app/schemas.py`) accepts only version 1, and `send_output_command` (`hub/bootstrap/app/service.py`) always publishes `protocol_version` on `esp-output/local/<node>/command`. `test_output_node_commands` checks that MQTT payload.
- Firmware ack: `firmware/esp-output/main/output_command.c` adds `protocol_version: 1` to every ack and publishes that same JSON on `ack` and `status`. `message_type` is copied when the command included one. A command that omits `protocol_version`, or includes any value other than the number 1, is logged and ignored, with no GPIO change and no ack.
- Firmware hello/health/keepalive: `dbmap_publish_hello` / `dbmap_publish_health` in `firmware/components/node_common/health.c` include `protocol_version: 1` plus state fields (`node_id`, `firmware_version`, `hardware_revision`, `config_version`, `calibration_version`, `classifier_version`, `uptime_s`, optional `wifi_rssi_dbm`). Health/keepalive may also attach an optional RAM-only `diag` object (D1). The Hub stores reported JSON either way (`apply_reported`).

**Ask:** Additive `protocol_version` on Output command, ack, and hello. All three include `protocol_version: 1`. Health and keepalive stay unversioned.  
**This branch:** **do**  
**Reply:** `P-14`

---

#### P-15 — Hub subscribe to `ack` and keep last command result

**Status:** unimplemented  
**Code:** `mqtt_listener.py` has no `+/ack` subscription; status publish from firmware can overwrite `reported.payload_json` with ack-only JSON (`output_command.c` L180)  
**Ask:** Either dedicated last-command field, or stop stuffing acks into the health payload.  
**This branch:** **later** (command UI will say “published”; check serial/LED). Can promote if you reply `do`.  
**Reply:** `P-15`

---

#### P-16 — Desired-state delivery (see Q-05)

**Status:** stored only  
**This branch:** **later**  
**Reply:** `P-16`

---

### E. Identity, policy, multi-user

#### P-17 — Ordinary-user sessions / OAuth

**Status:** schema only  
**Sources:** S3; `docs/identity-and-operating-modes.md`  
**This branch:** **later** (Q-06)  
**Reply:** `P-17`

---

#### P-18 — `hub_id` column on nodes

**Status:** Hub identity is configured `DBMAP_HUB_ID` / status API `hub_id` (default `local-01`). There is still no per-node `hub_id` column; this single Local Hub remains the association boundary.  
**Sources:** S3 §6; `docs/identity-and-operating-modes.md`  
**This branch:** **later** for a per-node column / multi-Hub association (config + status `hub_id` already in use)  
**Reply:** `P-18`

---

### F. Web / HTTPS / Compose

#### P-19 — HTTPS `:8443`, base path, optional Avahi

**Status:** implemented  
**Sources:** S2 §4; `compose.yaml`; `scripts/publish-hub-mdns.sh`  
**Ask:** No code. Confirm you run `./scripts/publish-hub-mdns.sh` when `hub.local` is needed.  
**Reply:** `P-19` (`skip` if already in your boot notes)

---

#### P-20 — firmware-builder serves HTTPS on port 8443 (dedicated certificate)

**Status:** Resolved — firmware-builder now runs HTTPS on port 8443 using a dedicated TLS
certificate (`firmware-builder.crt` / `firmware-builder.key`) generated by
`scripts/gen-mqtt-certs.sh` and signed by the same local CA as all other services.  
**Changes:**
- `scripts/gen-mqtt-certs.sh` — generates `shared/firmware-builder/certs/firmware-builder.crt` / `.key`
  (CN=`firmware-builder`, SAN=`DNS:firmware-builder`) alongside the existing `web` and `opa`
  certificates.
- `firmware-builder/server.py` — server socket wrapped with `ssl.SSLContext`; listens on
  `0.0.0.0:8443` (HTTPS).
- `firmware-builder/Dockerfile` — `EXPOSE 8443` (port 8090 removed).
- `compose.yaml` — publishes port 8443; healthcheck uses `https://firmware-builder:8443/health`
  with the CA bundle at `/certs/ca.crt`. The certificate SAN is `DNS:firmware-builder`, so a
  check against `127.0.0.1` fails verification.
- `nginx/default.conf.template` — builder proxy location updated to `https://firmware-builder:8443`
  with `proxy_ssl_name firmware-builder`.

**Reply:** `P-20`

---

#### P-21 — Automated tests for root vs `/dbmap` deep links

**Status:** S2 L376 asks for them; not present as a dedicated suite  
**This branch:** **later**  
**Reply:** `P-21`

---

### G. Intentionally not following (until you say otherwise)

These instruction-file items are **not** platform blockers for Output or for later sensor bring-up. Reply only if you disagree.

| ID | Instruction | Why not now |
| --- | --- | --- |
| X-01 | S1 event/track engine, map, uncertainty | You scheduled this with sensor hardware |
| X-02 | S2 Acoustic Monitors, WAV training, raw audio | Sensor + privacy/policy work |
| X-03 | S2 Home Assistant + regional export | Needs stable local command/event API first (P-06 is the seed) |
| X-04 | S1 Regional Hub | Outbound-only design; no second site |
| X-05 | S4 “do not implement flash/provision” | Superseded by S6 and the current repo |
| X-06 | S3 “do not rebuild Devices tab” | Tab already evolved; freeze further redesign |
| X-07 | S2 remove Site Map | No Site Map in current `web/index.html` |

---

## 6. Suggested order

1. **This branch:** P-01…P-05, P-06…P-09 (docs + Hub command; post-provision wait P-10 skipped per Q-01).
2. **Your first Output test:** register → catalog flash → USB provision → Hub shows online → relay 5s from UI.
3. **Your replies:** Q-03, Q-04, Q-05, P-11, P-15 if you want them before sensor boards arrive.
4. **After sensor hardware:** direction estimation and event/track — not this document.
