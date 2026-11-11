# ESP-Ear firmware

Initial Ear application for ESP32-S3 + one I2S MEMS microphone.

## Scope (first milestone)

- Reuses `node_common` for Wi-Fi, HTTPS provision, MQTT, OTA Desired poll, hello/health.
- Captures from **one** microphone model per image (currently **INMP441** via `ear_mic`).
- NVS `microphone_count` **1** → mono LEFT; **2** → Philips stereo on shared DIN0 (mic0=left, mic1=right). Observation / L0 / activity still use **mic 0 only**.
- Attaches windowed RMS/peak **dBFS** levels on hello/health/keepalive reported state (same path as `wifi_rssi_dbm`). Hub shows them under **Additional reported values** until a dedicated Ear UI exists.
- JSON uses `audio.microphones[]` / `capabilities.microphones[]` so a later multi-mic Ear can add indexes without a schema break.
- **Observation publisher** (`ear_observation`): protocol-v1 Observations on the provisioned `…/observation` topic (QoS 1, not retained) with `classification: null` and `bearing: null`.
- **Phase 2 RMS activity detector**: one aggregate acoustic-activity stream per Ear (not source classification). Emits start / periodic / end Observations. Optional Phase 1 one-shot test trigger remains available (`DBMAP_EAR_OBS_TEST_PUBLISH_ONCE`, default off).
- Does **not** include source-family ML classification, bearing/DoA, multi-source separation, detection_confidence, Event/Track logic, Output playback, or raw-audio MQTT.

### Observation capabilities (reported state)

`capabilities.observation` on hello/health/keepalive:

| Field | Phase 2 value | Meaning |
| --- | --- | --- |
| `publisher` | `true` | Device can emit protocol-v1 Observations |
| `protocol_version` | `1` | Observation envelope version |
| `detector_kind` | `"rms_activity"` | Windowed RMS activity (not a sound-family classifier) |
| `detector_version` | `1` | Detector implementation version |

### Sequence-number NVS strategy

Avoid one NVS write per Observation:

| Layer | Role |
| --- | --- |
| NVS `ear_obs` / `seq_lim` | Exclusive upper bound of the currently reserved block |
| RAM `next`…`limit` | Hand out numbers inside that block |

On boot, `next` starts at the durable `seq_lim`, so any unused remainder of the previous block is **skipped** (gaps OK). A new block is reserved (`seq_lim := next + block_size`, default 64) and written to NVS **before** any number from that block is allocated (failed NVS commit rolls back the RAM limit). Phase 1 key `seq` meant “next to assign” (persisted before publish); when `seq_lim` is absent it migrates to `legacy_seq + block_size` (skip-forward). Overflow saturates at `UINT32_MAX` and stops allocation (no wrap).

**Consumption point:** a sequence number is consumed when `dbmap_obs_seq_allocate()` returns it (the containing block limit is already in NVS). If MQTT publish fails afterward, that number is **not** reused. If MQTT is down when an emission is due, the emission is dropped and **no** sequence number is allocated.

### RMS activity vs microphone diagnostics

| Concern | Mechanism |
| --- | --- |
| Mic / I2S health | Existing hello/health `audio.*` fields (`microphone_status`, `classification_reason`, RMS/peak, counters) — unchanged |
| Input-quality gate | Transport `ok`/`degraded`; reject hard faults (`unavailable`, `disconnected_suspected`, `noise_suspected`); allow `signal_present` **and** `unknown` |
| Acoustic activity | RMS enter/exit thresholds only (not `signal_present` alone) |
| Invalid / boot windows | Boot-settle ignored; invalid windows **hold** state (not silence); after `max_invalid_hold_ms` force END at last usable sample |

How `microphone_status` is determined: `dbmap_audio_classify_microphone()` uses structure diagnostics (zero ratio, unique values, rail hits)—see [`docs/diagnostics/ear-i2s-read-path-verification.md`](../../docs/diagnostics/ear-i2s-read-path-verification.md). It is **not** an activity detector and **not** `source_family`.

Thresholds are **provisional and uncalibrated**. One activity stream per Ear: overlapping sources are not separated.

### Emission timing (`duration_ms`)

Each Observation describes an already measured interval `[event_time_utc, event_time_utc + duration_ms)`:

| Kind | Interval |
| --- | --- |
| start | From first window that began the enter condition through the window that satisfied `min_activity_ms` |
| periodic | Since the previous emission |
| end | Remaining activity since the previous emission through episode end |

`signal_level_dbfs` is the RMS of the window that closed that interval (digital FS, not SPL). Not the simulator’s synthetic 850 ms duration.

### Boot vs Acoustic Monitor

Startup runs networking and the boot Desired/OTA check **before** starting the
Acoustic Monitor (I2S capture, RMS detector, L0). The monitor starts **once**
per boot after that gate. Later MQTT/Wi‑Fi disconnects and Desired polls do
**not** stop, restart, or reinitialize capture/detector/L0 state.

### Background RMS (Reported State, Iteration 1)

When `CONFIG_DBMAP_EAR_MIC_BACKGROUND` is enabled (default), each mic object on
hello/health/keepalive may include a nested `background` object: rolling RMS
over `CONFIG_DBMAP_EAR_MIC_BACKGROUND_WINDOW_MS` (default 180 s). Stats use
linear-mean energy → dBFS. `status` is `partial` or `full` from **elapsed
collection time**, not from sample count alone. Clipped windows and the first
post-I2S window (`window_seq == 1`, plus boot settle via
`ms_since_i2s_enable`) are excluded using existing fields. If no in-window
valid samples remain, `background` is omitted. History is independent of
Observation/MQTT; publish only reads a time-filtered snapshot and does not
clear the ring. RAM-only (no reboot persistence).

### MQTT unavailable

- Mic capture and the RMS detector task keep running.
- Emissions while MQTT is down are dropped (no offline queue); sequence numbers are not consumed for those drops.
- Network loss is not interpreted as absence of acoustic activity.

### OTA pre-reboot Observation closure

Before `persist_pending_and_reboot` continues, Ear runs a registered acoustic-close
callback (explicit registration from `app_main` after MQTT start and **before**
`dbmap_desired_boot_check`; Output never registers — NULL is an intentional no-op).

| Detector state at OTA close | `s_start_publish_accepted` | Action |
| --- | --- | --- |
| IDLE | — | No-op |
| PENDING_ENTER | (START never produced) | Silent → IDLE; no END |
| ACTIVE / PENDING_EXIT | true (START `publish_json` returned `ESP_OK`) | Forced END, `termination_reason: ota_reboot`, then IDLE |
| ACTIVE / PENDING_EXIT | false | Silent → IDLE; no orphan END |

`s_start_publish_accepted` means MQTT **client** acceptance only — not broker PUBACK and not Hub ingest. It is preserved across MQTT disconnect/reconnect and cleared on episode close or silent abort.

OTA END uses a detached snapshot (identity, topic, interval, full L0) taken under the lifecycle mutex; PUBACK wait (≤5000 ms) runs **outside** the mutex. OTA reboot proceeds even if END publish fails, MQTT is down, or PUBACK times out.

### Observation lifecycle scenarios

| Scenario | Detector / L0 | Emissions | `s_start_publish_accepted` | Sequence | Reconnect effect |
| --- | --- | --- | --- | --- | --- |
| MQTT down mid-episode | Continues | Dropped while down | Unchanged by disconnect | Not consumed on drops | No synthetic START; no acoustic reset |
| MQTT up → START accepted → down → OTA | Continues until OTA close | OTA attempts END | Stays true across disconnect | END allocates if publish path runs | — |
| START only while MQTT down, then OTA | Continues | No START accepted | false → OTA silent abort | No END seq | — |
| Natural END vs OTA race | Gate `CLOSING`/`CLOSED` stops further feed | At most one closer wins under mutex | Cleared on close | Per winner | — |
| OTA END PUBACK timeout / fail | Closed in RAM | Logged; reboot continues | Cleared | Consumed if allocate succeeded | — |
| Boot Desired OTA before Acoustic start | Not started | Close no-op | — | — | Hook already registered |

## Reported audio shape (excerpt)

```json
{
  "wifi_rssi_dbm": -62,
  "capabilities": {
    "role": "ear",
    "hardware_profile": "esp32-s3-n16r8",
    "microphones": [{ "index": 0, "model": "INMP441" }]
  },
  "audio": {
    "units": "dBFS",
    "subsystem_status": "ok",
    "audio_transport_status": "ok",
    "microphone_status": "signal_present",
    "sample_rate_hz": 16000,
    "sample_format": "int32_from_i2s32_left_justified_24",
    "microphones": [
      {
        "index": 0,
        "model": "INMP441",
        "channels": 1,
        "rms_dbfs": -42.5,
        "peak_dbfs": -28.1,
        "sample_count": 16000,
        "window_ms": 1000,
        "status": "ok",
        "microphone_status": "signal_present"
      }
    ]
  }
}
```

Levels are digital full-scale (**dBFS**), not calibrated dB SPL.

`audio_transport_status` (also mirrored as `subsystem_status` / `microphones[].status`) is **I2S fill health only**:

| Transport | Meaning |
|-----------|---------|
| `ok` | ≥50% of expected samples, no read errors/timeouts/empties |
| `degraded` | Partial fill and/or some stalls/read errors |
| `error` | No samples, missing data, or &lt;10% of expected samples |

`microphone_status` is **structure-based** (not a single dBFS threshold). Transport stays separate.

| Mic | Meaning |
|-----|---------|
| `signal_present` | Ambient-like energy: enough unique values, not zero-/rail-dominated |
| `noise_suspected` | Floating/noisy DIN (e.g. mostly zeros + rare full-scale hits) |
| `disconnected_suspected` | Digital silence / near-all-zero window |
| `unknown` | Transport usable but ambiguous |
| `unavailable` | Transport unusable / no samples |

Each mic also reports `classification_reason` plus diagnostics (no raw PCM): `raw_min`, `raw_max`, `zero_sample_count`, `full_scale_sample_count`, `unique_sample_count`, `unique_capped`, `consecutive_identical_samples`. See [`docs/diagnostics/ear-i2s-read-path-verification.md`](../../docs/diagnostics/ear-i2s-read-path-verification.md).

INMP441 extract remains `(int32_t)slot >> 8` (24-bit left-justified assumption); do not change without datasheet + scope confirmation.

`expected_sample_count` is published beside `sample_count`. Per-window read results stay **separate**: `timeout_reads`, `empty_reads`, `read_errors` (capped at 10 000 with `counters_capped`). `sample_count` counts only frames actually extracted from read buffers.

I2S stall / zero-sample investigation evidence (source vs telemetry vs inference vs hardware): [`docs/diagnostics/ear-i2s-read-path-verification.md`](../../docs/diagnostics/ear-i2s-read-path-verification.md). Do not fleet-OTA until EAR-003 passes that targeted test; removing an experimental `vTaskDelay(5)` on the stall path is **not** a proven root-cause fix.

## Hardware

Full wiring (Output + Ear, reserved 4-mic map): [`docs/device-wiring-and-pinout.md`](../../docs/device-wiring-and-pinout.md).

| Item | Value | Status |
|------|-------|--------|
| Hardware profile | `esp32-s3-n16r8` | **Verified** (ESPConnect): ESP32-S3 QFN56 rev v0.2, 16 MB flash (`0x184068`), 8 MB PSRAM (AP Memory 3.3 V), dual-core up to 240 MHz |
| Mic model | INMP441 | One model per Ear image |
| Mic index | **0** (0-based); **1** when `mic_count=2` | Indexes 2–3 reserved (DIN1 not read) |
| I2S pins | BCLK=4, WS=5, DIN0=6; DIN1=7 reserved | Placeholder — verify free GPIOs on this board |
| Slot alignment | 32-bit Philips; mono LEFT or stereo L/R; 24-bit left-justified `>> 8` | Unverified — confirm on scope/logic analyzer |
| L/R strap (mic 0 / 1) | Left (GND) / Right (VDD) | Unverified on board |
| Capture diag | `sdkconfig.defaults.capture_diag` | UART `capture_diag` lines; does not prove dual HW |

### Stage 1 stereo capture integrity (bench)

With `microphone_count=2`, health already reports both channels; Observation / activity still use mic 0. Stage 1 adds `dbmap_ear_mic_get_channel` / `dbmap_ear_mic_get_stream` for in-firmware diagnostics and documents a USB-console hardware procedure (1 kHz speaker move, 30 s continuity, power-off SD disconnect):

[`docs/diagnostics/ear-stereo-capture-integrity.md`](../../docs/diagnostics/ear-stereo-capture-integrity.md).

`sdkconfig.defaults` currently enables `CONFIG_DBMAP_EAR_MIC_CAPTURE_DIAG` so USB console shows dual-channel `capture_diag` (with `window_seq`) after `win#` stops at 30. Turn it off in menuconfig / defaults for quieter production fleet UART if unwanted. The `sdkconfig.defaults.capture_diag` overlay remains for rebuilds that start from a tree without the flag.

### Capture diagnostics (dual-mic Test A/B/C)

Build with the project defaults (capture diag on) or force the bench overlay:

```bash
cd firmware/esp-ear
idf.py fullclean   # needed if an existing sdkconfig was generated without the overlay
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.capture_diag" set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.capture_diag" build
```

After boot/OTA, confirm:

```text
ear_mic: capture_diag UART enabled (~5s): per-ch slot/rms/peak/n/expect/window_seq/mic/xport + stream incomplete (bench only; not fleet default)
```

Then ~every 5 s (raw values, unfiltered):

```text
ear_mic: capture_diag ... mode=stereo_lr live=2 ... incomplete=0 ...
ear_mic: capture_diag ch0 slot=left  n=16640 expect=16000 rms=-40.0 peak=-28.0 ... mic=signal_present xport=ok ... window_seq=42
ear_mic: capture_diag ch1 slot=right n=16640 expect=16000 rms=-52.0 peak=-40.0 ... mic=signal_present xport=ok ... window_seq=42
```

Do **not** use `ear_obs` `diag snap` for L/R identity (mic0 only). Procedure: [`docs/diagnostics/ear-stereo-capture-integrity.md`](../../docs/diagnostics/ear-stereo-capture-integrity.md).

## L0 analysis (optional Observation extension)

With `CONFIG_DBMAP_EAR_MIC_L0` / `CONFIG_DBMAP_EAR_OBS_L0` (default y), each 1 s window runs the L0 library; intervals finalize on START/PERIODIC/END (`start < window_end ≤ end`).

MQTT stays **`protocol_version: 1`**. Default images omit L0 fields. Enable `CONFIG_DBMAP_EAR_OBS_PUBLISH_L0` only after Hub accepts the extension — then every Observation includes required `emission_kind` + `measurements` (L0). Missing L0 / history overflow refuses publish. Contract: [`docs/ear-l0-measurement-contract.md`](../../docs/ear-l0-measurement-contract.md). Ring capacity: `CONFIG_DBMAP_EAR_OBS_L0_MAX_WINDOWS` (default 64 → ~62 s at 1 s windows).

## Opt-in serial diagnostics (RMS activity + L0)

`sdkconfig.defaults` currently enables rate-limited console diagnostics for local/field work (quiet-room vs fans, L0 inspection). The activity task prints even when MQTT/HTTPS are down. Disable `CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG` for production fleet images.

| Setting | Default | Effect |
| --- | --- | --- |
| `CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG` | `y` (local defaults) | ~1 Hz `diag snap` + ~5 s `diag counters` on UART; with L0 also `L0 win` / `L0 interval` / `L0 perf`. Turn off for fleet images. |

**Enable (local IDF):**

```bash
cd firmware/esp-ear
# Cleanish opt-in: merge defaults + serial_diag overlay (reconfigure if sdkconfig already exists)
idf.py fullclean   # optional; needed if switching an existing build tree
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.serial_diag" set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.serial_diag" build
# or: idf.py menuconfig → "dBmap Ear Observation" → "Opt-in serial diagnostics…"
```

Do **not** ship this overlay in production fleet images. Thresholds, hysteresis, timing, and Observation emission are unchanged.

**`diag snap` fields (~1 Hz)**

| Field | Meaning |
| --- | --- |
| `mono_us` | `esp_timer` monotonic microseconds (uptime clock) |
| `rms_dbfs` / `peak_dbfs` | Latest completed mic window (digital FS); `n/a` if no window yet |
| `xport` / `mic` | `transport_status` / `microphone_status` from that window |
| `state` | Detector: `IDLE` / `PENDING_ENTER` / `ACTIVE` / `PENDING_EXIT` |
| `enter` / `exit` | Configured RMS thresholds (dBFS) |
| `win_valid` | `1` if a completed window is available |
| `usable` | `1` if the window passes the existing input-quality gate (`dbmap_activity_sample_usable`) |
| `win_seq` | Mic window sequence (when valid) |

**`diag counters` fields (~5 s)** — existing detector / publisher counters (no new MQTT):

| Field | Source |
| --- | --- |
| `start` / `periodic` / `end` | `emissions_start` / `emissions_periodic` / `emissions_end` |
| `invalid` | `windows_invalid` (unusable windows) |
| `forced_end_invalid` | `forced_ends_invalid` |
| `mqtt_drop` | `s_dropped_while_mqtt_down` (emission dropped; no seq consumed) |
| `windows` / `boot_settle` | `windows_seen` / `windows_boot_settle` |

**Capture**

```bash
idf.py -p /dev/ttyUSB0 flash monitor
# or: screen /dev/ttyUSB0 115200
# Filter: grep 'ear_obs: diag'
```

Expect continuous `diag snap` / `diag counters` lines with Wi-Fi/MQTT disconnected. No credentials are logged; no raw PCM.

## Build

Prefer the Hub **Firmware → Build reusable firmware** target `ear-esp32-s3` (series `0.7.*`).

The builder container has **no internet**. Ear vendors `espressif/mdns` **1.14.1** at `esp-ear/components/mdns` (local IDF component) so the registry is never contacted. Ear components live under `firmware/ear_components/` (not shared with Output’s Helix/LittleFS tree under `firmware/output_components/`).

After builder `server.py` / component-tree changes, recreate with a **rebuild**:

```bash
docker compose --profile firmware-build up -d --build --force-recreate firmware-builder nginx
```

Local IDF: place the Hub CA at `main/root_ca.pem` (gitignored `*.pem`, same as Output), then:

```bash
cd firmware/esp-ear
idf.py set-target esp32s3
idf.py build
```
