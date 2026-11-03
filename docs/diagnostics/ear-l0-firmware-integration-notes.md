# Ear L0 firmware integration notes (archived)

**Archived work note** (validation/CPU notes from bring-up). Living protocol
contract: [`../ear-l0-measurement-contract.md`](../ear-l0-measurement-contract.md).
Ear README covers current cutover flags.

**Status at archival:** L0 on-device + optional MQTT extension shipped; Hub
accepts legacy and extended v1. `PUBLISH_L0` defaults may differ from the text
below—check `firmware/esp-ear/sdkconfig.defaults`.

**Library:** `acoustic_l0.c`, `dbmap_acoustic_l0.h`

## What changed

| Area | Behaviour |
| --- | --- |
| I2S capture + RMS detector | Unchanged (thresholds, hysteresis, invalid-hold, ~5 s PERIODIC) |
| MQTT Observation payload | Protocol **v1**; optional L0 via `CONFIG_DBMAP_EAR_OBS_PUBLISH_L0` (default n) |
| L0 per window | Optional (`CONFIG_DBMAP_EAR_MIC_L0`, default y): double-buffer PCM in SPIRAM during capture; `dbmap_ear_mic_l0_poll()` runs `dbmap_l0_analyze_window` off the I2S task |
| L0 per Observation | Optional (`CONFIG_DBMAP_EAR_OBS_L0`): ring of usable windows; finalize on START/PERIODIC/END using Phase A interval association |
| Filter state | Reset-per-window (Phase A); no continuous IIR |
| Serial diagnostics | Still opt-in (`CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG`, default n); when on with L0, prints per-window and per-interval L0 plus CPU/RAM counters |

## Architecture

```
ear_mic task (prio 5)          ear_obs task (prio 4)
  measure_window                 get_levels → detector_feed  (unchanged path)
    RMS/peak as before           l0_poll → analyze pending PCM (SPIRAM)
    store PCM → pending slot     push usable L0 into ring
    flip double-buffer           on emission: interval finalize (serial only)
```

L0 is **not** consulted for enter/exit. Detector feed runs before L0 work each loop.

## Accumulator sizing

`EAR_OBS_L0_HIST_CAP = CONFIG_DBMAP_EAR_OBS_L0_MAX_WINDOWS` (default **64**, range 8–128).  
Windows needed for the configured PERIODIC interval: `INTERVAL_MS / WINDOW_MS + 2`.  
Defaults (5000 / 1000) need 7 ≤ 64. Max supported interval at cap 64 ≈ **62 s**.  
If needed > cap and `PUBLISH_L0=y`, observation start fails with `ESP_ERR_INVALID_SIZE` (no silent truncation). Runtime hist overflow clears T1–T6 and refuses L0 publish.

## Build

```bash
cd firmware/esp-ear
# Normal image (L0 on, serial diag off):
idf.py build

# Local validation with serial L0 dumps:
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.serial_diag" fullclean build
idf.py -p PORT flash monitor
```

Host Phase A tests (regression):

```bash
cc -std=c11 -Wall -Wextra -O2 -g \
  -I firmware/ear_components/ear_mic/include \
  firmware/ear_components/ear_mic/acoustic_l0.c \
  firmware/ear_components/ear_mic/test_host/test_acoustic_l0.c \
  -lm -o /tmp/test_dbmap_acoustic_l0 && /tmp/test_dbmap_acoustic_l0
```

## Serial markers (diag build)

| Log prefix | Meaning |
| --- | --- |
| `ear_mic: L0 ready pcm_cap=…` | SPIRAM double-buffer allocated |
| `ear_obs: L0 win rms=…` | Per-window L0 (~1 Hz with other snaps) |
| `ear_obs: L0 interval kind=…` | Observation-boundary aggregates (T1/T2/T3/T6, bands) |
| `ear_obs: L0 perf analyze_n=…` | On-device analyze µs, overwrite count, heap/SPIRAM at init |
| `ear_obs: diag counters …` | Existing detector counters (cadence check) |

## Validation checklist (EAR-001)

1. Build succeeds for `esp32s3` + SPIRAM.
2. Detector counters / ~5 s PERIODIC unchanged vs pre-Phase-B baseline.
3. With serial diag: L0 window + interval lines present; no L0 keys in MQTT JSON.
4. `pending_overwrite` stays 0 under continuous capture (activity task keeps up).
5. `analyze_us_max` recorded; I2S `timeout_reads` / `read_errors` not regressing.
6. Indoor stimuli (silence, tones, impulse, clip) for functional L0 only — do **not** retune outdoor thresholds.

## On-device evidence (EAR-001, 2026-11-02)

Local flash of Phase B + `SERIAL_DIAG=y` (Desired OTA cleared for the run, then restored to `0.5.21`).

| Metric | Result |
| --- | --- |
| Target build | `esp32s3`, app ≈ 0x100140 B with diag / 0xff500 without; partition 0x1c0000 |
| Host Phase A tests | `test_acoustic_l0` → `ok` |
| L0 PCM | SPIRAM double-buffer `pcm_cap=16176`, `pcm_bytes=129408` |
| Heap after L0 init | `free_heap≈8.46 MB`, `free_spiram≈8.25 MB` |
| Analyze CPU | steady `us_last≈160–169 ms` / window; `us_max≈169 ms` (no OTA); `overwrite=0` over 89 windows |
| I2S | `timeout=0`, `partial=0`, full ~16640 frames/window after boot; no short-window under L0 alone |
| Detector | enter=-45 / exit=-55 unchanged; boot_settle=4; invalid=0 |
| Cadence | `start` then `periodic` every ≈5 windows (≈5 s); `end` observed; matching `L0 interval kind=…` |
| MQTT | protocol v1 only (no L0 fields added) |

Example serial (abridged):

```
ear_mic: L0 ready pcm_cap=16176 bytes=129408 free_heap=8458520 free_spiram=8251648
ear_obs: L0 win rms=-51.68 peak=-41.69 crest=9.99 clip=0 B0=… hl=-6.33 n=16176 …
ear_obs: L0 interval kind=periodic win=5 … T1_stdev=… T2=… T3_… T6=… t=[…,… ) us
ear_obs: L0 perf analyze_n=89 us_last=169169 us_max=169169 overwrite=0 …
ear_obs: diag counters start=1 periodic=10 end=1 invalid=0 … windows=89 boot_settle=4
```

Indoor functional only — thresholds not retuned.

## Deviations from Phase A

None. Integration calls the existing `dbmap_l0_*` API as-is (reset-per-window, same formulas). Firmware-only addition: SPIRAM PCM staging + observation ring + optional serial prints.

## Remaining risks / recommendations

1. **CPU cost:** ~165 ms of soft-float/`double` work per 1 s window on ESP32-S3 keeps up (`overwrite=0`) but is heavy. A future authorized float-path or continuous-IIR streaming optimize should be a separate change (would touch Phase A).
2. **OTA during validation:** Hub Desired will replace a local Phase B image; pin/clear Desired when flashing experimental builds.
3. **Concurrent OTA + L0:** first flash attempt showed I2S short windows and `overwrite` while HTTPS OTA ran — re-validate L0 after Desired is quiet.
4. **Stimuli coverage:** ambient indoor episode exercised start/periodic/end + crest/clip flags; dedicated tone/impulse/clip lab passes still useful before Phase C.
5. Checkpoint C (Hub/sim optional v1 extension) is authorized separately; keep physical `PUBLISH_L0` off until operator cutover.

## Out of scope (still)

Protocol version bump, L1 signatures, classification, raw audio storage, continuous IIR state, detector threshold changes. (Checkpoint C covers Hub/API/simulator optional L0 on protocol v1.)
