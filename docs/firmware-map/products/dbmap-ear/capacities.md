# dBmap EAR capacities and limits (source-verified)

Composition: EAR × `esp32-s3-n16r8` × builder `ear-esp32-s3` × IDF `v5.4.4`.

Value classes: [schema.md](../../schema.md).  
**None of these rows are `verified_operating` performance limits** unless a
[performance record](../../records/) says so.

---

## Platform Observation registry

| Item | Value | Class | Source | Controls | Binding |
| --- | --- | --- | --- | --- | --- |
| Concurrent episode slots | 2 | `configured` (+ Stage‑1 `architectural_limit`) | `SP_OBS_EPISODE_SLOTS` in `firmware/platform/observation/include/sp_episode_registry.h` | Max simultaneous Platform episode slots | compile-time |

---

## EAR Observation Manager

| Item | Value | Class | Source | Controls | Binding |
| --- | --- | --- | --- | --- | --- |
| Open bindings (active Obs) | 2 | `configured` | `EAR_OBS_MGR_BINDING_CAP` in `ear_obs_manager.h` | Max bound Observations in Manager | compile-time |
| Reservation slot array | 2 | `configured` | `EAR_OBS_MGR_RSV_SLOTS` | HELD/PENDING/BOUND reservation records | compile-time |
| Pending queue capacity | 2 | `configured` | `EAR_OBS_MGR_PENDING_CAP` | Max pending promotions | compile-time |
| Admission: held + pending | ≤ 2 | `configured` | `ear_obs_manager_reserve` uses `held + pending >= EAR_OBS_MGR_PENDING_CAP` → full (`ear_obs_manager.c`) | Joint reservation budget | compile-time policy |
| Pending timeout default | 30000 ms | `configured` | `EAR_OBS_PENDING_TIMEOUT_MS_DEFAULT` in `ear_obs_manager.h`; applied in manager init / `ear_observation.c` | Expire pending promote wait | compile-time default |
| Promote / live evidence signature ring | ≤ 3 | `configured` | `EAR_OBS_PROMOTE_SIG_CAP` (default 3) in `ear_obs_manager.h` | ENTER seeds from AE history; post-promote live updates append candidate-associated by-value copies; newest at `[0]`, FIFO drop oldest when full | compile-time |
| Detached Obs END watchdog | 60000 ms | `configured` | `EAR_OBS_DETACHED_END_TIMEOUT_MS_DEFAULT` / `CONFIG_DBMAP_EAR_OBS_DETACHED_END_TIMEOUT_MS` | After matching CE EXIT on a non-AS002 open binding, OM ends that binding if still open && !ce_live when hold elapses; PERIODIC does not reset; AS002 excluded | kconfig / init-default |

**Note:** Manager binding cap and Platform `SP_OBS_EPISODE_SLOTS` are both 2 and
are checked together on promote/open paths, but they are **different symbols**.
Do not assume changing one updates the other.

“Held reservations = 2” refers to `EAR_OBS_MGR_RSV_SLOTS` / joint admission with
pending (above). It is not a third independent pool of size 2 beyond pending.

---

## Candidate Engine (CE)

| Item | Value | Class | Source | Controls | Binding |
| --- | --- | --- | --- | --- | --- |
| Candidate slots | 8 | `configured` | `CE_MAX_CANDIDATES` in `ce_engine.h` | Max simultaneous Candidates | compile-time |
| `n_enter` | 2 | `configured` (init default) | `ce_config_set_defaults` in `ce_engine.c` | NASCENT → ACTIVE evidence windows | init-default |
| `t_coast` | 3 | `configured` (init default) | same | COASTING windows before expire | init-default |
| `match_distance_max` | 8.0 | `configured` (init default) | same | Rematch if identity distance ≤ this | init-default |
| `match_hysteresis` | 2.0 | `configured` (init default) | same | Create only if distance > max + hysteresis | init-default |
| `match_ambiguity_delta` | 1.5 | `configured` (init default) | same | Ambiguous if second−best < this within match | init-default |
| `min_rms_dbfs` | −80.0 | `configured` (init default) | same | **Create** energy gate for new NASCENT only | init-default |

**Not Observation capacity.** CE create floor is **not** the RMS Activity
Detector ENTER/EXIT pair. Identity matching uses relative band shape +
spectral/pair when valid (CE-local distance); RMS weight is reduced and is
not an identity or promotion threshold.

---

## Audio Engine (AE)

| Item | Value | Class | Source | Controls | Binding |
| --- | --- | --- | --- | --- | --- |
| Signature history capacity | 32 | `configured` | `AE_HISTORY_CAPACITY` in `ae_history.h`; hint in `ae_config_set_defaults` | Global rolling signature ring | compile-time |
| AE signature CSV capture | on with EAR target defaults, opt-in by named profile | `configured` | `CONFIG_DBMAP_EAR_AE_SIG_CAPTURE=y` in `firmware/esp-ear/sdkconfig.defaults`; Kconfig symbol default n; List key `EAR_AE_SIG_CAPTURE` | Bounded UART CSV for AS-001 calibration | kconfig |
| Capture buffer capacity | 48 | `configured` | `AE_SIG_CAPTURE_CAPACITY` in `ae_sig_capture.h` | Max stored AE windows per session | compile-time |
| Reported map key | `AE_SIG_CAPTURE` bool | `configured` | `capabilities.observation.AE_SIG_CAPTURE` in `ear_observation.c` | Build metadata visible on hello/health | compile-time |
| USB AS-001/AS-002 decision diagnostic | on with EAR target defaults | `configured` | `CONFIG_DBMAP_EAR_OBS_DECISION_DIAG`; List config `EAR_OBS_DECISION_DIAG` | Actual profile gates, pre-evaluation background and siren streak | kconfig |
| USB Candidate and Policy decision diagnostic | on with EAR target defaults (same switch) | `configured` | `CONFIG_DBMAP_EAR_OBS_DECISION_DIAG` | AE sequence, CE association, Candidate eligibility, ownership and Policy outcome | kconfig |

For the USB decision diagnostic, both Audio Engine and RMS activity must be
enabled. Kconfig defaults to off, and the EAR target enables it in
`firmware/esp-ear/sdkconfig.defaults`. **List config** reports the
resolved builder configuration under `config.product.dbmap.observation`.
Check the generated `sdkconfig` for a locally built image that uses the bench
overlay. The flag determines whether the logs are compiled in. There is no
diagnostic-specific runtime switch or live Hub state. See the
[USB capture and interpretation procedure](../../../ear-usb-console.md#ear-signature-and-promotion-decisions).

---

## Analysis PCM handoff

| Item | Value | Class | Source |
| --- | --- | --- | --- |
| PCM banks / pending slots / outstanding leases | 2 / 1 / 1 | `configured` | `firmware/platform/mic/sp_mic_window_job.c` |
| Retained capacity | 16,000 int32 samples per channel per bank at 16 kHz and 1,000 ms | `configured` | `sp_mic_start()` in `firmware/platform/mic/ear_mic.c` |
| Consumers | AE and AE-disabled legacy acoustic analysis release immediately after PCM access | `configured` | `ae_device_poll()` and `dbmap_ear_mic_l0_poll()` |
| P4/P5 USB flags | `SP_MIC_CAPTURE_DIAG=y`, `DBMAP_EAR_AUDIO_ENGINE=y`, `DBMAP_EAR_AE_DIAG=y`, `DBMAP_EAR_AE_DIAG_EVERY_N=10`, `DBMAP_EAR_OBS_SERIAL_DIAG=y`; FreeRTOS runtime stats disabled | `configured` | EAR target defaults and ESP-IDF v5.4.4 Kconfig. [USB baseline](../../../ear-usb-console.md#p4-capture-and-pcm-integrity-baseline) |

Named diagnostic profiles on the same EAR builder target select overlays
without changing the default target or physical microphone support. See
[the exact profile settings and provenance](../../../device-registration-and-deployment.md#ear-diagnostic-build-profiles).
The optional CSV capture owns a 48-record array plus capture state and
capture-local classifier background. These symbols account for 5,028 bytes
in a pre-P5 ESP32-S3 ELF. Confirm section savings for the current build with
a matched CSV-on/off pair.

Bank ownership is shared across the retained channel group. P6 uses a configured
frame target and keeps the unread suffix of each DIN batch in its existing
scratch. At 16 kHz and 1,000 ms each channel needs 128,000 PSRAM bytes across
both banks. Mono/stereo total 128,000/256,000 bytes. Logical 3/4-channel
totals are 384,000/512,000 bytes and need physical acquisition verification.
The P5 capacity was 129,408 bytes per channel across both banks.
[P6 verification procedure](../../../ear-usb-console.md#p6-frame-bounded-pcm-acceptance)

## Microphone acquisition (Kconfig defaults)

| Item | Value | Class | Source | Controls | Binding |
| --- | --- | --- | --- | --- | --- |
| Sample rate | 16000 Hz | `configured` | `CONFIG_SP_MIC_SAMPLE_RATE_HZ` default in `firmware/platform/mic/Kconfig` | PCM / analysis rate | kconfig default |
| Analysis window | 1000 ms | `configured` | `CONFIG_SP_MIC_WINDOW_MS` default | Window length | kconfig default |

---

## RMS Activity Detector / Observation cadence (Kconfig)

| Item | Value | Class | Source | Controls | Binding |
| --- | --- | --- | --- | --- | --- |
| ENTER threshold | −45.0 dBFS | `configured` | `DBMAP_EAR_OBS_ENTER_THRESHOLD_DBFS_X1000` default `-45000` in `ear_observation/Kconfig` | Aggregate activity enter | kconfig default |
| EXIT threshold | −55.0 dBFS | `configured` | `DBMAP_EAR_OBS_EXIT_THRESHOLD_DBFS_X1000` default `-55000` | Aggregate activity exit (absolute) | kconfig default |
| PERIODIC interval | 5000 ms | `configured` | `DBMAP_EAR_OBS_INTERVAL_MS` default `5000` | PERIODIC while detector ACTIVE (and Manager interval) | kconfig default |

Kconfig help text marks ENTER/EXIT as provisional / not calibrated — still
`configured` for what firmware uses; **not** `verified_operating`.

---

## ESP-IDF / memory (composition reminders)

| Item | Value | Class | See |
| --- | --- | --- | --- |
| Primary ESP-IDF | v5.4.4 | `configured` | [idf/v5.4.4.md](../../idf/v5.4.4.md) |
| Flash (build) | 16 MB | `configured` | [hardware profile](../../hardware/esp32-s3-n16r8.md) |
| PSRAM (board name / ESPConnect) | 8 MB | board-verified citation | same |

---

## Proposed architecture (ADR 0003) — not configured

Source: [ADR 0003](../../../adr/0003-critical-observation-path-and-shared-evidence.md),
[`CRITICAL_PATH.md`](../../../../firmware/products/dbmap/ear/ear_observation/CRITICAL_PATH.md).

| Item | Value | Class | Notes |
| --- | --- | --- | --- |
| Active Critical Observation capacity | 1 per EAR | `configured` (source) | `ear_critical_path`; second trigger dropped + counted |
| Overflow while active | Option A: drop + diag | `configured` (source) | No queue / snapshot buffer in this slice |
| Critical uses Platform registry slot | no | `configured` (source) | UUID via `format_uuid_v4` without registry allocate |
| Critical depends on CE / free normal slot | no | `configured` (source) | Independent binding |
| Automatic acoustic START | **disabled** | `configured` (source) | No production trigger; `force_start` for tests |
| Idle / max / PERIODIC | 15 s / 30 s / 5 s | `configured` defaults | Kconfig; PERIODIC 5 s is provisional |
| Idle-reset match D / crest floor | 4.0 / 12.0 | experimental | Idle-reset only — not approved detectors |
| Report origin mark | `context.origin=critical_path` | `configured` (Hub+payload) | Hub accepts; normal reports omit |
| Pending-event capacity | unresolved | unresolved | Not in this slice |
| DoA interval | — | not implemented | — |
| Analysis Scheduler / Resource Manager | — | not implemented | — |

## Not implemented / deferred

| Item | Status |
| --- | --- |
| Approved acoustic Critical detector / thresholds | Absent (hard-disabled) |
| DoA / TDOA | Absent; ΔRMS/Δpeak ≠ direction |
| Overflow evidence snapshots (ADR option B) | Absent (MVP uses A) |
| Analysis Scheduler / Resource Manager | Absent |
| Device / Hub MQTT Critical soak verification | Outstanding |

---

## Investigation checklist (M2)

All investigation targets from the Map task were traced to source as above.
No target was copied without a citation. Remaining composition caveats:

1. Product CPU is **240 MHz** / FreeRTOS **100 Hz** in
   `firmware/esp-ear/sdkconfig.defaults` (see hardware profile). Historical R0
   160 MHz builds use `sdkconfig.defaults.r0_ref` only.
2. Fleet `firmware_version` is builder/catalog-bumped; defaults file series base
   is not the sole runtime identity for a given device image.
