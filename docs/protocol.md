# dBmap Protocol Contract

This document describes the contract between dBmap nodes, simulators, the
Local Hub, and the MQTT broker. It records implemented behavior separately
from planned behavior. It is not an implementation guide.

## Status and compatibility

The current observation message protocol is version 1. The fields
`protocol_version` and `message_type` are required on observations. The hub
rejects unsupported versions and message types.

Output `hello`, command, and acknowledgement messages carry
`protocol_version: 1`. Hello uses that field alone. Health and keepalive
keep the unversioned state payload. These messages are separate from the
observation envelope. The hub accepts hello, health, and keepalive with or
without `protocol_version` and stores the reported state either way.

Incompatible changes require a new protocol version. Receivers must reject
unknown versions rather than silently interpreting them as version 1.

## Identifiers

| Identifier | Current contract |
| --- | --- |
| Node ID | Assigned by the hub. Physical Outputs use `OUT-<sequence>`, physical Ears use `EAR-<sequence>`, and simulated Ears use `SIM-EAR-<sequence>` (for example `SIM-EAR-001`). The sequence is at least three digits and may grow. IDs are case-insensitive for API lookup; MQTT topic node slugs are lowercase. |
| Observation ID | UUID. It identifies one observation and is the hub's deduplication key. Re-publishing the same observation ID from the same Ear does not create another record. Reusing it from a different node is rejected. |
| Event ID | Reserved for a future hub-correlated event. No event message, allocation rule, or API is currently implemented. |
| Track ID | Reserved for a future source track. No track message, allocation rule, or API is currently implemented. |

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

### Observation — protocol version 1

An observation is UTF-8 JSON. The hub validates its fields, the node identity,
and registration as an Ear before persistence.

```json
{
  "protocol_version": 1,
  "message_type": "observation",
  "observation_id": "00000000-0000-0000-0000-000000000001",
  "node_id": "SIM-EAR-001",
  "sequence_number": 1,
  "event_time_utc": "2026-10-05T15:00:00Z",
  "capture_timestamp_monotonic_us": 123456789,
  "timing_quality": "estimated",
  "clock_offset_ms": null,
  "timestamp_uncertainty_ms": 10.0,
  "classification": {
    "source_family": "vehicle",
    "confidence": 0.72,
    "hints": {
      "simulated_source_id": "SIM-VEHICLE-001"
    }
  },
  "bearing": {
    "deg": 180.0,
    "reference": "node",
    "confidence": 0.61
  },
  "signal_level_dbfs": -34.0,
  "duration_ms": 850.0
}
```

`event_time_utc` must include a timezone and marks the beginning of the
observation interval; the hub normalizes it to UTC. `duration_ms` is the
interval length, so the observation covers
`[event_time_utc, event_time_utc + duration_ms)`. Ending that interval means
only that this Ear no longer observed that signal; it does not mean the
physical source disappeared. A later detection is a new observation with its
own ID and sequence number. Ears report observations and do not claim that
separate observations came from the same physical source.
`timing_quality` is `synchronized`, `estimated`, or `unsynchronized`.
`classification` and `bearing` may be `null` when no result is available.
Classification confidence describes confidence in `source_family`; bearing
confidence describes confidence in the direction estimate. Both are bounded
from 0 to 1. A bearing's `deg` is in `[0, 360)`, `reference` is currently
`node`, and its angle is clockwise from the Ear's configured forward/reference
axis: `0` is that axis, `90` is clockwise from it, `180` is behind it, and
`270` is counter-clockwise from it. It is not itself a geographic or magnetic
compass bearing. Installation `orientation_deg`, when present, is clockwise
from geographic true north and defines the node's forward/reference axis.
Combining installation orientation and node-relative bearing yields a
geographic bearing modulo 360; the hub does not yet calculate or return that
conversion.

`signal_level_dbfs` is a digital signal level relative to the audio chain's
full-scale value. It is not dB SPL and is not comparable as an absolute
acoustic pressure level without hardware-chain calibration. Optional
measurements may be `null`; durations and timestamp uncertainties must be
non-negative. The hub adds `received_time_utc` to the observation returned by
its API.

Retained observations are rejected. Duplicate deliveries are safe when the
same observation ID is used. The simulator uses the same envelope and topic
as a registered Ear. Scenario simulator observations may carry
`classification.hints.simulated_source_id` as test-only ground truth, not as a
real-world identity or an event/track identifier. Tracker analysis must not
consume that hint for association.
Previously stored observations using the earlier flat `confidence`,
`bearing_deg`, and `classification_hints` fields must be cleared before
deploying this schema revision. See the simulator cleanup instructions in the
README. New MQTT observations must use this structured form; legacy fields
are rejected rather than silently discarded.

`scripts/hub_event_track.py` is a read-only exploratory report. It fetches node
installation metadata, prints each observation independently, and reports
pairwise Ear site distances as context. It deliberately does not read
`classification.hints.simulated_source_id` for grouping or correlation.
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
`keepalive`, including `node_id`, `firmware_version`, `hardware_revision`,
`config_version`, `calibration_version`, `classifier_version`, `uptime_s`,
and the optional `wifi_rssi_dbm` signal level. Hello also includes
`protocol_version: 1`. Health and keepalive use the same state fields
without that version field. The Hub derives
`pending_configuration_change` by comparing reported state with desired state;
the device does not report that flag. These messages are not wrapped in the
version-1 observation envelope.

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

## Delivery and timing

MQTT uses MQTT 3.1.1 over TLS. The current QoS behavior is:

| Message | QoS |
| --- | --- |
| `hello` | 1 |
| Output `health` and `keepalive` | 0 |
| Output `status` | 1 |
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
| `GET /api/v1/observations` | Read persisted observations, optionally filtered by `node_id`. | Admin or viewer bearer token |

The local web UI accepts an admin or viewer bearer token. Viewer access is
limited to session, Hub status, device summaries, device details, and
observations; it does not include node list or write actions. Device status
is derived from the most recent Hub keepalive: online within 90 seconds,
otherwise offline. See the [local deployment guide](./local-deployment.md)
for configuring the two local tokens.

Event and track APIs, a general node polling endpoint, and configuration
delivery are not implemented.
