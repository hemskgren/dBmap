# Relative END v1 — activity detector design

**Status:** Implemented in firmware; **enabled by default** for EAR field validation (`DBMAP_EAR_OBS_RELATIVE_END=y` in Kconfig and `firmware/esp-ear/sdkconfig.defaults`). Parameters are **provisional field-test values**, not validated production thresholds.  
**Scope:** RMS activity lifecycle only. Does not change L0/L1/L2 calculations or Observation payload schemas.

## Problem

Absolute exit (`rms < -55 dBFS`) cannot end an episode when ambient RMS sits above −55 dBFS (observed on EAR-001 ≈ −51 dBFS after a tone). Relative END freezes a pre-event baseline and ends when level returns near that baseline.

## Enabling

| Kconfig | Default | Meaning |
| --- | --- | --- |
| `DBMAP_EAR_OBS_RELATIVE_END` | `y` | Master switch; when off, absolute-only behavior is unchanged |

`firmware/esp-ear/sdkconfig.defaults` also sets `CONFIG_DBMAP_EAR_OBS_RELATIVE_END=y` so firmware-builder / Hub catalog metadata and fresh ESP-IDF configures do not leave it disabled. Absolute thresholds and hold times are unchanged; absolute END remains the fallback when Relative END cannot arm.

### Provisional parameters (field-test only)

| Kconfig | Default | Meaning |
| --- | --- | --- |
| `DBMAP_EAR_OBS_REL_RETURN_MARGIN_DB_X1000` | 4000 (4.0 dB) | Relative entry: `rms <= baseline + margin` |
| `DBMAP_EAR_OBS_REL_EXIT_HOLD_MS` | 2000 | Hold while relative mode latched |
| `DBMAP_EAR_OBS_REL_CANCEL_HYST_DB_X1000` | 1500 (1.5 dB) | Cancel when `rms > baseline + margin + hyst` |
| `DBMAP_EAR_OBS_REL_MIN_CONTRAST_DB_X1000` | 5000 (5.0 dB) | Arm when max(first two episode RMS) − baseline ≥ contrast |
| `DBMAP_EAR_OBS_REL_BASELINE_STALE_MS` | 15000 | Drop IDLE candidates older than this before freeze; 0 = off |

These are **not** calibrated fleet defaults. Retune only after controlled EAR-001 validation.

### Visibility (effective / compiled config)

| Path | What you see |
| --- | --- |
| Firmware-builder `GET …/dbmap-config` (effective) | Full Relative END keys under `dbmap_config.observation` (Kconfig defaults + `sdkconfig.defaults`) |
| Catalog / downloadable metadata `dbmap_config` | Defaults-file overrides only — includes `EAR_OBS_RELATIVE_END=true` when built from esp-ear defaults |
| Device hello/health `capabilities.observation` | Compiled `EAR_OBS_RELATIVE_END` plus margin / hold / hyst / contrast / stale ms when enabled |

## Baseline collection and freeze

- Collect up to **five** usable RMS samples only in **IDLE** while `rms < enter` (−45 dBFS).
- Do **not** collect in PENDING_ENTER, ACTIVE, or PENDING_EXIT.
- Store each sample’s `window_end` (`measured_at_us`).
- At **START**, freeze candidates with `t_end <= episode_start` (excludes the first above-enter window and later event windows).
- Baseline = **median**; **reliable** only if `n >= 3`.
- Apply staleness filter before freeze when configured.
- Unreliable / insufficient → Relative END stays unarmed; absolute exit only.
- After END (natural, invalid-hold, force_end, reset_idle): clear freeze, arming, latch, and the candidate ring; resume IDLE collection.

## Arming and contrast

- Evaluated **only in ACTIVE** (and the START window is counted via episode-start bookkeeping).
- Contrast = `max(RMS of first two usable episode windows) − frozen_baseline`.
- Arm when baseline is reliable and `contrast >= min_event_contrast_db`.
- Arming decision runs only once both of the first two windows are recorded.
- **Call order on ACTIVE:** update arming **before** exit-entry checks so a window that completes contrast and meets an exit condition enters PENDING_EXIT **already armed**.
- Do **not** arm while already in PENDING_EXIT.

## Exit-mode latch

At PENDING_EXIT entry, latch:

- `relative` if Relative END is armed at that instant;
- `absolute` otherwise.

The latch is fixed for the whole attempt (cancel rules + hold duration). Returning to ACTIVE clears the latch; arming may continue on later ACTIVE windows.

## PENDING_EXIT semantics

### Relative latch

| Action | Condition |
| --- | --- |
| Enter (from ACTIVE, armed) | `rms < -55` **OR** `rms <= baseline + return_margin` |
| Stay | `rms <= baseline + return_margin + cancel_hysteresis` |
| Cancel → ACTIVE | `rms > baseline + return_margin + cancel_hysteresis` |
| Hold | `RELATIVE_EXIT_HOLD_MS` |

Absolute cancel (`rms >= -55`) **must not** run while relative is latched.

### Absolute latch (unarmed / feature off)

| Action | Condition |
| --- | --- |
| Enter | `rms < -55` |
| Stay | `rms < -55` |
| Cancel | `rms >= -55` |
| Hold | existing `EXIT_HOLD_MS` |

## Natural END hold (absolute and relative)

Natural END no longer uses wall-clock time since `PENDING_EXIT` entry. Both latches share a continuity-aware accumulator:

- On enter `PENDING_EXIT`: elapsed = 0; anchor = current `(measured_at_us, window_seq)`.
- Only **qualifying usable stay** windows that are **sequence-consecutive** with the anchor and within `1.5 × window_ms` timestamp delta add to `exit_hold_elapsed_us`.
- Explicit unusable samples, forward `window_seq` gaps, backward sequences, and excessive timestamp jumps **invalidate the anchor** (pause). Accumulated elapsed is preserved; the next qualifying stay re-anchors with zero credit across the gap.
- Cancel → `ACTIVE` clears elapsed and anchor.
- `pending_since_us` remains diagnostic only and does not control natural END.
- `max_invalid_hold_ms` stays independent (still based on time since last usable sample).

## Invalid / unusable windows

- No exit entry, no cancel, no natural-END credit, no latch change.
- Hold anchor is invalidated (pause); accumulated hold progress is kept.
- Existing `max_invalid_hold_ms` forced END unchanged.
- Invalid windows are never treated as quiet RMS.
- Missing windows (no sample delivered) are detected via `window_seq` gaps inside the activity detector.

## Accepted v1 limitations

- Weak events that never meet contrast stay on absolute exit (may stick if ambient ≥ −55).
- No secondary ambient exit if background rises during an episode.
- An unarmed absolute PENDING_EXIT attempt does not become relative mid-attempt.
- Not a classifier; does not alter L0/L1/L2.

## Serial diagnostics (rate-limited)

With `CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG`, the existing ~1 Hz `ear_obs` snap still prints absolute `enter`/`exit` and SM `state`. A second line `diag rel …` adds Relative END runtime from `dbmap_activity_detector_fill_diag` / `dbmap_activity_format_rel_diag` (no independent recalculation):

- `armed`, frozen `base` / `base_n` / `base_ok`, `rel_exit` / `rel_cancel`, `contrast`
- latched exit mode (`latch`), configured hold (`hold_ms`), current and peak continuous hold (`hold_el_ms`, `hold_max_ms`)
- lifetime `pend_enter` / `pend_cancel` counters

When Relative END is compiled off: `rel_end=0 state=…`. Thresholds show `n/a` until a reliable baseline is frozen.

## Implementation pointers

- [`dbmap_activity_detector.h`](../../firmware/ear_components/ear_observation/include/dbmap_activity_detector.h)
- [`activity_detector.c`](../../firmware/ear_components/ear_observation/activity_detector.c)
- Host tests: `test_host/test_activity_relative_end.c`, `test_host/test_activity_detector.c` (absolute / disabled)
- Firmware-builder: `firmware-builder/tests/test_dbmap_config.py` (effective Relative END defaults)
