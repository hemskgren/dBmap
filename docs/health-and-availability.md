# Health and Availability (Local Hub)

Phase 1 monitoring for the Local Hub, plus device **availability** (D0) and
device **health** from optional on-device diagnostics (D1 firmware / D2 Hub).
Planned reboots, leaderboards, and Regional Hub synchronization remain later work.

## Concepts

Health and Availability are independent.

| Metric | Meaning | Time basis |
| --- | --- | --- |
| **Health** | Current condition of Hub services | Instantaneous |
| **Core Hub availability** | Share of eligible observation time when core Hub functions for device communication were confirmed available | Historical windows |
| **Component availability** | Independent availability of MQTT, API, or Web from the same samples | Historical windows |
| **Device connectivity** | Whether the Hub has a recent device keepalive (`last_seen`) | Historical windows (device monitor) |

A working web session does **not** prove that MQTT or device communication is available. A healthy API does **not** prove that MQTT is working. Operators should read Core Hub availability together with MQTT and API component availability before troubleshooting devices.

### Why the web UI can stay up during an MQTT outage

The browser talks HTTPS to Nginx → Web / API. Device traffic uses MQTT (Mosquitto). Those paths are independent. Stopping Mosquitto disconnects devices while an already-loaded Hub tab can keep working until the next API call that depends on MQTT-backed data.

## Observation model (Hub)

Every `DBMAP_HUB_MONITOR_INTERVAL_S` seconds (default **60**), a background thread inside `hub-bootstrap` runs a monitor cycle that:

1. Probes API/database, Nginx, Web, MQTT, and OPA.
2. Optionally reads Docker runtime diagnostics for monitored Compose services (not Firmware Builder).
3. Persists a compact Hub sample **only on state transitions** (and after monitoring gaps), marked with `sample_mode: sparse`. Payload stores `states`, `last_success`, and `reasons` only — Docker inspect is not persisted in new samples; restart detection uses in-memory signatures and events.
4. On a monitoring gap (more than 120 seconds by default since the previous successful Hub check), records `monitoring_resumed` and inserts an explicit `unobserved` sample so hold attribution excludes Hub-down / PC-off time from availability percentages. Successful Hub checks keep this clock current even when no sample is written.
5. Records events on state transitions, monitoring gaps, and detected restarts.
6. Periodically deletes monitor rows older than retention (default **30** days for Hub samples, configurable **30–90**). A sparse timeline keeps its last boundary before the cutoff so a long-lived state can still be attributed inside the reporting windows.

The background cycle persists samples and events. Status requests calculate the full availability history on demand.

**Attribution compatibility:** sparse-era samples hold state until the next sample (including a pre-window carry-in sample so a stable state can cover a window with no interior samples). Legacy dense samples (no `sample_mode`) keep interval confirmation for long inter-sample gaps so historical Hub-down periods stay unobserved without rewriting rows. Older samples may still contain a `docker` blob in `components_json`; readers must tolerate its presence or absence.

The monitor runs inside the existing `hub-bootstrap` container. It does not add containers, databases, or external services.

### Component probes

| Component | Check | Priority | Notes |
| --- | --- | --- | --- |
| API | Monitor process is running the cycle | Core | Being able to run the check is the signal |
| Database | `SELECT 1` against the Hub SQLite database | Reachability | Failure marks database unhealthy |
| Nginx | HTTPS `GET` of the public `/health` path through the Nginx service | Reachability | Uses the configured public base path |
| Web | HTTPS `GET https://dbmap-web/health` | Reachability / Web availability | Application health, not merely container state |
| MQTT | Hub TLS session `is_connected()`; `$SYS` metrics are contextual | Core (device communication) | MQTT down does not block the API after startup |
| OPA | `GET /health?plugins=true` over HTTPS | Auxiliary | Used for degraded vs healthy; does not mark Core Hub unavailable |

Docker inspect (`StartedAt`, `RestartCount`, container health status) is diagnostic context only. It does not replace application probes. Container creation time is never treated as uptime. Compose `healthy` means the configured healthcheck passed; it does not prove external LAN reachability.

### Firmware Builder exclusion (strict)

**Firmware Builder never participates in Health and Availability monitoring.**

It does not:

- appear in the monitoring component list
- affect health state or score
- affect Core Hub or component availability calculations
- generate monitoring events (including restart detection)
- change the overall monitoring result when stopped, unhealthy, restarting, or absent

The optional builder remains available for firmware builds and for the separate Hub Components / details probes used by admins. That surface is not the Health and Availability score of record.

### Overall Hub health states

| State | Score | Rule |
| --- | --- | --- |
| `healthy` | 100 | Reachability path (API, database, Nginx, Web) ok; MQTT ok or disabled; OPA ok |
| `degraded` | 70 | Reachability ok, but MQTT and/or OPA failed. Score is never 100 while MQTT is confirmed failed |
| `unhealthy` | 0 | API, database, Nginx, or Web failed |
| `unknown` | `null` | Checks could not produce a defensible state |

Scores use the fixed mapping above (100 / 70 / 0). There are no arbitrary per-component weights.

When health is not `healthy`, the summary includes a plain-language `health.reason` that names the failing component (for example that the MQTT broker is offline and devices cannot communicate, even if the web session still works).

### Overall Hub availability states (per sample)

These feed **Core Hub availability** history:

| State | Meaning | Counts in Core Hub % |
| --- | --- | --- |
| `available` | Reachability and MQTT ok (OPA ok) | Available |
| `degraded` | Reachability and MQTT ok; OPA failed | Available (authorization impaired, devices can still talk) |
| `unavailable` | Reachability failed **or** MQTT failed while monitoring could still record a sample | Unavailable |
| `unobserved` | Monitoring was not running; excluded from the percentage denominator | Excluded |

**Change from earlier Phase 1 wording:** MQTT failure used to be stored as `degraded` and therefore counted as available, which could show `100.0%` Core Hub availability during a confirmed MQTT outage. MQTT failure is now stored as `unavailable` so the outage contributes to confirmed unavailable duration. The metric is labeled **Core Hub availability** so it is not mistaken for “web was reachable.”

Planned maintenance is not implemented in Phase 1.

## Availability percentages

### Core Hub availability

For each window (24 hours, 7 days, 30 days):

```text
Core Hub availability (%) =
  confirmed available-or-OPA-degraded time / eligible observation time × 100
```

Where:

- **Confirmed available time** includes sample intervals in `available` or `degraded` (OPA-only).
- **Confirmed unavailable time** includes intervals where overall state was `unavailable` (reachability or MQTT failure).
- **Eligible observation time** is confirmed available time plus confirmed unavailable time.
- **Unobserved gaps** are excluded from the denominator and never counted as uptime or downtime.
- Missing evidence is never treated as proof of health.

If eligible time is below **10 minutes**, the API returns `percent: null` with `insufficient_data: true` instead of a fabricated percentage.

If eligible time is under **50%** of the window length, `low_coverage: true` is set. Multi-day windows (7d / 30d) also set `score_ready: false` in that case: UIs must not present a bare “100% over 7 days” after only a few hours of observation. They should show an incomplete / observed-duration label instead. The 24h window may still show an early annotated percentage (for example `100% of 3h observed`) while coverage is building.

When any confirmed unavailable seconds exist, the displayed percentage is never rounded up to a bare `100.00`.

Presentation includes, where practical: percentage, eligible duration, confirmed unavailable duration for that metric, Hub-not-running duration, and incomplete / low-coverage labels. **Hub not running** is time the monitor wrote no samples, which looks the same whether the Hub container stopped or the computer was off. That time is left out of the percentage. **Unavailable duration always names what was unavailable** (for example “MQTT unavailable (24h)” vs “Core Hub unavailable (24h)”). MQTT unavailable duration is not total Hub downtime unless reachability also failed.

### Component availability (MQTT / API / Web)

Independent windows are computed from per-sample component states in the same Hub samples:

| Label | Source state |
| --- | --- |
| **MQTT availability** | `mqtt` probe |
| **API availability** | `api` probe |
| **Web availability** | `web` probe |

Unknown or missing component states are **unobserved** for that component and are not counted as available or unavailable. MQTT and API can therefore disagree with Web for the same wall-clock interval.

### Gaps after restart

The monitor updates one checkpoint per successful Core check in the same transaction as any sample and event writes. This checkpoint is separate from availability history. After either a graceful restart or a crash, the next Core check uses the checkpoint, when valid, as the last confirmed observation. The interval before the next check is unobserved, even for a brief restart. The existing monitoring-resumed event threshold stays in effect. A missing or invalid checkpoint, including one older than the latest Hub sample, falls back to the latest sparse sample and the existing gap threshold. The fallback can include time when successful checks wrote no samples. Before the next successful check, the Core and public availability summaries stop crediting sparse coverage at the last confirmed check if the worker is stale or has yet to check after startup. This read-time gap is not written to history.

A detected `service_restart_detected` event (Docker `StartedAt` / `RestartCount` change) is diagnostic only. It does **not** invent an unavailable duration. Confirmed outage duration comes only from probe-based sample states. Restart event `previous_state` / `new_state` use `running` → `restart_detected` so they are not confused with health scores or availability enums.

A future Regional Hub can observe Local Hub reachability externally and provide a more authoritative view. Phase 1 only prepares local event/summary shapes for that later sync.

## Persistence (Hub)

SQLite tables (same database as the Hub):

- `hub_monitor_samples` — compact periodic snapshots
- `hub_monitor_events` — state transitions and diagnostic events

Retention cleanup deletes only those tables. Device, observation, ownership, and other application data are untouched. Secrets and raw logs are not stored in monitoring history.

Useful Hub event types:

- `state_changed` — per-component probe state (for example MQTT `ok` → `offline`)
- `health_changed` — overall Hub health (`healthy` / `degraded` / `unhealthy`)
- `availability_changed` — overall Core Hub availability sample state
- `monitoring_resumed` — unobserved gap closed
- `service_restart_detected` — inferred from Docker runtime signature; not an outage duration

Each event carries UTC `occurred_at` and `observed_at`, component id, previous/new state, short reason, source (`local_monitor`), and certainty (`confirmed` or `inferred`).

`state_changed` and `health_changed` / `availability_changed` describe different layers. A health score field of `0` is not the same as a component state of `offline`; UI labels should keep those distinct.

## API and UI (Hub)

`GET /api/v1/hub/status` (existing auth: bearer token + OPA `hub.status.read`) includes a `monitoring` object with:

- current health state, score, reason, and per-component summary (Firmware Builder omitted)
- Core Hub availability windows for 24h / 7d / 30d
- independent MQTT / API / Web availability windows
- last successful checks
- last relevant state change
- observation gaps / insufficient-data warnings

Recent event history is **not** embedded in every status refresh. Open `GET /api/v1/hub/monitoring/events?limit=15` (same `hub.status.read` action) when the Hub tab expands that section, or when `hub_summary` prints the human-readable event list. Default limit is 15 (max 50).

The Hub tab and `scripts/hub_summary.py` both use those shared calculations. There is no second scoring algorithm in the CLI.

## Configuration (Hub monitor)

| Variable | Default | Notes |
| --- | --- | --- |
| `DBMAP_HUB_MONITOR_ENABLED` | `true` | Set `false` to disable the background thread |
| `DBMAP_HUB_MONITOR_INTERVAL_S` | `60` | Full probe interval in seconds |
| `DBMAP_HUB_MONITOR_GAP_THRESHOLD_S` | `120` | Infer a gap when elapsed time between successful Hub observations exceeds this threshold |
| `DBMAP_HUB_MONITOR_RETENTION_DAYS` | `30` | Clamped to 30–90; raise via env if longer Hub sample history is needed |
| `DBMAP_PUBLIC_API_MONITOR_ENABLED` | `false` | Probe the public Hub HTTPS health endpoint |
| `DBMAP_PUBLIC_MQTT_MONITOR_ENABLED` | `false` | Probe an authenticated MQTT session through the public endpoint |

### Public API and Public MQTT

These optional checks run independently every 60 seconds in the Hub process. The Hub tab displays their state, last check, reason, transitions, and 24h, 7d, and 30d availability with eligible observation time. API clients can read `monitoring.public_components` in `GET /api/v1/hub/status`. Public checks are informational. Core Hub, Web, and Device Monitor continue using their existing inputs and schedules.

- Public API appends `/health` to `DBMAP_PUBLIC_HUB_URL`, verifies the configured hostname with the existing CA, and requires a successful response identifying `hub-bootstrap`. It sends no API credentials.
- Public MQTT uses `DBMAP_PUBLIC_MQTT_HOST`, `DBMAP_PUBLIC_MQTT_PORT`, the existing MQTT CA and regular Hub login, and verified TLS. It requires a successful MQTT 3.1.1 CONNACK for a unique, clean client session. It disconnects without subscribing, publishing, or reconnecting. DynSec client-ID restrictions or rejected credentials can block this check and are surfaced as failure reasons.

Public probes use the same sparse observation store as Core Hub. The separate
`public_monitor_samples`, `public_monitor_events` and
`public_monitor_checkpoints` tables mirror the `hub_monitor_*` tables.
Public checkpoints are keyed by Hub and component, so a completed API check
cannot advance MQTT coverage. A stable cycle updates only its checkpoint.
Both families retain the last sparse boundary before the cutoff, and use
committed observations to exclude gaps and restart downtime. A short restart
with an unchanged state creates gap boundaries without a resume event.
A delayed public probe stays on its own worker.

Upgrades create the new public tables empty. The former
`public_monitor_transitions` table, if present, stays intact and is unused by
the new build. Public availability accumulates from the first new observation.
Core and Device Monitor history carry through the upgrade.

These results describe reachability from the Hub's network. Router NAT loopback and split DNS can make the result differ from what an internet client sees. The probes use the Hub resolver and do not compare DNS answers to an independently measured WAN address. Use an off-LAN client to validate externally visible reachability.

## Limitations (Hub)

For a separately approved history reset, see [Monitoring history recovery](monitoring-history-recovery.md).

- Local monitoring cannot prove external client reachability beyond the Hub’s own probes.
- Complete Hub outages appear as unobserved gaps after recovery, not as precisely timed downtime.
- Hub API and database status in the legacy component list remain tautological for a successful status request; the monitoring section is the historical/current assessment surface.
- Device-to-Hub correlation of MQTT disconnects with Hub-observed MQTT outages is **not implemented** yet. Timestamps and distinguishable event sources are preserved so a later phase can match them without treating same-minute events as proven causation.

---

## Device monitoring

### Concepts

| Metric | Meaning | Source |
| --- | --- | --- |
| **Connectivity / availability** | Whether the Hub has confirmed a recent keepalive | `reported_state.last_seen_at` vs online window (D0) |
| **Health** | Rule-based condition from device diagnostics | Optional `diag` on health/keepalive (D1 device / D2 Hub) |
| **`nodes.availability`** | Operational mode (`SERVICE`, `OPERATIONAL`, …) | Unchanged protocol field — **not** an uptime percentage |

Online means `last_seen_at` is within `DBMAP_DEVICE_ONLINE_WINDOW_S` (default **90** seconds). That same window drives the Devices list `status` (`online` / `offline`).

### D0 — Hub-observed availability

Device Monitor runs every 30 seconds by default on its own background thread in `hub-bootstrap`. Its schedule and persistent checkpoint are independent of the legacy Hub Monitor, so it continues when that monitor is disabled or its infrastructure probes fail. Device checks sample connectivity from reported state; they do not run Hub probes, depend on Operations, or update Hub continuity.

Each Device Monitor check:

1. For every **active** node that has been seen at least once, compute `available` or `unavailable` from `last_seen_at`.
2. Persist a sample only on **state transitions** (and when a Device Monitor gap is detected).
3. On a process restart or delayed Device Monitor cycle, insert an `unobserved` marker so time without device sampling is excluded from the availability denominator. A detected gap does not by itself confirm device downtime.
4. Skip **deactivated** nodes (excluded from further sampling; not counted as downtime).
5. Prune samples, monitor events, and retained device diagnostics by the configured retention days and existing per-device caps.

The Device Monitor checkpoint is committed with its samples/events. On the first cycle after upgrade, an existing Hub Monitor checkpoint seeds Device Monitor continuity when available; subsequent cycles use only the Device Monitor checkpoint. The independent worker also performs retention cleanup if Device Monitor sampling is disabled.

Availability windows (24h / 7d / 30d) reuse the Hub honesty rules: minimum eligible time, `low_coverage`, and `score_ready`.

### D1 — on-device RAM diagnostics

Implemented in `node_common` (firmware):

- Optional `diag` object on **health/keepalive** only (not hello).
- Counters (saturated; RAM only; clear on reboot):

  | Field | Meaning |
  | --- | --- |
  | `wifi_disconnects` | STA disconnect observations (one per down transition; duplicates while down ignored) |
  | `mqtt_disconnects` | Transitions from MQTT connected → disconnected |
  | `mqtt_reconnects` | Successful MQTT CONNECT after a prior disconnect (not the first boot connect) |
  | `mqtt_connect_attempts` | MQTT connect/reconnect starts |
  | `mqtt_connect_failures` | Connect/reconnect endings that did not reach connected |
  | `desired_failures` | Desired HTTPS checks that failed |

- One-shot `last_reset_reason` after boot, cleared after a successful health publish.
- Anomaly-only nested `diag.mem` (RAM; sampled at health/keepalive attach):

  | Field (only when anomalous) | Meaning |
  | --- | --- |
  | `free_heap` / `min_free_heap` | Combined heap below compile-time floor |
  | `free_internal` / `min_free_internal` | Internal DRAM below floor |
  | `largest_internal` | Contiguous internal block below floor (alone or with `free_internal` when fragmented) |
  | `stack_hwm_min` | Minimum FreeRTOS stack HWM among registered tasks below floor |

  No healthy-state memory fields. No `memory_health` wire field. Unchanged
  anomalies are fingerprint-deduped until the condition clears or worsens
  across a 4 KiB quantum. Hub D2 does **not** treat `mem` as a counter
  field or health rule (latest snapshot only; no dedicated clear/replace
  event). Clearing is therefore undefined at the Hub: a later counter-only
  `diag` may drop `mem` from `last_diag_json`, while a health with no `diag`
  leaves the previous snapshot untouched.
- `protocol_version: 1` on health/keepalive.
- Tunables default from Kconfig (`DBMAP_DIAG_*`). Future Desired `payload.diag` + `config_version` bump can override them via `dbmap_diag_apply_desired_payload()` once config sync exists. Memory floors are compile-time constants in `sp_diag_memory.h`.

#### Network retry backoff (`node_common`)

Independent exponential backoff state for MQTT, Wi-Fi, and HTTPS Desired (see `dbmap_backoff`):

| Path | Initial | Max | Reset rule |
| --- | --- | --- | --- |
| MQTT | 1s (Kconfig) | 60s | After connection stays up for `DBMAP_BACKOFF_MQTT_STABLE_MS` (default 30s) |
| Wi-Fi | 1s | 60s | On GOT_IP; every failed join advances backoff (incl. `NO_AP_FOUND`) |
| HTTPS Desired | 1s | 60s | On successful request; at most 3 transient retries per poll |

- ~20% jitter; overflow-safe doubling; `in_flight` prevents concurrent reconnects for the same path.
- MQTT disables esp-mqtt auto-reconnect and schedules non-blocking `esp_timer` oneshots. MQTT does not attempt reconnect while Wi-Fi has no IP.
- Wi-Fi reconnect uses the same pattern (no immediate `esp_wifi_connect` storm).
- HTTPS retries only transient failures (timeouts / selected 5xx / 429); 401/403/404 are not retried. Delays run only in the Desired task — not in MQTT/audio callbacks.
- Diagnostics and retries are best-effort and must not block primary device functions. Counters stay in RAM (no flash writes).

Host unit tests for the pure backoff math: `firmware/components/node_common/test_host/test_backoff.c`.

Hardware validation still required for: sustained broker outage, Wi-Fi AP loss, Desired Hub downtime under load, and OTA pre-reboot status PUBACK / post-reboot confirmation when MQTT is flaky.

### OTA install vs confirmed boot

OTA reporting uses the existing `status` topic (`message_type: ota_update`). The Hub distinguishes:

| Phase | Meaning |
| --- | --- |
| `ota_install_reported` | New image written and selected for next boot; device still running the old version |
| `ota_boot_confirmed` | Device is running the target version after reboot (status and/or hello correlation) |
| `ota_boot_mismatch` | Pending target did not match the version that came up |

Diag `last_reset_reason` (for example `SW`) remains separate from `reboot_reason: ota`.

#### OTA reboot pending (expected teardown)

After the pre-reboot install report and bounded MQTT PUBACK wait, firmware enters
an explicit **OTA reboot pending** state (`dbmap_lifecycle_enter_ota_reboot_pending`):

1. Wi-Fi and MQTT cancel reconnect scheduling and do not start new reconnect attempts.
2. MQTT may request a quick disconnect; teardown is logged but not counted as a
   new connectivity incident.
3. Diagnostic counters (`mqtt_disconnects`, `mqtt_reconnects`,
   `mqtt_connect_attempts`, `mqtt_connect_failures`, `wifi_disconnects`) do not
   increment for that expected teardown.
4. Cleanup is capped at **500 ms**; reboot proceeds regardless.

OTA status is published on each node's `…/status` topic (Output and Ear). The Hub
MQTT listener must subscribe to both `esp-output/local/+/status` and
`esp-ear/local/+/status`; without the Ear subscription, install/boot reports are
dropped and only `reset_reported` (e.g. `SW` from `esp_restart`) may appear.

OTA Hub events (`ota_install_reported` / `ota_boot_confirmed`) stay separate from
those counters. An OTA update does not increment MQTT/Wi-Fi counters merely because
OTA occurred; genuine failures before reboot-pending remain visible.

**Limitation:** errors that occur *before* reboot-pending (during download, TLS,
or Desired HTTPS) are still counted normally. The gate is not applied by
proximity to an OTA timestamp alone.

### D2 — Hub ingest and health rules

On each reported health/keepalive/hello apply:

1. Parse optional `diag` (malformed/missing is ignored; old firmware keeps working).
2. Record **deltas** vs the last retained diag snapshot (`device_diag_state.last_diag_json`), not vs the previous MQTT payload. Hello omits `diag`; using the payload alone previously re-counted full cumulative totals into `*_burst` rows (for example inflated `mqtt_reconnect_burst` counts).
3. Aggregate same-type bursts within `DBMAP_DEVICE_DIAG_AGGREGATE_WINDOW_S` into one retained row (`event_count` is the sum of positive deltas in that window).
4. Enforce a **hard cap of 30** retained diag events per device (FIFO; one device never blocks another).
5. Assess health and emit `health_changed` on `device_monitor_events` when the state changes (`source: hub_assessed`).

| Health state | Score | Rule |
| --- | --- | --- |
| `unknown` | `null` | Offline / no recent heartbeat, or no diag evidence yet |
| `healthy` | 100 | Online; diag seen (or empty after evidence); no elevated counters; RSSI not below threshold |
| `degraded` | 70 | Online with wifi/mqtt disconnect/reconnect/failure/desired activity, or RSSI below `DBMAP_DEVICE_DIAG_RSSI_DEGRADED_DBM` (`mqtt_connect_attempts` alone does not degrade) |
| `unhealthy` | 0 | Online with unexpected reset reason (`PANIC`, `TASK_WDT`, `INT_WDT`, `WDT`, `BROWNOUT`) |

Device-reported reconnect counts **inform health**, not the availability clock (avoid double-counting the same outage).

### D3 — UI

Device detail **Health and availability** shows:

- Health state/score/reason and last diag snapshot when present
- Observed connectivity and availability windows (with incomplete multi-day labels)
- Diag retention counter (`n / 30`)
- Expandable recent events with source tags: `[hub]` (Hub-observed / Hub-assessed) and `[device]` (device-reported diag)

### Future: device-to-Hub event correlation (not implemented)

A later phase may correlate device-reported MQTT disconnect/reconnect diag events with Hub-observed MQTT `state_changed` outages using timestamps and distinct sources. This phase only preserves those fields and keeps the event streams distinguishable. Same-minute timestamps must not be treated as proven causation.

### Device API

- `GET /api/v1/nodes/{node_id}` includes `monitoring` (detail only).
- `GET /api/v1/nodes/{node_id}/monitoring/events?limit=15` returns merged Hub-observed and device-reported events (newest first).

### Device configuration

| Variable | Default | Notes |
| --- | --- | --- |
| `DBMAP_DEVICE_ONLINE_WINDOW_S` | `90` | Seconds; clamped 30–600 |
| `DBMAP_DEVICE_MONITOR_ENABLED` | `true` | Enable Device Monitor sampling and gap recording |
| `DBMAP_DEVICE_MONITOR_INTERVAL_S` | `30` | Device connectivity sampling interval in seconds (5–3600) |
| `DBMAP_DEVICE_MONITOR_RETENTION_DAYS` | `90` | Clamped to 30–90 |
| `DBMAP_DEVICE_MONITOR_MAX_SAMPLES_PER_DEVICE` | `500` | Soft cap for availability samples |
| `DBMAP_DEVICE_MONITOR_MAX_EVENTS_PER_DEVICE` | `500` | Soft cap for Hub-observed monitor events |
| `DBMAP_DEVICE_DIAG_ENABLED` | `true` | Skip diag ingest when `false` |
| `DBMAP_DEVICE_DIAG_MAX_EVENTS_PER_DEVICE` | `30` | Hard ceiling; clamped 5–30 |
| `DBMAP_DEVICE_DIAG_RSSI_DEGRADED_DBM` | `-80` | Hub health rule threshold |
| `DBMAP_DEVICE_DIAG_AGGREGATE_WINDOW_S` | `3600` | Merge same-type bursts within this window |

Device-side diagnostic defaults use on-device Kconfig / future Desired `payload.diag`, not these Hub `.env` values.

### Device persistence

- `device_monitor_samples` — sparse connectivity timeline
- `device_monitor_events` — Hub-observed transitions + Hub-assessed `health_changed`
- `device_diag_events` — device-reported bursts/resets (≤30/device)
- `device_diag_state` — latest diag snapshot and health

## Related surfaces

- Component list and on-demand details: Hub tab Components section / `GET /api/v1/hub/components/...` (may still list Firmware Builder for admin ops; that list is not HA scoring)
- Compose healthchecks: process/dependency checks used by Docker, not the historical score source of record
- Device detail `Last seen` / status: same online window as D0 availability
- Protocol `diag` fields: [protocol.md](./protocol.md)

### EAR runtime snapshot

Package 8 appends counters to the existing health/keepalive diagnostic object
after the platform and legacy EAR diagnostic rules have run. It reuses the
latest diagnostic snapshot and Device page. The new fields have no device
diagnostic event, connectivity sample, health assessment, or Observation
persistence rule. See [EAR runtime diagnostic snapshot](protocol.md#ear-runtime-diagnostic-snapshot)
for field and publish outcome semantics.
