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
| AE signature CSV capture | on (EAR `sdkconfig.defaults`) | `configured` | `CONFIG_DBMAP_EAR_AE_SIG_CAPTURE=y` in `firmware/esp-ear/sdkconfig.defaults`; Kconfig symbol default n; List key `EAR_AE_SIG_CAPTURE` | Bounded UART CSV for AS-001 calibration | kconfig |
| Capture buffer capacity | 48 | `configured` | `AE_SIG_CAPTURE_CAPACITY` in `ae_sig_capture.h` | Max stored AE windows per session | compile-time |
| Reported map key | `AE_SIG_CAPTURE` bool | `configured` | `capabilities.observation.AE_SIG_CAPTURE` in `ear_observation.c` | Build metadata visible on hello/health | compile-time |

---

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
