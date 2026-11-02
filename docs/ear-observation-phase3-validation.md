# Ear Observation — Phase 3 field validation

Validate the complete path on **one** physical ESP32-S3 Ear with INMP441:

Physical microphone → I2S → `ear_mic` metrics → RMS activity detector → Observation publisher → MQTT → Hub ingest → `GET /api/v1/observations`.

Do **not** wipe observation stores or Docker volumes for this checklist.

## Review points A–C (resolved in firmware)

### A. Microphone diagnostic gating

`microphone_status` is produced by `dbmap_audio_classify_microphone()` (`audio_metrics.c`): structure-based presence diagnostics (zero ratio, unique samples, rail hits), **not** an RMS activity threshold and **not** `source_family`.

| Status | Meaning (diagnostics) | Activity input gate |
| --- | --- | --- |
| `signal_present` | Ambient-like energy distribution | Allowed |
| `unknown` | Ambiguous window (e.g. weak but not faulted) | **Allowed** — RMS thresholds decide activity |
| `noise_suspected` | Floating DIN / rail-sparse patterns | Rejected |
| `disconnected_suspected` | Digital silence / near-all-zero | Rejected |
| `unavailable` | Transport unusable / no samples | Rejected |

Transport must be `ok` or `degraded`. Activity enter/exit uses only RMS vs Kconfig thresholds.

### B. Invalid measurements during an active episode

- Invalid windows **never** count as acoustic silence and **do not** advance exit-hold or emit periodic Observations.
- Episode state is **held**; when usable samples resume, detection continues (exit-hold restarts only from new quiet usable samples).
- After `DBMAP_EAR_OBS_MAX_INVALID_HOLD_MS` (default 30 s) without a usable sample while ACTIVE/PENDING_EXIT, emit a forced **END** ending at the **last usable** measurement time so episodes cannot stay open indefinitely.

### C. Phase 1 sequence migration

Phase 1 NVS key `seq` stored the **next** sequence to assign (persisted **before** MQTT publish). Migration: if `seq_lim` is absent, set durable limit to `legacy_seq + block_size` (skip-forward). Numbers are never allocated until the block’s `seq_lim` is successfully committed to NVS. Overflow saturates at `UINT32_MAX` and stops allocation (no wrap).

## Build / flash

Prefer Hub firmware build target `ear-esp32-s3`, or local:

```bash
cd firmware/esp-ear
idf.py set-target esp32s3
idf.py build flash monitor
```

Confirm boot logs include `RMS activity detector ready` and `reserved sequence block`.

## Hub checks (non-destructive)

```bash
# List recent observations for the physical Ear (example EAR-001)
curl -sS -H "Authorization: Bearer $DBMAP_ADMIN_TOKEN" \
  "https://hub.local:8443/api/v1/observations?node_id=EAR-001&limit=20" | jq .
```

Or: `uv run python scripts/hub_event_track.py list` (read-only).

## Field checklist (one Ear)

| # | Scenario | Expect |
| --- | --- | --- |
| 1 | Quiet room after boot-settle | No Observations (or only forced ends if previously active) |
| 2 | Normal indoor background | Usually below enter threshold; no false START from boot floating-DIN |
| 3 | Deliberately louder sustained sound | START, then PERIODIC at interval, END after quiet + exit hold |
| 4 | Weak sound near enter threshold | May START if RMS usable (`unknown` allowed); tune thresholds if needed |
| 5 | MQTT disconnect during activity | Detector keeps running; emissions dropped; no sequence consumed for drops; after reconnect new emissions publish |
| 6 | Reboot | `sequence_number` continues at/above prior `seq_lim` (gaps OK); never decreases |
| 7 | Unplug / fault mic path briefly while active | No false END from short invalid; forced END only after max invalid hold |
| 8 | Capabilities on hello/health | `detector_kind=rms_activity`, `detector_version=1` |

## Opt-in serial diagnostics (field bring-up)

For continuous RMS / detector visibility **without** relying on MQTT/HTTPS logs, enable `CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG` (default **off**). See [`firmware/esp-ear/README.md`](../firmware/esp-ear/README.md) § Opt-in serial diagnostics.

Does **not** change thresholds, hysteresis, timing, detector transitions, or Observation schema. Disable for production fleet images.

## Out of scope

Classification, bearing, multi-source separation, Hub Event/Track correlation, simulator changes, protocol schema changes, dataset/ML tooling.
