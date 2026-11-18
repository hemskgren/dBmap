# Sensor Platform connectivity and MQTT recovery

**Status:** Stages 1–4 are implemented in the working tree. Hub behavior is unchanged. This change is not committed.  
**Branch:** `firmware/ae-sig-capture-as001-calib`. Unrelated acoustic-capture work on this branch was left untouched.  
**Baseline:** current tree at review time. Deployed images such as EAR firmware `0.8.17` are not this tree. `sdkconfig.defaults` records series bases `0.8.0` (Ear) and `0.7.0` (Output). Catalog builds bump `CONFIG_DBMAP_FIRMWARE_VERSION`. Sections 1–10 describe the pre-Stage-1 source. Stage records below describe what landed. ESP-IDF on this host: **v5.4.4**.

**Products in scope:** Ear and Output, the only firmware products on Sensor Platform today. Future products (N.O.S.E., MON) do not exist in tree; they must consume the same platform recovery. The Ear simulator is a laptop MQTT client and does not use this firmware stack.

---

## Stage 1 record

Stage 1 closes the stall holes in platform Wi-Fi and MQTT recovery. It does not add `sp_connectivity`, interface restart, or a device reboot. Wi-Fi and MQTT lifecycle stay in Sensor Platform. Ear and Output hello/health loops are unchanged. Backoff stays 1 s, doubling to 60 s, with ±20% jitter.

### Design decisions

**1. Verified MQTT progress.** QoS in the current publishers: hello QoS 1 (`sp_publish_hello`), health QoS 0, keepalive QoS 0, status QoS 1, Ear observation QoS 1, Output ack QoS 1. Verified progress is only:

- `MQTT_EVENT_PUBLISHED` whose `msg_id` was recorded at publish time for a QoS 1 enqueue (`msg_id > 0` and `qos > 0`), or
- inbound `MQTT_EVENT_DATA`.

`CONNECTED` starts the deadline and is not itself verified delivery. A successful enqueue returns `ESP_OK` and does not refresh the deadline. QoS 0 calls `sp_mqtt_progress_note_qos0`, which does not record an id and does not refresh the deadline. `SUBSCRIBED`, `ERROR`, and `DISCONNECTED` are local client events and are not progress. A PUBACK whose id was never tracked returns false and leaves the deadline unchanged. A second PUBACK for an id already consumed does the same. Up to 8 ids are outstanding; a 9th QoS 1 publish drops the oldest id, and a later PUBACK for that dropped id does not count. Each accepted PUBACK removes only that id. PINGRESP is not delivered as `MQTT_EVENT_DATA`, so the MQTT keepalive is not progress. Hello is QoS 1, so a session that is actually delivering application traffic still produces PUBACKs. Any tracked QoS 1 id counts, including observation and ack PUBACKs: that is delivery confirmation, and it does not add observation-specific recovery policy.

The deadline is `SP_MQTT_PROGRESS_LIMIT_MS` (90000), the existing Hub `DBMAP_DEVICE_ONLINE_WINDOW_S`. It is set at `CONNECTED` to now + 90 s and moves forward only on verified progress.

**2. Observation independence.** Recovery does not call Ear observation code, does not read whether an episode is open, and does not change audio acquisition. The recovery task `sp_net` is priority 3. The mic task is priority 5 and the observation task is priority 4, so recovery does not preempt capture. Clearing `s_connected` uses the existing `sp_mqtt_is_connected()` checks: observation publish and the product hello/health loops skip network work while the latch is down. Those loops were not rewritten.

**3. Timer callback safety.** `esp_timer` callbacks for Wi-Fi reconnect, the Wi-Fi in-flight watchdog, MQTT reconnect, the MQTT connect watchdog, and the MQTT progress deadline only call `sp_net_dispatch_post`. That sets one bit under a spinlock and notifies `sp_net`. `esp_wifi_connect`, `esp_wifi_disconnect`, `esp_mqtt_client_reconnect`, `esp_mqtt_client_stop`, `esp_mqtt_client_destroy`, `esp_mqtt_client_start`, and broker DNS run on `sp_net`. The stable-backoff timer callback only calls `sp_backoff_poll_stable`, which does not call the driver. This task is a dispatcher for those existing operations. It does not publish hello, stop the Wi-Fi interface, or reboot.

**4. Independent progress monitoring.** Stage 1 does not add the 60 s supervisor log, and that log would not be a watchdog. What exists today:

- The shared `esp_timer` task can still run Wi-Fi and MQTT timers while `sp_net` is inside a driver call, because those callbacks no longer call the driver.
- The product hello/health loops call `sp_mqtt_request_reconnect()` only when `sp_mqtt_is_connected()` is false. After a progress-deadline expiry the latch is cleared, so the next loop iteration requests reconnect. That loop does not notice a `sp_net` task blocked inside `esp_mqtt_client_stop` or DNS.
- `sdkconfig.defaults` has no task watchdog covering `sp_net`.

Remaining Stage 1 risk: if `sp_net` blocks indefinitely inside stop or name resolution, recovery stops even though unrelated timers still run. If `xTaskCreate` for `sp_net` fails, later posts are no-ops; the initial boot connect still runs on `app_main`. Nothing in Stage 1 detects that stuck worker. Stage 2's supervisor is the place for an independent check, and it is not in this change.

### What Stage 1 changed

Wi-Fi (`wifi_reconnect_sm.c`, `net_wifi.c`):

- `sp_wifi_reconnect_sm_on_reconnect_arm_failed` keeps the reconnect reservation when `esp_timer_create` or `esp_timer_start_once` fails. `defer_reconnect_arm` posts `SP_NET_OP_WIFI_ARM_TIMER` after the existing backoff delay. The reservation is not released.
- In-flight watchdog expiry sets `allow_late_ip` for that connect generation and sets `disconnect_before_next_connect`. The backoff timer returns `SP_WIFI_SM_DISCONNECT_FOR_RETRY`. The worker calls `esp_wifi_disconnect`. The matching `STA_DISCONNECTED` returns `SP_WIFI_SM_START_CONNECT` with `skip_backoff`, so the intentional disconnect is not a second failure. The disconnect wait reuses the existing 30 s watchdog and bumps `watchdog_generation` only. If `esp_wifi_disconnect` fails, `on_disconnect_api_failed` connects directly on the worker. If the address is already held, `on_disconnect_skipped` drops the disconnect intent.
- If the watchdog timer itself cannot be created or started, the glue treats that as the watchdog firing, because the 30 s bound cannot be enforced.
- `IP_EVENT_STA_LOST_IP` is handled in `on_lost_ip`. The glue clears the `GOT_IP` event bit before the state machine runs, so `sp_wifi_has_ip()` cannot stay true after the loss. The state machine sets `accept_renewed_ip` and schedules reconnect with disconnect-before-connect. A later `GOT_IP` is accepted. A stale `GOT_IP` from the previous lease is indistinguishable from a renewal; both are accepted once.
- `sp_backoff_config_wifi` sets `stable_reset_ms` to the existing MQTT stable window (`SP_BACKOFF_MQTT_STABLE_MS`, default 30 s). `note_success` on `GOT_IP` no longer clears the failure count immediately. A oneshot calls `sp_backoff_poll_stable` after that window.
- `esp_wifi_set_ps(WIFI_PS_NONE)` runs on every accepted `GOT_IP`.

MQTT (`hub_mqtt.c`, `mqtt_progress.c`):

- Progress expiry clears `s_connected`, counts one disconnect failure, and recreates the client on `sp_net` (`mqtt_recreate`: re-resolve, stop, destroy, init, start). `.local` may be replaced with the resolved address, and the TLS verify name stays the configured host. Public DNS names stay DNS names.
- `s_recreate_active` makes the `DISCONNECTED` handler return without another failure count. In ESP-IDF 5.4, `esp_mqtt_client_stop` waits until the MQTT task has exited, and that task dispatches events before it sets the stopped bit.
- Connect watchdog expiry and escalate-after-3 still end in recreate, and that recreate runs on `sp_net`.
- Session keepalive stays 60 s. The proposal's 30 s keepalive is a new timeout and is not required to clear the false-connected latch, so it is deferred.
- If recreate's start fails, the progress path counts a connect failure and `schedule_reconnect` uses the existing backoff.

### Files and functions

| File | Role |
| --- | --- |
| `firmware/platform/node_common/wifi_reconnect_sm.c` | `on_disconnected`, `on_reconnect_timer`, `on_connect_accepted`, `on_connect_start_failed`, `on_got_ip`, `on_watchdog`, `on_boot_wait_timeout`, `prepare_ota_reboot`, `on_lost_ip`, `on_reconnect_arm_failed`, `on_disconnect_skipped`, `on_disconnect_api_failed` |
| `firmware/platform/node_common/include/sp_wifi_reconnect_sm.h` | `SP_WIFI_SM_DISCONNECT_FOR_RETRY`, `defer_arm_to_worker`, `skip_backoff`, lost-IP and arm-failure prototypes |
| `firmware/platform/node_common/net_wifi.c` | `arm_reserved_reconnect_timer`, `arm_watchdog_timer`, `apply_sm_result`, `on_lost_ip`, `wifi_worker_connect`, `wifi_worker_disconnect_for_retry`, `wifi_worker_arm_timer`, `on_ip` |
| `firmware/platform/node_common/backoff.c` | `sp_backoff_config_wifi` stable window |
| `firmware/platform/node_common/mqtt_progress.c`, `include/sp_mqtt_progress.h` | Progress tracker |
| `firmware/platform/node_common/hub_mqtt.c` | `rebuild_broker_uri`, `try_reconnect`, `mqtt_recreate`, `mqtt_on_progress_expired`, timer callbacks, `on_connected`, `on_disconnected`, `MQTT_EVENT_DATA` / `PUBLISHED`, `sp_mqtt_publish_json`, `sp_mqtt_publish_json_wait` |
| `firmware/platform/node_common/net_dispatch_sm.c`, `net_dispatch.c`, `include/sp_net_dispatch.h` | Bit dispatch and `sp_net` task |
| `firmware/platform/node_common/CMakeLists.txt` | Adds the three new sources |
| `firmware/platform/node_common/test_host/test_wifi_reconnect_sm.c` | Late IP, lost IP, arm failure, disconnect-wait |
| `firmware/platform/node_common/test_host/test_backoff.c` | Wi-Fi stable reset at 30 s |
| `firmware/platform/node_common/test_host/test_mqtt_progress.c` | PUBACK ids, QoS 0, inbound DATA, expiry |
| `firmware/platform/node_common/test_host/test_net_dispatch_sm.c` | `request` does not run the operation |
| `firmware/scripts/run_firmware_fast_gate.sh` | Host section for the four tests, inserted before the existing AE host tests |

### Tests and results

Host binaries, each printed `ok` and exited 0 (`-std=c11 -Wall -Wextra -Werror -O2`, include `firmware/platform/node_common/include` and `test_host`):

- `wifi_reconnect_sm.c` + `test_host/test_wifi_reconnect_sm.c`
- `backoff.c` + `test_host/test_backoff.c`
- `mqtt_progress.c` + `test_host/test_mqtt_progress.c`
- `net_dispatch_sm.c` + `test_host/test_net_dispatch_sm.c`

`./firmware/scripts/run_firmware_fast_gate.sh --host-only`  
`pass=31 fail=0 skip=0 duration_s=14 mode=host-only`  
`RESULT: PASS`

`source /home/christer/esp/esp-idf/export.sh && ./firmware/scripts/run_firmware_fast_gate.sh`  
`pass=41 fail=0 skip=0 duration_s=142 mode=fast`  
`RESULT: PASS`  
ESP-IDF 5.4. The modularity step builds isolated Platform audio archives. It does not compile `node_common`.

Because those archives do not compile the changed MQTT and Wi-Fi sources, the component library was compiled separately, isolated from the project `sdkconfig`, and not flashed:

`idf.py -B /tmp/dbmap-stage1-node -D SDKCONFIG=/tmp/dbmap-stage1-node/sdkconfig -D SDKCONFIG_DEFAULTS=sdkconfig.defaults set-target esp32s3 && ninja -C /tmp/dbmap-stage1-node esp-idf/node_common/libnode_common.a`

Exit 0. The log ends at linking `esp-idf/node_common/libnode_common.a`. No compiler warning lines for these sources.

### Remaining limitations

- Product hello/health backoff can still grow to 120 s, which is longer than the Hub 90 s window (finding F8). Stage 2 owns that loop.
- MQTT session keepalive remains 60 s. `docs/protocol.md` describes 30 s. The session value was not changed.
- No `sp_connectivity` supervisor, no 60 s diagnostic line, no new diag fields (finding F11).
- No `esp_wifi_stop` / interface restart (Stage 3). No reboot and no NVS reboot budget (Stage 4).
- A `sp_net` task blocked inside stop or DNS is not detected. See decision 4.
- `sp_net_dispatch_post_after` stores one delayed operation. Stage 1 uses it only for a failed Wi-Fi reconnect-timer arm.
- A missing `STA_DISCONNECTED` after a successful `esp_wifi_disconnect` is covered by the existing 30 s watchdog, which then starts connect with `skip_backoff`.
- After `LOST_IP`, the next `GOT_IP` is accepted whether it is a renewal or a late event from the old attempt.
- `.local` is re-resolved when the client is recreated (escalate or progress loss), not on every TCP reconnect.
- Devices already stuck on an older image stay stuck until they are flashed or power-cycled. This change does not reboot the fleet.

### Stage 4

See **Stage 4 record** below: 60-minute no-progress reboot (no Observation END), no NVS budget.

Stage 3 (Wi-Fi interface recovery) is recorded after Stage 2. Product hello/health loops remain unchanged.

---

## Stage 2 record

### Inspection vs Stage 1 report

Working-tree Stage 1 matched the Stage 1 record (Wi-Fi SM, dispatch, MQTT progress, recreate on `sp_net`). Differences addressed in Stage 2:

- Review findings **M1/M2/M3** were still open in the tree; they are required fixes before trusting progress under load.
- Proposal Stage B also moved hello/health into the supervisor. This Stage 2 delivery implements the **independent supervisor and stall detection** only. Product hello/health intervals and loops are unchanged (explicit task constraint).
- ESP-IDF in this environment is **5.4** (export.sh). Gate and `node_common` builds used that toolchain. A separate 5.4.4 pin was not present on the host; no tests were skipped for version skew beyond noting 5.4.

Shared entry points (Ear and Output): `sp_wifi_start`, `sp_mqtt_start` in `node_common`. Both call `sp_connectivity_start()` and `sp_net_dispatch_start()`.

### M1 / M2 / M3 resolution

**M1.** `hub_mqtt.c` takes `s_progress_mu` (FreeRTOS mutex) around every read/write of `s_progress`. The mutex is never held across `esp_mqtt_client_publish`, stop/destroy/start, or DNS. Host-testable logic stays in lock-free `mqtt_progress.c`.

**M2.** `sp_mqtt_progress_on_published` parks an unknown PUBACK in an early-ack ring while connected (`SP_MQTT_PROGRESS_PUB_PARKED_EARLY`). `sp_mqtt_progress_note_qos1` consumes a matching early ack and only then calls `mark_verified` (`SP_MQTT_PROGRESS_NOTE_VERIFIED_EARLY`). Enqueue alone never verifies. Host test `test_puback_before_note_qos1` covers the race order.

**M3.** `arm_oneshot` returns `bool`. Failures on reconnect, progress, and connect-watchdog timers call `sp_connectivity_note_timer_arm_failed` and `sp_net_dispatch_post_after` with at least `SP_CONN_TIMER_RETRY_MIN_MS` (1 s). The supervisor bounds retries with `SP_CONN_TIMER_RETRY_MAX` (5). Stable-timer failure is recorded but does not drive a tight loop.

### Supervisor architecture

| Piece | Role |
| --- | --- |
| `connectivity_supervisor_sm.c` | Pure decisions from a snapshot (host-tested) |
| `connectivity_supervisor.c` | Task `sp_conn`, period `SP_CONN_SUPERVISOR_PERIOD_MS` (5 s) |
| `sp_net` | Remains the only task that runs blocking Wi-Fi/MQTT driver ops |
| Heartbeat | `sp_net` stamps `heartbeat_ms` at loop top and after each op |
| Last dispatch | Stamped only after an op runs |
| Pending age | Stamped when bits become non-zero |

The supervisor does **not** ask `sp_net` to report “I am healthy” as a separate message channel beyond those monotonic stamps. Idle MQTT backoff with `dispatch_pending == false` must not be classified as a stall even if the heartbeat looks old (worker blocked in `ulTaskNotifyTake`).

### Thresholds and rationale

| Constant | Value | Rationale |
| --- | --- | --- |
| `SP_CONN_PROGRESS_LIMIT_MS` | 90000 | Unchanged Stage 1 / Hub online window |
| `SP_CONN_WORKER_STALL_MS` | 30000 | Same scale as MQTT connect watchdog and Wi-Fi in-flight timeout |
| `SP_CONN_DISPATCH_STALL_MS` | 30000 | Same bound when heartbeat is fresh but pending never completes |
| `SP_CONN_SUPERVISOR_PERIOD_MS` | 5000 | Proposal tick; shorter than 30 s publish/watchdogs |
| `SP_CONN_TIMER_RETRY_MIN_MS` | 1000 | Existing backoff floor |
| `SP_CONN_TIMER_RETRY_MAX` | 5 | Prevents tight retry under persistent OOM |

### What Stage 2 recovers vs only detects

**Can recover (when worker heartbeat is fresh):** post `MQTT_PROGRESS` / `MQTT_RECONNECT`, retry failed MQTT timer arms via dispatch, surface stage transitions in logs and `sp_connectivity_get_status`.

**Detects only:** `WORKER_STALLED` / `DISPATCH_STALLED` when `sp_net` is blocked inside stop/DNS/connect. Stage 2 does **not** delete the worker, stop Wi-Fi, or reboot. That is the Stage 3/4 boundary.

### Diagnostics snapshot (`sp_conn_status_t`)

Wi-Fi IP bit, MQTT connected, MQTT progress age, stage + stage age, worker heartbeat age, last dispatch age, dispatch pending, last timer/init fail reason, supervisor/worker started flags. Transitions log at info; stalls at error. No 60 s healthy spam loop.

### Tests and builds (Stage 2)

Host (`-Wall -Wextra -Werror`): `mqtt_progress`, `connectivity_supervisor_sm`, plus Stage 1 wifi/backoff/dispatch — all `ok`.

`./firmware/scripts/run_firmware_fast_gate.sh --host-only` → `pass=32 fail=0 skip=0`, `RESULT: PASS`.

`./firmware/scripts/run_firmware_fast_gate.sh` (ESP-IDF 5.4) → `pass=42 fail=0 skip=0`, `RESULT: PASS`.

Isolated `node_common`:  
`idf.py -B /tmp/dbmap-stage2-node … set-target esp32s3 && ninja -C /tmp/dbmap-stage2-node esp-idf/node_common/libnode_common.a` → exit 0, linked `libnode_common.a` (includes supervisor objects). Not flashed.

Skipped: none for Stage 2 host tests. Full product flash/HW verification not run (out of scope).

### Remaining Stage 2 limitations

- Hello/health still in product `app_main` (120 s backoff / F8 remains).
- Blocked `sp_net` is detected, not unblocked (Stage 3 will not drive Wi-Fi stop/start through it either).
- MQTT keepalive still 60 s.
- Supervisor and `sp_net` share priority 3; supervisor cannot preempt a running `sp_net` op (by design).

---

## Stage 3 record

### Inspection notes

Working tree before Stage 3: Stages 1–2 as documented. Wi-Fi lifecycle in `net_wifi.c`: single `esp_wifi_init`, default STA netif, handlers for CONNECTED/DISCONNECTED/GOT_IP/LOST_IP; no prior STOP/START handlers; reconnect via `esp_wifi_connect` / disconnect-for-retry on `sp_net`. Least-disruptive Stage 3 path: **`esp_wifi_stop` / `esp_wifi_start`** without re-init or duplicate handler registration. ESP-IDF **v5.4.4**.

Discrepancy vs older Stage 2 report text that said “Stages 3–4 unimplemented”: Stage 3 is now implemented; Stage 4 remains unimplemented.

### Activation

Supervisor-only (`sp_conn_s3_evaluate`). Requires all of:

- Supervisor up ≥ `SP_CONN_STAGE3_BOOT_GRACE_MS` (2 min).
- No verified MQTT progress for ≥ `SP_CONN_STAGE3_ESCALATE_MS` (15 min), measured from `last_verified_ms` (survives disconnect) or supervisor uptime if never verified.
- Worker responsive (not Stage 2 stalled).
- Not already ACTIVE; not EXHAUSTED; cooldown elapsed.

Normal MQTT backoff, brief disconnect, and DHCP (≤30 s) cannot meet the 15 min gap alone.

If escalation is due but the worker is stalled: state `SP_CONN_S3_BLOCKED_BY_WORKER`, action record-only — **no** stop/start attempt.

### Operation (serialized)

1. `sp_conn_s3_note_begin` bumps `iface_generation`.
2. `sp_wifi_request_iface_restart` posts `SP_NET_OP_WIFI_IFACE_RESTART`.
3. Worker: clear GOT_IP, cancel Wi-Fi timers, reset reconnect SM, `esp_wifi_stop`, `esp_wifi_start`, `esp_wifi_connect` via existing connect path.
4. Phases: STOP → START → CONNECT → WAIT_IP → WAIT_MQTT.
5. Success only on verified MQTT progress (`sp_connectivity_note_mqtt_verified`) for the current generation’s WAIT_MQTT phase.

Stale GOT_IP: `sp_conn_s3_on_got_ip` ignores `iface_generation != sm->iface_generation`.

### Limits

| Constant | Value | Role |
| --- | --- | --- |
| `SP_CONN_STAGE3_ESCALATE_MS` | 15 min | Activation |
| `SP_CONN_STAGE3_COOLDOWN_MS` | 5 min | Between failed attempts |
| `SP_CONN_STAGE3_OP_TIMEOUT_MS` | 60 s | Stop/start/DHCP phases |
| `SP_CONN_STAGE3_MQTT_WAIT_MS` | 90 s | Verified progress after IP |
| `SP_CONN_STAGE3_MAX_ATTEMPTS` | 3 | Then `SP_CONN_S3_EXHAUSTED` |
| `SP_CONN_STAGE3_BOOT_GRACE_MS` | 2 min | No Stage 3 after boot |

Exhausted: no further interface resets; audio/observation continue; exposed for Stage 4. Verified progress while idle resets consecutive failures.

### Acoustic / Observation

Stage 3 code paths do not call mic, I2S, AE, CE, or observation APIs. Host test `test_observation_isolation` asserts a sentinel is unchanged.

### Diagnostics

`sp_conn_status_t` includes Stage 3 state, phase, attempts, consecutive failures, op elapsed, last result, stop/start/IP/MQTT flags, blocked-by-worker, iface generation. Transition logs only.

### Tests and builds

`connectivity_stage3_sm` host test (`-Werror`): `ok`.

`./firmware/scripts/run_firmware_fast_gate.sh --host-only` → `pass=33 fail=0 skip=0` **PASS**.

`./firmware/scripts/run_firmware_fast_gate.sh` (ESP-IDF v5.4.4) → `pass=43 fail=0 skip=0` **PASS**.

`ninja … libnode_common.a` under `/tmp/dbmap-stage3-node` → exit 0. Not flashed.

Skipped: none for Stage 3 host tests. Hardware verification not claimed.

### Boundary to Stage 4

Stage 3 does not reboot and does not write NVS reboot budgets. Stage 4 owns the 60-minute no-progress reboot. Stage 3 may still be `EXHAUSTED` / `BLOCKED_BY_WORKER` when Stage 4 fires; that is expected.

---

## Stage 4 record

Controlled reboot when the device cannot observe-and-report (no verified MQTT progress) for **60 minutes**. No NVS reboot budget. **No Observation END** before this reboot. Hub/protocol unchanged. Repeated reboots every 60 minutes after each boot are acceptable.

### Activation

| Condition | Timer |
| --- | --- |
| Never verified since boot | `now_ms` (esp_timer from boot) ≥ 60 min |
| Had progress, then lost | `now_ms - last_verified_ms` ≥ 60 min |

Verified MQTT progress (same Stage 1–3 definition: QoS1 PUBACK / inbound DATA) resets Stage 4 to idle and restarts the 60-minute window. Monotonic time; if `now < last_verified`, age is 0.

Does **not** reboot merely because Wi-Fi is down at boot, DHCP is in window, or MQTT is in normal backoff — those intervals are far below 60 minutes. Stage 1–3 thresholds and backoff are unchanged. Stage 3 Wi-Fi recovery continues on its own policy during the hour.

Suppresses when `sp_lifecycle_ota_reboot_pending()` is already set (OTA owns the reboot).

### Sequence

1. Supervisor evaluates `sp_conn_s4_*` each period.
2. On eligibility: `EXECUTE_REBOOT` immediately (no Observation active check, no END publish).
3. Reboot path: `sp_lifecycle_enter_ota_reboot_pending`, `sp_mqtt_prepare_ota_reboot`, `sp_wifi_prepare_ota_reboot`, 500 ms settle, `esp_restart()`.

Stage 4 does **not** call `sp_ota_invoke_acoustic_close` or any Observation-close API. OTA install reboot continues to use its own acoustic-close + `ota_reboot` END path unchanged.

### Explicitly not implemented

- Observation END / `offline_reboot` / connectivity-specific termination reasons
- NVS reboot counters / daily budgets / persistent cooldowns
- New Hub events, diag fields, or protocol changes
- Stage 4 dependency on OTA close APIs

### Tests and builds

`connectivity_stage4_sm` host test (`-Werror`): pre/post 60 min, timer reset, never-verified boot case, direct reboot (no close API), no NVS budget, OTA-pending suppress.

Stage 1–3 host tests remain in the fast gate.

### Remaining limitations

- An open Observation may disappear across reboot without an END (accepted for connectivity Stage 4).
- Host tests do not prove on-device reboot timing.

---

## Stage 1 independent review

Review-only pass over the working-tree Stage 1 diff against this document. No code was changed in this pass. Host tests were re-run: `wifi_reconnect_sm`, `backoff`, `mqtt_progress`, and `net_dispatch_sm` each printed `ok` and exited 0. No flash, OTA, Hub change, acoustic-capture edit, or commit.

### Verdict

No **critical** findings. Stage 1 is ready for **controlled** hardware verification of the Wi-Fi recovery paths and of MQTT progress recovery on a quiet session (hello QoS 1 only).

Do **not** treat Ear observation-load MQTT progress as validated until the major PUBACK concurrency findings below are fixed or explicitly accepted as known risk for that test. Stages 2–4 remain unimplemented (`sp_connectivity`, interface restart, reboot budget: no matches in `firmware/**/*.c`).

### Findings

#### Critical

None.

#### Major

**M1. `s_progress` is mutated from multiple tasks without synchronization.**  
`sp_mqtt_publish_json` / `sp_mqtt_publish_json_wait` call `sp_mqtt_progress_note_qos1` on the publisher task (Ear observation priority 4, `app_main`, Output command path). `mqtt_event` calls `sp_mqtt_progress_on_published`, `on_inbound_data`, `on_connected`, and `on_disconnected` on the MQTT client task. `mqtt_on_progress_expired` on `sp_net` reads `expired` and recreate clears progress. There is no lock around `s_progress` in `hub_mqtt.c`. Slot updates in `mqtt_progress.c` are plain stores. On ESP32-S3 this is a real data race. Failure mode: torn slots, missed PUBACK match, or a false match after corruption. Safe side is usually an extra reconnect after 90 s; unsafe side is a corrupted match that refreshes the deadline without a real tracked delivery.

**M2. PUBACK can arrive before `note_qos1`.**  
`esp_mqtt_client_publish` unlocks the MQTT API lock and returns `msg_id` before `hub_mqtt.c` calls `sp_mqtt_progress_note_qos1` (lines after the publish return in `sp_mqtt_publish_json`). ESP-IDF dispatches `MQTT_EVENT_PUBLISHED` from the MQTT task while that task holds the API lock during receive handling. After the publisher unlocks, the MQTT task can process PUBACK and run `on_published` before `note_qos1` runs. Result: PUBACK is unknown (no deadline refresh); then `note_qos1` stores an already-acked id. Under dual-core preemption with fast broker RTT this is reachable. Failure mode: a healthy link that only publishes infrequent QoS 1 hello can still hit the 90 s progress expiry and recreate the client (false reconnect). Observation traffic makes the race more frequent but also supplies more PUBACKs and inbound DATA.

**M3. MQTT oneshot timer create/start failure is still silent.**  
`arm_oneshot` in `hub_mqtt.c` returns without scheduling if `esp_timer_create` or `esp_timer_start_once` fails. That covers the reconnect timer, connect watchdog, stable timer, and progress timer. Wi-Fi reconnect arm failure was fixed (`on_reconnect_arm_failed` + `defer_reconnect_arm`). MQTT progress arm failure after `on_connected` leaves a connected latch with no 90 s deadline (F7 partially reopened under heap pressure). Connect-watchdog arm failure leaves `in_flight` until another path re-arms. Required before calling F7 fully closed under OOM; not required for normal-heap HW tests.

#### Minor

**m1. `sp_net_dispatch_post_after` keeps one delayed op.**  
A second `post_after` overwrites the first (`net_dispatch.c`). Stage 1 only posts `WIFI_ARM_TIMER` there. Safe for Stage 1; Stage 2 must not assume a queue.

**m2. Stale dispatch bits after SM state changes.**  
`apply_sm_result` can post `WIFI_DISCONNECT_FOR_RETRY` while a later `GOT_IP` accepts the address before the worker runs. Mitigated: `wifi_worker_disconnect_for_retry` checks `sp_wifi_has_ip()` and calls `on_disconnect_skipped`. Documented, not a latch bug.

**m3. `mqtt_on_connect_watchdog` log says recreate but only schedules.**  
`hub_mqtt.c` `mqtt_on_connect_watchdog` sets `s_force_stop_start` and `schedule_reconnect`; recreate runs later in `try_reconnect` on `sp_net`. Behavior is correct; the log line overstates immediacy.

**m4. Progress slot `seq` wraps at `UINT16_MAX`.**  
`mqtt_progress.c` `note_qos1` restarts `next_seq` at 1. After wrap, oldest-slot eviction can drop the wrong entry until the ring turns over. Needs >65535 QoS 1 publishes in one session.

**m5. Wi-Fi stable timer vs disconnect race.**  
`wifi_stable_timer_cb` may call `sp_backoff_poll_stable` while a disconnect path notes failure if the oneshot already fired. Brief wrong failure count; next events correct it.

#### Informational

**I1.** Dispatch bit coalesce is intentional: duplicate posts of the same op before drain become one execution. New posts during `exec` are taken on the next drain iteration (`net_dispatch.c`).

**I2.** Drain order is lowest `sp_net_op_t` first, so Wi-Fi connect/disconnect runs before MQTT recreate when both are pending. Reasonable for outage recovery.

**I3.** `sp_net` create failure makes later `post` no-ops (`s_task == NULL`). Boot Wi-Fi connect and the first MQTT start still run on `app_main`. Documented Stage 1 residual risk.

**I4.** Observation isolation holds: Stage 1 platform sources do not call Ear observation or mic start APIs. Ear only consumes `sp_mqtt_is_connected()`. `sp_net` priority 3 is below observation (4) and mic (5).

**I5.** Host tests cover SM and progress math, not dual-core PUBACK races or timer-create failure on the MQTT oneshot path.

### Claim check

| Stage 1 claim | Supported? |
| --- | --- |
| Wi-Fi timer create failure does not drop the reservation | Yes — `on_reconnect_arm_failed`, `defer_reconnect_arm` |
| Late `GOT_IP` after in-flight watchdog | Yes — `allow_late_ip` + disconnect-before-retry |
| `IP_EVENT_STA_LOST_IP` clears the address bit first | Yes — `on_lost_ip` in `net_wifi.c` |
| Backoff resets only after 30 s stable IP | Yes — `sp_backoff_config_wifi` + stable oneshot |
| MQTT stop/recreate not on `esp_timer` | Yes — timer callbacks only `sp_net_dispatch_post` |
| Connected latch cleared without verified progress | Yes in design and happy path; M1/M2/M3 qualify robustness |
| Enqueue / QoS 0 are not verified progress | Yes — `note_qos0` no-op; enqueue does not call `mark_verified` |
| No `sp_connectivity` / reboot / interface restart | Yes |
| Observation independence | Yes |

### `sp_net` block residual (known Stage 1 risk)

If `sp_net` blocks inside `esp_mqtt_client_stop`, DNS/`getaddrinfo`, or a stuck `esp_wifi_*` call:

- Other `esp_timer` callbacks still run (they only post bits). Unrelated timers stay functional.
- Pending Wi-Fi and MQTT recovery bits queue but do not execute until the blocked call returns.
- Product hello/health keeps running; once `s_connected` is false it requests reconnect, which only posts another bit.
- Audio and observation tasks keep running at higher priority.
- There is still no task watchdog on `sp_net` in `sdkconfig.defaults`.

Periodic logging is not a watchdog and must not be added as one.

Safe later-stage mitigations (not Stage 1):

1. Stage 2 supervisor (separate task, higher than nothing that can block in driver stop) records `sp_net` last-progress time and a non-blocking heartbeat from `sp_net` at the start of each drain; if the heartbeat stalls beyond an existing bound (for example the 30 s connect watchdog scale), escalate without calling into the blocked stop from the supervisor.
2. Prefer bounded stops where the IDF API allows; do not wait forever without a supervisor-visible arm.
3. Optional FreeRTOS task watchdog on `sp_net` once a recovery policy for the panic/reboot path exists (Stage 4 territory).

### Required corrections before hardware testing

| Before | Action |
| --- | --- |
| Controlled Wi-Fi HW tests (disconnect, late DHCP, lost IP, timer-arm failure injection if possible) | None. Proceed. |
| Controlled MQTT quiet-session progress test (hello QoS 1, no observation flood) | Proceed; watch for false recreates (M2). |
| Ear under observation load as proof of M1/M2-safe progress | Fix M1 (lock all `s_progress` access) and close M2 (serialize note vs PUBACK, or define connected-session `MQTT_EVENT_PUBLISHED` policy) first. |
| Claiming F7 closed under heap exhaustion | Fix M3 (MQTT oneshot arm failure must reschedule like Wi-Fi). |

### Hardware verification checklist (Stage 1)

1. Flash a build that includes this working tree; confirm version string.
2. Boot to Hub online; confirm hello then health; confirm `WIFI_PS_NONE` log after `GOT_IP`.
3. AP leave / channel disruption: console shows disconnect, backoff schedule, reconnect; Hub returns to available without reboot.
4. Hold association without DHCP if the AP allows: within ~30 s see in-flight timeout, disconnect-before-retry, then a later `GOT_IP` accepted (or a clean new attempt).
5. Force lease loss if possible (`LOST_IP`): address bit clears (`sp_wifi_has_ip` false / MQTT defers), recovery reconnects.
6. Quiet MQTT: after `CONNECTED`, block Hub application traffic path if available but leave TCP up, or drop PUBACKs; within ~90 s expect progress expiry log, `s_connected` clear, client recreate, then recovery. Confirm audio still captures on Ear during that window.
7. Healthy quiet session for >3 min: no progress-driven recreate (hello PUBACKs refresh). If recreates appear, treat M2 as confirmed on device.
8. Confirm Stages 2–4 absent in behavior: no 15 min Wi-Fi stop/start policy, no connectivity reboot, no supervisor 60 s stage line.

### Review confirmation

- Stages 2–4 remain unimplemented.
- No code, Hub, acoustic-capture, flash, or commit actions were taken in this review pass.
- Stage 1 is ready for controlled hardware verification within the scope above; M1–M3 are the corrections that gate broader MQTT progress claims.

---

## 1. Current architecture

Connectivity is owned by Sensor Platform `firmware/platform/node_common/`. Products do not call `esp_wifi_*` or `esp_mqtt_*`.

| Layer | Owner | Entry points |
| --- | --- | --- |
| Wi-Fi STA, DHCP wait, reconnect | Platform | `sp_wifi_start`, `on_wifi_event`, `on_ip` in `net_wifi.c`; decisions in `wifi_reconnect_sm.c` |
| Backoff math | Platform | `backoff.c` (`sp_backoff_config_wifi` / `_mqtt` / `_https`) |
| DNS / mDNS | Platform | `sp_resolve_ipv4` in `net_resolve.c`, called once from `sp_mqtt_start` |
| MQTT session | Platform | `sp_mqtt_start`, `try_reconnect`, `mqtt_event` in `hub_mqtt.c`. Auto-reconnect is disabled (`.network.disable_auto_reconnect = true`) |
| Application hello / health / keepalive | Duplicated in both product `app_main` loops | `firmware/esp-ear/main/app_main.c`, `firmware/esp-output/main/app_main.c` |
| Desired HTTPS | Platform task | `hub_desired.c` `poll_task`, hourly, own backoff |
| Local audio | Ear product, started before Wi-Fi | `start_ear_mic_once` then `sp_wifi_start` |
| Observation publish | Ear product, consumes `sp_mqtt_is_connected()` | `ear_observation.c` |
| Hub online / offline | Hub | `device_monitor.py` from `reported_state.last_seen_at` |

Boot order (both products):

1. NVS, optional USB configuration window.
2. Ear only: start mic / I2S before Wi-Fi.
3. `sp_wifi_start` waits up to 30 s for `IP_EVENT_STA_GOT_IP`. On timeout it returns `ESP_ERR_TIMEOUT` and arms background reconnect. The product then calls `sp_boot_wait_wifi_ip_or_usb(..., 0)`, which blocks `app_main` until `sp_wifi_has_ip()` is true. MQTT, Desired, and Ear observation do not start during that wait. Mic capture already did.
4. One-shot SNTP (`sp_time_sync(10000)`), provisioning if needed, `sp_mqtt_start`, Desired poll, then the hello/health loop.

There is no platform connectivity supervisor. Wi-Fi and MQTT each keep their own timers. Nothing checks that a "connected" MQTT session is still delivering application traffic.

Hub availability is not an MQTT ping. `mqtt_listener.py` calls `apply_reported` for `hello`, `keepalive`, `health`, and `status`. Observation ingest does not update `last_seen_at`. `device_monitor.sample_devices` marks `unavailable` and records `heartbeat_overdue` when `last_seen_at` is older than `DBMAP_DEVICE_ONLINE_WINDOW_S` (default 90 s). The sample clock is the Hub monitor cycle (`hub_monitor_interval_s`, default 60 s). One cycle stamps every device with the same `observed_at`.

---

## 2. Current behavior by scenario

Defaults from `firmware/platform/node_common/Kconfig.projbuild` unless noted. Wi-Fi and MQTT backoff: initial 1 s, double each failure, cap 60 s, ±20% jitter. No failure count stops the sequence. Wi-Fi resets the progression on `GOT_IP` immediately (`stable_reset_ms = 0`). MQTT resets only after the session stays up for 30 s.

### Wi-Fi

| Scenario | What the code does | Retry limit | Timeout |
| --- | --- | --- | --- |
| Link lost after success | `WIFI_EVENT_STA_DISCONNECTED` clears the `GOT_IP` bit, counts a failure, and schedules one reconnect (`on_wifi_event`, `sp_wifi_reconnect_sm_on_disconnected`) | None. Caps at 60 s | Next attempt after backoff |
| Reconnect fails repeatedly | Every disconnect counts. `esp_wifi_connect()` error uses `on_connect_start_failed` and schedules again | None | Same backoff |
| Associated, no IP | `WIFI_EVENT_STA_CONNECTED` logs "awaiting DHCP/IP" and returns. `in_flight` stays set until `GOT_IP`, disconnect, or the 30 s in-flight watchdog | Watchdog reschedules forever | `SP_WIFI_IN_FLIGHT_TIMEOUT_MS` = 30 s, same as the boot `GOT_IP` wait |
| DHCP stalls | Same as associated-without-IP. The watchdog does **not** call `esp_wifi_disconnect()`. The next attempt is another `esp_wifi_connect()` while the STA may still be associated | None | 30 s plus backoff |
| Interface says up, traffic does not flow | No probe. `sp_wifi_has_ip()` is the `GOT_IP` event bit, not a packet check. `sta_netif_has_ip()` is used only in disconnect logs | No recovery while the bit stays set and no disconnect event arrives | None |
| Missed Wi-Fi event / inconsistent state | See findings. `IP_EVENT_STA_LOST_IP` is not registered. A late `GOT_IP` after the in-flight watchdog is ignored and does not set the `GOT_IP` bit | Can stop. See findings | — |

Disconnect logs are rate-limited to one line per 30 s unless the reason code changes (`WIFI_DISCONNECT_LOG_INTERVAL_US`). Scheduling lines are info-level and are not rate-limited.

`esp_wifi_set_ps(WIFI_PS_NONE)` runs only after the first successful `GOT_IP` inside `sp_wifi_start`. A boot that times out and later obtains an IP never reaches that call.

### IP and name resolution

| Scenario | What the code does |
| --- | --- |
| Has IP, broker or DNS unreachable | MQTT reconnect is the only detector. Desired HTTPS is a separate hourly task (`kPollMs` = 1 h, HTTP timeout 8 s, 3 transient retries) and does not repair Wi-Fi or MQTT |
| DNS fails or stalls at MQTT start | `sp_mqtt_start` calls `sp_resolve_ipv4` on `app_main`. `.local` uses mDNS (3 × 3 s) then `getaddrinfo`. Other names use `getaddrinfo` with no extra timeout in this module. Failure keeps the DNS name in the URI and logs that the broker client will retry |
| `.local` resolves | The URI is rewritten to `mqtts://<ip>:port` for the life of the client. `stop`/`start` reuses that URI. A later Hub address change is not looked up again |
| Public DNS name | The name stays in the URI so later connects can resolve it again. The lookup result is only logged |
| Network returns after a long outage | If Wi-Fi emitted disconnects, backoff is at the 60 s cap and the next attempt still runs. MQTT, if it knows it is down and `sp_wifi_has_ip()` is true, reconnects on its own timer. Success resets Wi-Fi backoff immediately and MQTT backoff after 30 s up |
| Stack stops making progress | No task watchdog is enabled in Ear or Output `sdkconfig.defaults`. No connectivity supervisor. A failed `esp_timer` create drops the Wi-Fi reservation and does not schedule another attempt |

### MQTT

Session settings in `sp_mqtt_start`: keepalive **60 s**, network timeout **20 s**, connect watchdog **30 s**, auto-reconnect **off**. Escalate to `esp_mqtt_client_stop` + `esp_mqtt_client_start` after 3 consecutive failures, or immediately after a reconnect call returns an error (`MQTT_RECONNECT_ESCALATE_AFTER`).

| Scenario | What the code does |
| --- | --- |
| Session lost after success | `MQTT_EVENT_DISCONNECTED` clears `s_connected`, counts one failure, schedules backoff. A duplicate `DISCONNECTED` does not count again; it schedules only if no timer is already armed |
| Reconnect fails repeatedly | Failure backoff to 60 s, forever. Every third failure (and any reconnect API error) uses stop/start on the same client |
| TCP up, MQTT connect or auth fails | Treated as connect failure via `DISCONNECTED` or `MQTT_EVENT_ERROR`. `ERROR` alone does not clear `s_connected`. If a connect is in flight it arms the 30 s watchdog; otherwise it schedules if no timer exists |
| Broker down, then back | Same reconnect loop. No IP check beyond `sp_wifi_has_ip()` |
| Client stalled | Connect watchdog clears `in_flight` if `CONNECTED`/`DISCONNECTED` never arrive. It does not run while `s_connected` is already true |
| Reports connected, publish or keepalive does not move | `sp_mqtt_is_connected()` is the `MQTT_EVENT_CONNECTED` latch. `sp_mqtt_publish_json` returns when the enqueue gets a message id. It does not wait for `PUBACK`. No code clears `s_connected` because publishes stop being delivered. The product loop calls `sp_mqtt_request_reconnect()` only when `!sp_mqtt_is_connected()` |
| Reconnected, reporting does not resume | `on_connected` re-subscribes the command topic and logs "connected". Hello/health resume only when the product loop publishes. That loop is independent and may be backed off to 120 s |

If Wi-Fi has no IP, `try_reconnect` does not count a failure. It arms a 2 s timer and logs "deferring MQTT reconnect".

`arm_oneshot` returns without a log if `esp_timer_create` or `esp_timer_start_once` fails. The product loop is then the only MQTT kicker, and only while `s_connected` is false.

### Cross-layer

| Scenario | What the code does |
| --- | --- |
| Wi-Fi returns, DHCP does not | Watchdog path above. MQTT stays deferred on the 2 s timer because `sp_wifi_has_ip()` is false, including when a late `GOT_IP` was ignored while the netif already has an address |
| IP is back, MQTT is not | MQTT's own timer calls `try_reconnect`. `GOT_IP` does not wake MQTT directly. That is fine while the MQTT timer exists |
| MQTT is up, keepalives are not | Product loop publishes hello then health/keepalive, then delays. Delay becomes 30 s after a hello **enqueue** succeeds, otherwise doubles from 1 s up to 120 s. A 120 s gap is already past the Hub's 90 s window. Enqueue success is not delivery |
| Events and state diverge | `GOT_IP` bit, netif address, `s_sm.in_flight`, and `s_connected` are updated on different events and can disagree. See findings |
| Retries stop | Wi-Fi: timer create/start failure releases `reconnect_armed` and does not reschedule (`arm_reserved_reconnect_timer`). Nothing else calls Wi-Fi reconnect. MQTT: timer failure is recovered by the product loop only when the latch says disconnected. A stuck `s_connected == true` stops MQTT recovery with no log |

OTA reboot pending (`sp_lifecycle_ota_reboot_pending`) freezes both reconnect state machines on purpose. That path ends in `esp_restart` from `hub_ota.c`. No other path reboots because the network is down.

---

## 3. Findings

Each item is a source-code fact.

### F1. False "up" is not detected

`sp_wifi_has_ip` (`net_wifi.c`) is the event-group bit set only from an accepted `IP_EVENT_STA_GOT_IP`. `sp_mqtt_is_connected` (`hub_mqtt.c`) is set only from `MQTT_EVENT_CONNECTED` and cleared only from `MQTT_EVENT_DISCONNECTED` (or OTA teardown). Publish enqueue success in `sp_mqtt_publish_json` does not mean the broker got the packet. Nothing records the last `MQTT_EVENT_PUBLISHED`, inbound packet, or ping response.

The Hub's 90 s window is application traffic (`apply_reported` in `service.py`), not the MQTT keepalive. Observation publishes do not refresh `last_seen_at` (`mqtt_listener.py` returns before `apply_reported` for `/observation`).

### F2. In-flight Wi-Fi watchdog does not tear down the STA

`wifi_in_flight_watchdog_cb` clears `in_flight`, counts a backoff failure, and calls `esp_wifi_connect()` again via `start_wifi_connect_attempt`. The tree never calls `esp_wifi_disconnect()`, `esp_wifi_stop()`, or `esp_netif` DHCP restart. An association that never completes DHCP can sit while later `esp_wifi_connect()` calls fail or return without a new `GOT_IP`.

### F3. Late `GOT_IP` after the watchdog is dropped

`sp_wifi_reconnect_sm_on_boot_wait_timeout` sets `allow_late_ip` so a slow boot DHCP can still count. `sp_wifi_reconnect_sm_on_watchdog` clears `allow_late_ip` and does not set it. `sp_wifi_reconnect_sm_on_got_ip` then ignores the event. `on_ip` returns before `xEventGroupSetBits(GOT_IP)`. The netif can hold an address while `sp_wifi_has_ip()` stays false, so MQTT defers forever (`try_reconnect` "no IP" branch) until some later accepted `GOT_IP`.

### F4. `IP_EVENT_STA_LOST_IP` is ignored

`sp_wifi_start` registers `WIFI_EVENT_STA_CONNECTED`, `WIFI_EVENT_STA_DISCONNECTED`, and `IP_EVENT_STA_GOT_IP` only. A lost lease that does not produce `STA_DISCONNECTED` leaves the `GOT_IP` bit set. MQTT keeps using a dead address. Wi-Fi schedules nothing.

### F5. Wi-Fi retry can stop after a timer failure

`try_reserve_reconnect` allows one reservation. `arm_reserved_reconnect_timer` releases it when `esp_timer_create` or `start_once` fails, then returns. No disconnect is in flight, so no later event is guaranteed. There is no periodic Wi-Fi kick comparable to `sp_mqtt_request_reconnect`.

The in-flight watchdog has the same hole from the other side: `arm_watchdog_timer` logs and returns if the timer cannot be created, leaving `in_flight` and `watchdog_armed` set with no timer. A missing `STA_DISCONNECTED` then never schedules again.

### F6. MQTT stop/start runs on the timer callback

`try_reconnect` calls `esp_mqtt_client_stop` / `esp_mqtt_client_start` from `reconnect_timer_cb`, which is an `esp_timer` callback. Wi-Fi reconnect and the MQTT connect watchdog use the same timer task. If `stop` waits for an MQTT task blocked in DNS, TLS, or a socket write, Wi-Fi and MQTT timers stop firing together. `s_connected` may already be false, so the console goes quiet. Ear mic and observation tasks are other FreeRTOS tasks and keep running.

This is a structural hazard in the call site. The ESP-IDF `stop` wait bound is not vendored in this repo, so the exact block duration is not claimed here.

### F7. MQTT "connected" suppresses recovery

`try_reconnect` returns immediately when `s_connected` is true. `sp_mqtt_request_reconnect` does too. `MQTT_EVENT_ERROR` while `s_connected` is true does not schedule recovery. A half-open session that still looks connected produces no reconnect log. That matches a console where audio processing continues and neither Wi-Fi nor MQTT logs a retry.

Hello failure is also silent: neither `app_main` logs a failed `sp_publish_hello`, nor does `sp_mqtt_publish_json` log an enqueue failure.

### F8. Application publish backoff outlives the Hub window

Both `app_main` loops use the same policy: 30 s after hello enqueue succeeds; otherwise double up to 120 s. The Hub window is 90 s (`config.py` `device_online_window_s`). After two failed hellos the device can be marked `heartbeat_overdue` while the MQTT latch is still true and no reconnect is requested. Health and keepalive are published in the same iteration as hello (`sp_publish_health` also publishes the keepalive topic), so they share that delay.

Documented "30-second keepalive" in `docs/protocol.md` and `docs/connectivity.md` does not match the firmware session keepalive of 60 s in `hub_mqtt.c`. The Hub's own listener uses keepalive 30 (`mqtt_listener.py`). Those are three different clocks.

### F9. `.local` address is frozen

`sp_mqtt_start` copies a resolved `.local` address into `s_uri`. Recovery stop/start does not call `sp_resolve_ipv4` again. A Hub that moves on the LAN stays unreachable until reboot.

### F10. Modem-sleep disable is skipped when boot has no IP

`WIFI_PS_NONE` is after the successful-boot path only (`sp_wifi_start`). The comment there states modem sleep stalls MQTT/TLS keepalive writes. A device that joins after the 30 s boot wait never applies that setting.

### F11. Diagnostics cannot name the stuck layer

`sp_diag_attach_json` adds counters only when non-zero, and only on a health/keepalive publish that then succeeds (`sp_publish_health` clears one-shots only after health enqueue succeeds). There is no `wifi_state`, `ip_state`, `mqtt_state`, or progress age. Counters saturate at 999. They are RAM-only and die on reboot. While the device is silent, the Hub has only `last_seen_at`.

UART disconnect detail exists (`reason`, `failures`, `armed`, `in_flight`, `netif_ip`) but only on `STA_DISCONNECTED`, and it is rate-limited. A healthy-looking latch logs nothing.

`sp_diag_log_resources` can print heap and stack high-water marks. Nothing calls it on a connectivity cadence.

### F12. Hub `heartbeat_overdue` is a database row, not a container log line

`_record_event` in `device_monitor.py` inserts `DeviceMonitorEvent` and does not log. `run_cycle` in `hub_monitor.py` logs exceptions, not each device transition. Absence of a matching `hub-bootstrap` log line is what this code does. It does not show that the Hub ignored the device. The UI event is the record. Broker logs are a separate file (`mosquitto_log_path`) and were not treated as evidence here.

Identical timestamps across devices are also what this code does. `sample_devices` uses one `observed_at` for the whole cycle. Two nodes that cross the 90 s window between polls share that stamp even when their last keepalives differed.

### F13. Platform versus product

Shared recovery lives in platform code. Violations of the ownership rule:

| Location | Issue |
| --- | --- |
| `firmware/esp-ear/main/app_main.c` and `firmware/esp-output/main/app_main.c` | Copy-pasted hello/health cadence and the `sp_mqtt_request_reconnect()` kick. The 120 s ceiling is a second, product-level retry policy, and it is the only MQTT kick if the platform timer is gone |
| Ear observation | Allowed. It reads `sp_mqtt_is_connected()` and skips publish. It does not reconnect |
| Output command / audio | No private Wi-Fi or MQTT state machine. Playback and commands fail until platform MQTT returns |
| Future products | Would copy `app_main` and inherit F7 and F8 |

Wi-Fi reconnect is not duplicated. Good.

### F14. No staged recovery and no connectivity reboot

Escalation today is only MQTT stop/start of the same client after three failures. There is no Wi-Fi driver restart and no `esp_restart` for a dead network. `esp_restart` in this tree is USB configuration accepted, unprovisioned boot recovery, and OTA (`serial_config.c`, `hub_ota.c`). A stuck latch stays stuck until a human power-cycles the board. Local Ear capture is intentionally independent of MQTT (`app_main` log: "independent of later MQTT/Wi-Fi drops") once observation has started. During the indefinite boot wait, capture runs and observation does not.

---

## 4. Root-cause hypotheses

These are not established for the two devices. Do not treat them as the same fault.

**Confirmed about the Hub event itself**

- `heartbeat_overdue` / "no keepalive within 90s online window" is produced by `device_monitor._emit_transition` when a sample moves `available` → `unavailable`.
- The stamp `2026-11-16 16:32:30 CET` is the monitor sample time, not a measured device-side failure time. Two devices in one cycle share it.
- No `hub-bootstrap` log line is required for that event to exist (F12).

**Hypothesis A — false healthy session (fits a quiet console)**

Ear processing tasks still run. Wi-Fi does not log reconnect because no `STA_DISCONNECTED` arrived. MQTT does not log reconnect because `s_connected` stayed true, so `sp_mqtt_request_reconnect` returned immediately (F7). Hello/health stopped being delivered, so the Hub window expired (F1, F8). Plausible triggers: half-open TCP, a stuck MQTT task, or publishes enqueued into a session that no longer moves. This does not require the two devices to share a firmware bug; a broker or AP event can trip both, and each device can then fail to notice.

**Hypothesis B — Wi-Fi state machine stopped scheduling (also quiet)**

Timer create failure or a watchdog that never armed (F5), or `in_flight` stuck with no disconnect. MQTT then sits in the "deferring, no IP" 2 s loop, which does log at warning level. A console with **no** Wi-Fi and **no** MQTT lines is a weaker fit unless that deferral was also stuck (timer task blocked, F6) or the log level hid warnings.

**Hypothesis C — associated without a usable IP (usually noisy)**

F2 and F3 cause repeated "scheduling Wi-Fi reconnect" / "in-flight timeout" lines. A long USB capture that lacks those lines argues against this being the steady state at the moment of inspection. It can still be the entry into hypothesis A if a later `GOT_IP` was ignored and a previous session latch stayed true. That combination needs the `GOT_IP` bit to remain set, which F3 does not do. F4 (lost IP, bit left set) plus F7 is a better quiet variant: Wi-Fi still "has IP", MQTT still "connected", no new events.

**Hypothesis D — shared Hub or broker pause**

One sample clock explains the shared timestamp without a shared device bug. A broker restart would normally produce MQTT `DISCONNECTED` and then reconnect logs. Lack of those logs, if the capture is complete and warning-level, points back at A or F6 rather than a clean broker bounce. This review did not read Mosquitto or device consoles from 2026-11-16.

**Hypothesis E — firmware older than this tree**

EAR-003 was described as an older generation than 0.8.17. This review did not diff that image. Recovery here (Wi-Fi state machine, MQTT disable-auto-reconnect, 30 s watchdogs) may be missing or different on that build. Same Hub timestamp + different firmware is compatible with a shared network event **or** with two different stuck states. It is not evidence they took the same branch in code.

**What would distinguish them on a future capture**

| Console, while audio still runs | More likely |
| --- | --- |
| No `sp_wifi` / `sp_mqtt` warnings, hello loop silent | A or F4+F7 |
| Repeating "in-flight timeout" / "scheduling Wi-Fi reconnect" | F2 / F3 |
| Repeating "deferring MQTT reconnect; Wi-Fi has no IP" | Wi-Fi bit clear, MQTT loop alive |
| Last line "MQTT recovery via stop/start", then silence | F6 |
| "waiting for IP indefinitely" and no later "Wi-Fi IP available" | Still in boot wait; observation not started |

---

## 5. Proposed design

Add one platform component, `sp_connectivity`, used by Ear, Output, and later products. Products keep consuming status. They stop owning the hello cadence and the reconnect kick.

### State model

Report layers separately. A higher layer must not be marked healthy because a lower event fired.

| Field | Values | Set by |
| --- | --- | --- |
| `wifi_link` | `down`, `associating`, `associated` | STA events, plus `associating` when a connect attempt is in flight |
| `ip` | `none`, `pending`, `assigned` | `GOT_IP`, `LOST_IP`, and a read of the netif address when an event is ambiguous |
| `mqtt` | `stopped`, `connecting`, `connected`, `degraded` | MQTT events, plus `degraded` when connected but unverified |
| `verified_progress_at` | monotonic time | `MQTT_EVENT_PUBLISHED` for QoS 1 hello/health, or any inbound `MQTT_EVENT_DATA` / ping response. Enqueue is not progress. `GOT_IP` is not progress |

`degraded` means the client latch says connected and `verified_progress_at` is older than the Hub window (90 s). That state **must** start recovery even though `sp_mqtt_is_connected()` is true. Callers that only need "may I enqueue?" can keep a boolean. Observation skip logic should treat `degraded` like disconnected so it does not block the audio task on a dead outbox.

### Supervisor

One platform task, started from `sp_wifi_start` / `sp_mqtt_start`, not from product `app_main`:

- Owns Wi-Fi reconnect, MQTT reconnect, hello/health/keepalive publish, and escalation.
- Period 5 s for state evaluation. Publish period stays 30 s while progress is fresh.
- Does not run on the `esp_timer` callback and does not call blocking `esp_mqtt_client_stop` from that callback. Timer callbacks only set a flag. The supervisor performs stop, destroy, and Wi-Fi disconnect.
- On `GOT_IP`, kicks MQTT immediately instead of waiting for a leftover timer.
- On `LOST_IP` or ignored-late-IP, clears the IP bit and continues recovery.
- Reschedules if a timer cannot be created (short delay on the supervisor, which is a FreeRTOS task, not a one-shot that can vanish).
- Resets Wi-Fi backoff only after an address has stayed assigned for 30 s (same stability idea MQTT already uses), not on the first `GOT_IP` of a flap.
- Resets MQTT backoff only after 30 s connected **and** one verified progress event.
- Leaves Desired HTTPS on its own task and backoff.

Product `app_main` loops delete the publish/reconnect block and call `sp_connectivity_wait` only if they still need a boot gate. Ear observation and Output commands stay subscribers of status.

### Recovery stages

The supervisor runs the highest stage whose condition is true. A stage is not "received an event". It is "no verified progress, and the previous stage has had time to work".

1. **Retry.** Existing exponential backoff, 1 s to 60 s, jitter 20%, no attempt cap. Every failure path must arm the next attempt, including timer-create failure, connect API error, `MQTT_EVENT_ERROR`, `LOST_IP`, and `degraded`.
2. **Component restart.** Wi-Fi: `esp_wifi_disconnect()` before the next `esp_wifi_connect()` when the in-flight watchdog fires or DHCP produced no address. MQTT: destroy and `esp_mqtt_client_init` again, and resolve the host again (including `.local`). Do this from the supervisor after 3 failed connect cycles or immediately on `degraded`.
3. **Interface restart.** `esp_wifi_stop` / `esp_wifi_start` (and DHCP client restart) when stage 2 has cycled for 15 minutes with no verified progress. Then recreate the MQTT client. Apply `WIFI_PS_NONE` on every successful `GOT_IP`, not only the boot success path.
4. **Device restart.** Last resort only. See section 7.

Local Ear capture and an open observation keep running through stages 1–3. Output playback may fail while MQTT or HTTP is down; that is not a reason to reboot during stage 1 or 2.

---

## 6. Timeout and retry proposal

Values already in the tree are kept where they match a real neighbor. New values are derived from those neighbors.

| Knob | Value | Why |
| --- | --- | --- |
| Wi-Fi / MQTT backoff | 1 s, double, cap 60 s, jitter 20% | Already configured and host-tested. Fast enough for a blip, slow enough not to hammer an absent AP. Stay indefinite |
| Wi-Fi in-flight / boot DHCP wait | 30 s | Already `SP_WIFI_IN_FLIGHT_TIMEOUT_MS`. Covers association plus DHCP. On expiry, **disconnect**, then backoff |
| MQTT connect watchdog | 30 s | Already `MQTT_CONNECT_WATCHDOG_MS`, longer than `network.timeout_ms` (20 s). Keep. Run the consequence on the supervisor, not inside a nested timer callback that may be blocked |
| MQTT session keepalive | **30 s** | Match `docs/protocol.md`, the Hub listener, and the 30 s hello period. The current 60 s session keepalive is a docs/code split, not a chosen third policy |
| Application hello + health/keepalive | 30 s fixed | Match the period the Hub window was built around (90 s ≈ three publishes). Remove the 120 s product backoff. Publish failure sets `degraded` and enters stage 1/2; it must not slow the next attempt past the Hub window |
| Verified-progress deadline | 90 s | Same number as `device_online_window_s`. When the Hub is about to call the device unavailable, the device must already be treating itself as `degraded` |
| No-IP MQTT deferral | 2 s | Already in `try_reconnect`. Keep, but also re-read the netif so a dropped `GOT_IP` cannot defer forever |
| Wi-Fi and MQTT stable reset | 30 s | MQTT already waits 30 s. Wi-Fi should too, so a flapping AP does not snap backoff back to 1 s on every brief `GOT_IP` |
| Supervisor tick | 5 s | Shorter than the 30 s publish period and the 30 s watchdogs, so a missed timer is noticed within one publish period |
| Stage 2 trigger | 3 failed connect cycles, or immediate `degraded` | 3 is already `MQTT_RECONNECT_ESCALATE_AFTER`. `degraded` cannot wait for three clean failures because the latch never counts them |
| Stage 3 trigger | 15 min since last verified progress, with stage 2 already attempted at least once | 15 min is 15 × the 60 s cap, plus several 30 s watchdogs. An AP or broker reboot finishes well inside it. It is short enough that a wedged driver is not left for days |
| Desired HTTPS | unchanged (8 s, 3 retries, 1 h poll) | It is not the connectivity path. Do not couple it |

`getaddrinfo` / mDNS during recovery must run on the supervisor with the existing mDNS attempt cap (3 × 3 s) and must not run on `app_main` after boot. Boot may still do one lookup. A failed lookup keeps the DNS name; a success must be refreshed on every stage-2 MQTT recreate.

---

## 7. Escalation and reboot

Stages 1–3 are the recovery policy. A periodic reboot is not a substitute for F5–F7. Those must be fixed first. Reboot is only for a network stack that still makes no verified progress after an interface restart.

**Trigger:** time since last verified progress, not time since boot, and not "Wi-Fi disconnected". A device at a site whose AP is off is failing retry correctly; that is not a stuck stack. Require all of:

- no verified progress for **24 h**, and
- at least one stage-3 interface restart already completed in that window, and
- not in OTA reboot-pending, and
- reboot budget still available.

24 h is long enough that a workday outage or an overnight AP outage does not reboot the fleet, and short enough that a wedged lwIP/Wi-Fi driver does not wait a week. Seven days is the wrong primary threshold: the device has already been useless to the Hub for a week, and the reboot teaches nothing about which stage was stuck. Keep seven days only as a **budget window**, not as the timer that fires the reboot.

**Local audio:** keep running through stages 1–3. Reboot is the step that stops capture.

**Open observation:** a reboot drops RAM episode state. If MQTT is down, an END cannot be delivered first. Policy:

- If an observation episode is open, defer the reboot by up to 2 minutes so a stage-2 reconnect can still publish END.
- If progress returns, cancel the reboot.
- If the episode is still open after that grace, reboot anyway. A multi-hour episode must not block recovery forever. The next boot's diag carries the reason so the missing END is explainable. Do not add a product-specific Ear reboot policy; the platform asks a registered "episode open?" hook with a default of "not open". Ear registers the hook. Output registers one if a clip is playing; otherwise it reboots on the platform timer.

**Reboot budget:** at most one connectivity reboot per 24 h, and at most three per rolling 7 days, stored in NVS with the reason. After the budget is spent, stay on stages 1–3 and keep a sticky `recovery_suppressed` flag for the console and the next successful diag publish. That prevents a reboot loop when stage 3 cannot help (bad credentials, wrong SSID).

**After reboot:** NVS stores `reason` (`no_verified_progress`), `stage` reached, `wifi_link`, `ip`, `mqtt`, progress age, and firmware version. The existing `last_reset_reason` path (`SW` for `esp_restart`) is not enough. The first health publish includes `connectivity_reboot_reason` even when other diag counters are zero.

Do not reboot because hello enqueue failed once, because RSSI is low, or on a fixed weekly schedule.

---

## 8. Diagnostics

### Console

Log a state transition at warning level whenever `wifi_link`, `ip`, `mqtt`, or recovery stage changes. Always include the reason code on disconnect (already partly there).

Log one supervisor line every 60 s even when healthy, at info level:

`link=<wifi_link> ip=<ip> mqtt=<mqtt> progress_age_s=<n> stage=<n> wifi_failures=<n> mqtt_failures=<n>`

Silence of that line means the supervisor task itself stopped. Today's silence means "no disconnect event", which is ambiguous.

On every recovery action, one warning: `action=wifi_disconnect|mqtt_recreate|wifi_restart|device_restart reason=...`.

Rate-limit repeats of the same line to once per 30 s. Do not rate-limit stage changes.

### Health payload

Add these `diag` fields on health/keepalive. Include them even when zero so a missing field is not confused with a healthy zero (today's omit-when-zero counters can stay omit-when-zero).

| Field | Meaning |
| --- | --- |
| `wifi_link` | `down` / `associating` / `associated` |
| `ip_state` | `none` / `pending` / `assigned` |
| `mqtt_state` | `stopped` / `connecting` / `connected` / `degraded` |
| `progress_age_s` | Seconds since verified progress, saturating at 86400 |
| `recovery_stage` | 0–3 while running, 4 if this boot followed a connectivity reboot |
| `connectivity_reboot_reason` | Present once after a stage-4 reboot, then cleared like `last_reset_reason` |

Existing counters (`wifi_disconnects`, `mqtt_connect_failures`, …) stay. Hub `device_diag.py` stores the diag object and already ignores unknown keys for health scoring. New fields ride along in `last_diag_json` without a new Hub event type. That is enough for a post-mortem after the device speaks again.

The Hub still cannot see a silent device's stage **during** the outage. `heartbeat_overdue` remains the live signal. It means "no hello/health/keepalive/status inside the window". It does not mean "device powered off", "Wi-Fi down", or "MQTT task wedged". After this change, the next keepalive distinguishes those. Optional later Hub work, not part of the firmware fix: log one `hub-bootstrap` warning when recording `heartbeat_overdue`, so container logs and the UI event match. That would be a Hub logging change, not a new event type.

Intentionally offline versus stuck: the firmware has no supported "stay offline" mode after provisioning. Unprovisioned devices reboot through the USB window and never hold a quiet MQTT session. A provisioned device that stops progress is stuck or out of range. The Hub cannot tell those apart until the device reports `wifi_link=down` with `recovery_stage` advancing, or never returns. `recovery_suppressed` on the next session means retries continued and the reboot budget was spent.

---

## 9. Implementation plan

Stages 1–3 are implemented (stall fixes, supervisor, Wi-Fi interface restart). Proposal Stage B's publish-loop ownership and Stage D (reboot budget) are not. Do not combine the reboot with the stall fixes.

**Stage A — close the stall holes (no new reboot, no new Hub event)**

- Handle `IP_EVENT_STA_LOST_IP`: clear `GOT_IP`, schedule Wi-Fi recovery.
- On in-flight timeout and on connect-while-associated: `esp_wifi_disconnect()` before the next connect.
- Accept a late `GOT_IP` for the watchdog generation the same way boot timeout does, or disconnect so a new `GOT_IP` is guaranteed. One policy, covered by `test_wifi_reconnect_sm.c`.
- If the reconnect or watchdog timer cannot be created, clear `in_flight` and schedule again from a FreeRTOS delay.
- Set `WIFI_PS_NONE` on every accepted `GOT_IP`.
- Treat "no `MQTT_EVENT_PUBLISHED` and no inbound MQTT for 90 s" as disconnect: clear `s_connected`, count a failure, reconnect.
- Move `esp_mqtt_client_stop` / `start` off the timer callback.
- Re-resolve the broker host on MQTT recreate. Do not keep a stale `.local` IP.
- Set session keepalive to 30 s.
- Host tests: late `GOT_IP` after watchdog, `LOST_IP` clears the bit, timer-create failure schedules again, degraded session requests reconnect while the latch was true.

**Stage B — platform supervisor and one publish loop**

- New `sp_connectivity` task owns backoff kicks, hello/health, and stage 1–2.
- Delete the duplicated loops in both `app_main.c` files. Products do not grow a third copy.
- Ear observation keeps the connected/degraded check.
- 60 s console line and the diag fields in section 8.
- Tests: publish period stays 30 s when enqueue fails; supervisor tick still runs.

**Stage C — interface restart**

- Stage 3 after 15 min without verified progress.
- Test the decision function on host: progress resets the 15 min clock; a single disconnect does not stop/start Wi-Fi.

**Stage D — reboot budget**

- NVS reason, 24 h trigger, 2 min observation grace hook, 1/24 h and 3/7 d caps, `connectivity_reboot_reason` on the next health.
- Ear supplies the episode-open hook. Output supplies a hook only if playback must be deferred; otherwise the default applies.
- Test the budget math on host. Do not reboot a device as the test.

Regression already in tree to keep green: `firmware/platform/node_common/test_host/test_wifi_reconnect_sm.c`, `test_backoff.c`.

---

## 10. Risks

| Change | Risk |
| --- | --- |
| Disconnect / Wi-Fi stop while an observation is open | Publishes fail until MQTT returns. Capture continues. Stage 3 can drop a few windows. Acceptable next to a stuck STA |
| MQTT destroy | In-flight QoS 1 observations and a half-sent hello are dropped. The Hub already dedups by observation id. A missed END stays a gap until the next episode |
| `degraded` forces reconnect | A slow but alive broker can flap if PUBACK is late. The 90 s deadline matches the Hub window, so the device reconnects when the Hub would already have marked it down. Do not use a shorter deadline |
| Keepalive 60 s → 30 s | More ping traffic. Power save is already supposed to be off. Apply `WIFI_PS_NONE` on every `GOT_IP` (F10) or the shorter keepalive makes the stall in the `sp_wifi_start` comment more likely |
| Larger `diag` object | Mosquitto `max_packet_size` is 16384. The new fields are a few dozen bytes. Do not attach logs or SSID |
| Reboot | Loses the open Ear episode, Output playback, and RAM counters. Budget and the grace hook exist for that. A bad SSID must hit `recovery_suppressed` instead of rebooting every day |
| Moving the publish loop | OTA `sp_ota_report_pending_boot` is called from the product loop today. The supervisor must keep calling it, or boot confirmation after OTA stops |
| Fixing retries without a reboot | Devices already stuck in F6 or F7 do not pick up the fix until they are flashed or power-cycled. The proposal does not remote-reboot the current fleet |

---

## Source index

| Topic | Location |
| --- | --- |
| Wi-Fi events, timers, boot wait, `WIFI_PS_NONE` | `firmware/platform/node_common/net_wifi.c` — `on_wifi_event`, `on_ip`, `arm_reserved_reconnect_timer`, `arm_watchdog_timer`, `wifi_in_flight_watchdog_cb`, `sp_wifi_start` |
| Wi-Fi decisions | `firmware/platform/node_common/wifi_reconnect_sm.c` — `on_disconnected`, `on_watchdog`, `on_boot_wait_timeout`, `on_got_ip` |
| Backoff | `firmware/platform/node_common/backoff.c`, `Kconfig.projbuild` menu "Network retry backoff" |
| MQTT | `firmware/platform/node_common/hub_mqtt.c` — `try_reconnect`, `on_disconnected`, `mqtt_event`, `sp_mqtt_start`, `sp_mqtt_request_reconnect`, `sp_mqtt_publish_json` |
| DNS | `firmware/platform/node_common/net_resolve.c` — `sp_resolve_ipv4` |
| Hello/health | `firmware/platform/node_common/health.c` — `sp_publish_hello`, `sp_publish_health` |
| Diag counters | `firmware/platform/node_common/diag.c` — `sp_diag_attach_json` |
| Indefinite boot wait | `firmware/platform/node_common/serial_config.c` — `sp_boot_wait_wifi_ip_or_usb` |
| Product loops | `firmware/esp-ear/main/app_main.c`, `firmware/esp-output/main/app_main.c` |
| Ear publish gate | `firmware/products/dbmap/ear/ear_observation/ear_observation.c` (`sp_mqtt_is_connected`) |
| Hub window and event | `hub/bootstrap/app/device_monitor.py` — `_emit_transition`, `_record_event`, `sample_devices` |
| Hub last-seen | `hub/bootstrap/app/service.py` — `apply_reported`; `hub/bootstrap/app/mqtt_listener.py` — `_on_message` |
| Window defaults | `hub/bootstrap/app/config.py` — `device_online_window_s`, `hub_monitor_interval_s` |
