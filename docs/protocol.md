# dBmap Protocol Contract

This document describes the contract between dBmap nodes, simulators, the
Local Hub, and the MQTT broker. It records implemented behavior separately
from planned behavior. It is not an implementation guide.

## Status and compatibility

The current observation message protocol is version 1. The fields
`protocol_version` and `message_type` are required on observations. The hub
rejects unsupported versions and message types.

Output `hello`, `health`, `keepalive`, command, and acknowledgement messages
carry `protocol_version: 1`. Health and keepalive may also include an optional
`diag` object (D1). These messages are separate from the observation envelope.
The hub accepts hello, health, and keepalive with or without `protocol_version`
and stores the reported state either way. Unknown optional fields (including
`diag`) are preserved in reported payload JSON and ignored by older Hub logic.

Incompatible changes require a new protocol version. Receivers must reject
unknown versions rather than silently interpreting them as version 1.

## Identifiers

| Identifier | Current contract |
| --- | --- |
| Node ID | Assigned by the hub. Physical Outputs use `OUT-<sequence>`, physical Ears use `EAR-<sequence>`, and simulated Ears use `SIM-EAR-<sequence>` (for example `SIM-EAR-001`). The sequence is at least three digits and may grow. IDs are case-insensitive for API lookup; MQTT topic node slugs are lowercase. |
| Observation ID | UUID in `identity.observation_id`. It labels one observation episode on the Ear; the same ID may appear on multiple MQTT payloads when emissions share an episode (different `sequence_number` values). |
| Sequence number | Monotonic per Ear in `identity.sequence_number`. Together with `identity.node_id` it is the hub deduplication key: re-publishing the same pair does not create another row. A conflicting `observation_id` for an already stored sequence is rejected. |
| Event ID | Reserved for a future hub-correlated event. No event message, allocation rule, or API is currently implemented. |
| Track ID | Optional UUID in `identity.track_id`. Labels physical or acoustic continuity across Observation report episodes on one Ear. Omitted or null on older firmware. Hub accepts and stores the value; it does not mint tracks or correlate Events from it yet. Report rotation keeps the same `track_id` while minting a new `observation_id`. |

Simulator identities are deliberately separate from physical Ear identities.
Only simulated Ear registrations may request a `SIM-EAR-*` identity; Output
nodes cannot be registered as simulated.

## MQTT topics

Node topic roots are:

```text
esp-output/local/<lowercase-node-id>/
esp-ear/local/<lowercase-node-id>/
```

The hub returns the provisioned topics in the provisioning response. The
current topic names are:

| Suffix | Direction | Current purpose |
| --- | --- | --- |
| `hello` | Node to hub | Announces/re-announces node state. |
| `keepalive` | Node to hub | Periodic liveness and reported state. |
| `health` | Node to hub | Periodic health and reported state. |
| `status` | Node to hub | State/status updates; Output command handling currently publishes here. |
| `ack` | Node to hub | Output command acknowledgement. |
| `command` | Hub to node | Commands. The current Output firmware subscribes here. |
| `config` | Hub to node | Reserved for configuration delivery. The current broker ACL allows it, but configuration delivery is not implemented. |
| `observation` | Ear to hub | Ear observation messages. The hub accepts messages only on the sending registered Ear's own topic. |

Node clients may publish only to their own node topics. They may subscribe
only to their own `command` and `config` topics. The hub uses a separate
broker identity with broader access. Broker ACLs default to deny.

## Message formats

### Observation — protocol version 1 (hierarchical)

An observation is UTF-8 JSON with a **hierarchical** envelope only. Flat
v1 payloads (top-level `observation_id`, `duration_ms`, and similar) are
**rejected**. Clear stored development observations before cutover; see
[Ear simulator — clear observations](./simulator.md#clear-observations-keep-sim-ears).

Required top-level keys: `protocol_version`, `message_type`, `identity`,
`context`, `acoustic`, `spatial`. The hub validates the envelope, the node
identity, and Ear registration before persistence.

```json
{
  "protocol_version": 1,
  "message_type": "observation",
  "identity": {
    "observation_id": "00000000-0000-0000-0000-000000000001",
    "node_id": "SIM-EAR-001",
    "hub_id": "",
    "sequence_number": 1,
    "track_id": null
  },
  "context": {
    "event_time_utc": "2026-10-05T15:00:00Z",
    "capture_timestamp_monotonic_us": 123456789,
    "timing_quality": "estimated",
    "clock_offset_ms": null,
    "timestamp_uncertainty_ms": 10.0,
    "interval_duration_ms": 850.0,
    "episode_duration_ms": 850.0,
    "emission_kind": "periodic"
  },
  "acoustic": {
    "signal_level_dbfs": -34.0,
    "measurements": {
      "schema": "l0",
      "version": 1,
      "band_plan_version": 1,
      "crest_threshold_version": 1,
      "crest_threshold_db": 12.0,
      "mic_index": 0,
      "sample_rate_hz": 16000,
      "analysis_window_ms": 1000,
      "window_count": 0,
      "clipped_window_count": 0,
      "n_crest": 0,
      "rms": { "mean_dbfs": null, "min_dbfs": null, "max_dbfs": null },
      "bands": {
        "mean_dbfs": [null, null, null, null],
        "fraction": [null, null, null, null]
      },
      "hl": { "mean_db": null },
      "t1": { "rms_stdev_db": null, "rms_range_db": null },
      "t2": { "rms_slope_db_per_s": null },
      "t3": {
        "crest_max_db": null,
        "high_crest_count": null,
        "high_crest_rate": null
      },
      "t6": { "hl_slope_db_per_s": null }
    },
    "signature": null,
    "classification": {
      "schema": "l2",
      "version": 1,
      "source_family": "vehicle",
      "confidence": 0.72,
      "hints": { "simulated_source_id": "SIM-VEHICLE-001" }
    },
    "assessment": null,
    "evidence": null,
    "debug": null
  },
  "spatial": {
    "bearing": {
      "deg": 180.0,
      "reference": "node",
      "confidence": 0.61
    },
    "position": null
  }
}
```

`context.emission_kind` is `start`, `periodic`, `end`, or `complete`.
`acoustic.measurements` (schema `l0`, combined mic scene) is **required**; metric
slots may be JSON null when insufficient data. Unknown keys inside
`measurements` are rejected. Full field rules:
[L0 measurement contract](./ear-l0-measurement-contract.md).
`acoustic.signature` is JSON null when L1 is disabled; otherwise a
`schema: "l1"` object (may be empty). See
[L1 signature contract](./ear-l1-signature-contract.md).

`context.termination_reason` is required when `emission_kind` is `end` or
`complete`. Allowed values: `natural`, `invalid_hold`, `ota_reboot`,
`report_rotation`. `report_rotation` means the Observation report episode ended
because of a reporting budget/interval rotation; physical track continuity is
conveyed by a stable `identity.track_id`, not inferred from the reason alone.
The hub rejects `termination_reason` on other emission kinds.

`context.event_time_utc` must include a timezone and marks the beginning of
the observation interval; the hub normalizes it to UTC.
`interval_duration_ms` is the interval length;
`episode_duration_ms` spans the Ear episode when emissions share an
`observation_id`. `timing_quality` is `synchronized`, `estimated`, or
`unsynchronized`. `acoustic.classification` (schema `l2`) and `spatial.bearing`
may be `null`. Optional EAR fields `acoustic.assessment` (`schema: ear_hyp_v1`),
`acoustic.evidence` (`schema: ear_ev_v1`), and development-only
`acoustic.debug` (`schema: ear_dbg_v1`) may be omitted or null; when present they
use strict nested objects (`extra` forbidden). Do not place EAR hypotheses in
`classification.hints`. `spatial.position` is reserved (currently `null`).
A future `unique_tracks` API metric (distinct `track_id`) is a separate
follow-up; `unique_observations` continues to count distinct `observation_id`
values only.

Retained observations are rejected. Duplicate MQTT delivery is safe when the
same `(node_id, sequence_number)` pair is reused. Stored `payload_json` preserves
the validated envelope; the API returns it plus `received_time_utc`.

### Sequence reconciliation (Ear ↔ Hub)

Distinguish three layers when debugging:

1. **Detector emissions** — START / PERIODIC / END on the Ear (serial diag /
   counters). May occur while MQTT is down.
2. **MQTT publish attempts** — Ear increments `sequence_number` only when it
   builds and publishes a payload. Refused L0 / missing measurements do **not**
   create a Hub row; watch Ear logs for refuse / drop counters.
3. **Hub-accepted records** — one row per unique `(node_id, sequence_number)`.
   Dedup returns success without a second row.

Reconciliation procedure: for a time window, list Ear serial/MQTT sequence
values, then `GET /api/v1/observations?node_id=…` and match
`identity.sequence_number` / `context.emission_kind`. Gaps mean publish
refuse or broker loss, not Hub rewriting sequences.

The simulator uses the same hierarchical envelope and topic as a registered
Ear. Scenario simulator observations may carry
`acoustic.classification.hints.simulated_source_id` as test-only ground truth.
`simulate_observation.py --include-l0` attaches deterministic L0 scene fixtures;
the baseline simulator still sends required empty L0 measurements.

`scripts/hub_event_track.py` is a read-only exploratory report. It fetches node
installation metadata, prints each observation independently, and reports
pairwise Ear site distances as context (annotated `≤500m` / `>500m`;
`--allow-long-range-sim-ears` marks far `SIM-EAR-*` pairs `SIM eligible`
without changing the 500 m threshold or creating associations). It also prints
one Hub regional weather block from
`GET /api/v1/hub/environmental-context` (Open-Meteo; informational only).
Per-Ear inactivity episodes default to a 30 s silence window. It deliberately
does not read `classification.hints.simulated_source_id` for grouping or
correlation.
Per-Ear inactivity episodes are time summaries only, not Events or claims that
the observations share a source. The tracker does not yet produce predicted
cross-Ear associations or Tracks. Watch mode re-fetches observations and node
metadata and recalculates the report when new observations arrive.

The scenario simulator keeps ground-truth source movement in the scenario
definition and may attach the scenario's ID to observations as test-only
metadata. Tracker logic must not consume that ID. It is reserved for
after-the-fact evaluation once the tracker produces predicted associations.
Event/track APIs, association logic, and the truth-versus-prediction evaluator
remain unimplemented.

### Existing Output and state messages

The current ESP-Output sends JSON state on `hello`, `health`, and
`keepalive`, including `protocol_version: 1`, `node_id`, `firmware_version`,
`hardware_revision`, `config_version`, `calibration_version`,
`classifier_version`, `uptime_s`, and the optional `wifi_rssi_dbm` signal
level. Health and keepalive may also include an optional compact `diag`
object when RAM diagnostics have something to report (for example
`wifi_disconnects`, `mqtt_disconnects`, `mqtt_reconnects`,
`mqtt_connect_attempts`, `mqtt_connect_failures`, `desired_failures`,
and a one-shot `last_reset_reason` after boot). Hello stays lean and omits
`diag`. Counters are RAM-only and clear on reboot; they are not written to
NVS. The Hub derives counter deltas from the last retained diag snapshot so
a hello that omits `diag` does not re-count cumulative totals. The Hub
derives `pending_configuration_change` by comparing reported state with
desired state; the device does not report that flag. These messages are not
wrapped in the version-1 observation envelope.

MQTT and Wi-Fi reconnect use independent exponential backoff in
`node_common` (`dbmap_backoff`); see [Health and availability](./health-and-availability.md).

Diagnostic defaults (enable flag, RSSI degraded threshold, counter saturate)
ship as firmware/Kconfig defaults. A future Desired configuration sync can
override them via `payload.diag` while bumping `config_version` (the revision
counter). Example Desired body:

```json
{
  "firmware_version": "0.1.20",
  "config_version": 3,
  "calibration_version": 0,
  "classifier_version": "",
  "payload": {
    "diag": {
      "enabled": true,
      "rssi_degraded_dbm": -80,
      "counter_saturate": 999
    }
  }
}
```

Until configuration sync is implemented on-device, only Kconfig defaults apply.
`dbmap_diag_apply_desired_payload()` is the merge hook for that future path.

The Output command contract is not yet implemented as a stable, versioned
schema. The current firmware supports a pilot relay and audio subset. Commands carry
`protocol_version: 1`. A command that omits `protocol_version`, or includes
any other value, is logged and ignored: the firmware does not drive GPIO/audio and
does not publish an acknowledgement.
`RELAY_1_ON` needs an integer `duration_s` from 1
through 3600, drives GPIO16 high for that many seconds, and blinks the onboard
GPIO2 LED during the interval. `RELAY_1_OFF` switches both outputs off
immediately. `AUDIO_PLAY_HTTPS` needs an `https://` `url` and starts Helix MP3
streaming to the MAX98357. `AUDIO_PLAY_FILE` needs a `path` under the LittleFS
`/media` mount (relative name or `/media/...`). `AUDIO_STOP` stops playback.
The firmware logs the received JSON and publishes the action
result to the acknowledgement and status topics. Commands without a supported
action or valid fields are rejected. GPIO16 is assumed active-high; verify
the relay module's electrical polarity before connecting it. The optional
`message_type: "output-test"` field selects a short double-pulse GPIO2 pattern
for testing. A missing field or any other value uses the standard alert blink
pattern.

The Output firmware synchronizes its UTC clock through SNTP and includes the
UTC receive time in command console logs. The NTP server is set by
`DBMAP_NTP_SERVER` at firmware build time; the default is the Swedish NTP Pool
zone. If the clock has not synchronized yet, the log identifies the timestamp
as unsynchronized.

The intended versioned command schema includes an absolute `expires_at_utc`
timestamp:

```json
{
  "protocol_version": 1,
  "message_type": "output-test",
  "action": "RELAY_1_ON",
  "duration_s": 5,
  "expires_at_utc": "2026-10-06T10:05:00Z"
}
```

`expires_at_utc` is the semantic validity deadline, independent of transport.
It must be an ISO 8601 timestamp with an explicit timezone. Device-side expiry
validation is now implemented; the firmware checks the timestamp and ignores
expired commands. The pilot acknowledgement and status payload looks like:

```json
{"node_id": "OUT-011", "action": "RELAY_1_ON", "result": "relay_1_on", "duration_s": 5}
```

Command, acknowledgement, and status payloads do not yet have a stable,
versioned schema. An acknowledgement reports that the firmware issued GPIO
operations; it does not independently confirm the relay's physical state.

### OTA update status (Output)

After a successful OTA image install (boot slot switched, before reboot), the
device publishes a QoS 1 `status` message and waits up to 5 seconds for MQTT
PUBACK. Missing MQTT or PUBACK timeout must not block the reboot. A minimal
pending record (previous version, target version, firmware id) is written once
to NVS so the outcome can be reported after boot if the pre-reboot publish was
lost. After the install report, firmware enters **OTA reboot pending**: Wi-Fi/MQTT
reconnects are frozen, expected teardown is not counted as a connectivity
incident, optional quick MQTT disconnect runs for at most 500 ms, then reboot.

```json
{
  "protocol_version": 1,
  "message_type": "ota_update",
  "event": "install_succeeded",
  "ota_phase": "installed_pending_reboot",
  "reboot_reason": "ota",
  "node_id": "OUT-011",
  "firmware_version": "0.1.4",
  "previous_firmware_version": "0.1.4",
  "target_firmware_version": "0.1.5",
  "firmware_id": "…"
}
```

`firmware_version` here is still the **running** (old) image. `reboot_reason`
is `ota` and is distinct from diag `last_reset_reason` (for example `SW` after
`esp_restart`). A software reset alone does not prove the reboot was OTA.

After reboot, once MQTT is up and a pending-verify image is marked valid, the
device publishes:

```json
{
  "protocol_version": 1,
  "message_type": "ota_update",
  "event": "boot_confirmed",
  "ota_phase": "boot_confirmed",
  "reboot_reason": "ota",
  "firmware_version": "0.1.5",
  "previous_firmware_version": "0.1.4",
  "target_firmware_version": "0.1.5"
}
```

Normal `hello` / `health` / `keepalive` continue to report the actual running
`firmware_version`. The Hub records `ota_install_reported` when the pre-reboot
status arrives, keeps a pending transition, and confirms with
`ota_boot_confirmed` when either the boot status or a later hello/health shows
the target version. Installed-pending-reboot is not the same as a confirmed
boot into the new image.

## Delivery and timing

MQTT uses MQTT 3.1.1 over TLS. The current QoS behavior is:

| Message | QoS |
| --- | --- |
| `hello` | 1 |
| Output `health` and `keepalive` | 0 |
| Output `status` | 1 |
| Ear `status` (OTA install / boot confirmation) | 1 |
| Output command subscription | 1 |
| Ear observation publication and hub subscription | 1 |
| `ack` | 1 in the current Output firmware |

The hub does not rely on retained observation messages and rejects them.
Commands must not be retained. Health/status messages are not a durable
history; QoS 0 messages may be lost. QoS 1 may deliver duplicates, so
consumers of durable observations must deduplicate by observation ID.

MQTT QoS describes delivery assurance; it does not describe whether a command
is still semantically valid. The command's `expires_at_utc` remains
authoritative for every transport. The current broker connection uses MQTT
3.1.1 and therefore has no MQTT 5 Message Expiry Interval. If the transport is
later upgraded to MQTT 5, the hub may additionally set Message Expiry Interval
from the command's remaining lifetime when publishing. Broker expiry is
defense in depth only: Output must still check `expires_at_utc`, since queued
or duplicated delivery must never make an expired command executable.

MQTT clients currently negotiate a 30-second keepalive. The existing
ESP-Output publishes hello and health/keepalive at approximately 30-second
intervals. An Ear duty-cycle/poll schedule, including the previously
considered 24-hour poll, is not implemented and remains to be specified with
the Ear firmware.

## Transport and authentication

- The broker accepts MQTT over TLS on port `8883`. The broker certificate is
  validated against the local CA by firmware and clients.
- Broker client certificates are not required. Current MQTT authentication is
  per-node username/password, not mutual TLS (mTLS).
- Node credentials are issued during HTTPS provisioning and scoped by broker
  ACL to that node's topics.
- The bootstrap API is reached through Nginx over HTTPS on port `8443` by default. Administrative API routes
  require `Authorization: Bearer <admin-token>`. Initial provisioning uses a
  one-time bootstrap token; the hub stores its hash and consumes it once.
- Never commit private CA keys, device credentials, bootstrap tokens, or local
  Wi-Fi secrets. See [Security Policy](../SECURITY.md).

## Configuration versions and state

The hub stores desired state and reported state separately.

| Version/state field | Meaning |
| --- | --- |
| `firmware_version` | Desired or reported firmware release identifier. |
| `config_version` | Desired or reported configuration revision. |
| `calibration_version` | Desired or reported calibration revision. |
| `classifier_version` | Desired or reported classifier revision. |
| `payload` | Extension object for desired or reported data. |

An administrator updates desired configuration/version fields with
`PUT /api/v1/nodes/{node_id}/desired` (not `firmware_id`). Changing Desired
firmware for OTA uses `POST /api/v1/nodes/{node_id}/firmware-selection`, which
sets both `firmware_id` and `firmware_version` from a catalog entry. Node APIs
return both `desired` and `reported` state and calculate
`pending_configuration_change` by comparing the version fields. Nodes currently
report versions in their periodic messages. Desired configuration is stored by
the hub but is not yet delivered over MQTT, and firmware does not yet apply or
acknowledge configuration revisions. That synchronization behavior must be
defined before configuration delivery is treated as operational.

## Hub API surface

The current relevant HTTPS API endpoints are:

| Endpoint | Purpose | Authentication |
| --- | --- | --- |
| `GET /api/v1/session` | Identify the role associated with a local access token. | Admin or viewer access token |
| `GET /api/v1/devices` | Read basic device status and installation summaries. | Admin or viewer access token |
| `POST /api/v1/nodes` | Create a node and issue its one-time bootstrap token. | Admin bearer token |
| `POST /api/v1/provision` | Consume the bootstrap token and issue per-node MQTT credentials, topics, and a one-time device API token. Ordinary calls require `provisioning_state=pending`. | One-time bootstrap token |
| `GET /api/v1/device/desired-firmware` | Read the authenticated device's Desired firmware id, version, and `esp-output.bin` SHA-256/size when the Desired catalog entry exists and its metadata version matches Desired `firmware_version` (candidates allowed when they are the Desired id). If id and version disagree, id/version are still returned but SHA/size are empty so the device will not OTA. ESP-Output calls this at boot and every 60 minutes. On version mismatch with a non-empty id, SHA, and size it starts one OTA attempt and aborts without switching slots if the downloaded app descriptor version differs from Desired. | Device API token. Not OPA. |
| `GET /api/v1/device/ota-image` | Download `esp-output.bin` for the authenticated node's current Desired firmware only. No firmware id in the path or query. Rejects when Desired `firmware_version` does not match the catalog entry for `firmware_id`, and rejects images larger than the Output OTA slot (`0x1C0000`). Response includes `X-Content-SHA256`. | Device API token. Not OPA. |
| `GET /api/v1/nodes` | List nodes with full identity and desired/reported state. | Admin bearer token |
| `GET /api/v1/nodes/{node_id}` | Read one node's identity and desired/reported state. | Admin or viewer bearer token |
| `PUT /api/v1/nodes/{node_id}/desired` | Replace desired configuration/version fields. Does not change `firmware_id`. When `firmware_id` is set, rejects a `firmware_version` that does not match that catalog entry. Use `POST /api/v1/nodes/{node_id}/firmware-selection` to change Desired firmware for OTA. | Admin bearer token |
| `PUT /api/v1/nodes/{node_id}/installation` | Store mandatory WGS84 latitude/longitude and optional placement metadata. | Admin bearer token |
| `GET /api/v1/observations` | Read persisted observation reports as `{pagination, reports}` (shared pagination contract; collection key remains `reports`). | Admin or viewer bearer token |
| `GET /api/v1/users` | List internal users as `{pagination, items}`. | Admin or viewer bearer token |
| `GET /api/v1/groups` | List groups as `{pagination, items}`. | Admin or viewer bearer token |

### Hub list pagination

`GET /api/v1/observations`, `GET /api/v1/users`, and `GET /api/v1/groups` share one JSON envelope shape:

```json
{
  "pagination": {
    "total_count": 0,
    "page": 1,
    "page_size": 200,
    "total_pages": 0,
    "order": "newest",
    "snapshot_id": null
  },
  "items": []
}
```

Defaults and limits: `page=1`, `page_size=200`, maximum `page_size=500`, `order` is `newest` or `oldest`. `total_count` / `total_pages` reflect the filtered snapshot. Empty results return `total_count=0`, `total_pages=0`, an empty collection, and `snapshot_id=null`.

Resource-specific differences:

| Resource | Collection key | Order key + tie-breaker | Snapshot boundary |
| --- | --- | --- | --- |
| Observations | `reports` (retained) | `received_time_utc`, `id` | Autoincrement `Observation.id` |
| Users | `items` | `created_at`, `user_id` | SQLite `rowid` (UUID PK is not monotonic) |
| Groups | `items` | `created_at`, `group_id` | SQLite `rowid` (UUID PK is not monotonic) |

Pass `pagination.snapshot_id` from the first page on later requests so concurrent inserts do not duplicate or omit rows mid-traversal. Observations also accept deprecated `limit` as an alias for `page_size`.

The local web UI accepts an admin or viewer bearer token. Viewer access is
limited to session, Hub status, device summaries, device details, and
observations; it does not include node list or write actions. Device status
is derived from the most recent Hub keepalive: online within `DBMAP_DEVICE_ONLINE_WINDOW_S` (default 90 seconds),
otherwise offline. See the [local deployment guide](./local-deployment.md)
for configuring the two local tokens.

Event and track APIs, a general node polling endpoint, and configuration
delivery are not implemented.
