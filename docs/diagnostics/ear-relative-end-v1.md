# Relative END — activity detector design (v1 + v2 A3)

**Status:** Implemented in firmware; **enabled by default** for EAR field validation (`DBMAP_EAR_OBS_RELATIVE_END=y` and `DBMAP_EAR_OBS_REL_SECONDARY_AMBIENT=y` in Kconfig / `firmware/esp-ear/sdkconfig.defaults`). Parameters are **provisional field-test values**, not validated production thresholds.  
**Scope:** RMS activity lifecycle only. Does not change L0/L1/L2 calculations or Observation payload schemas.

## Problem

Absolute exit (`rms < -55 dBFS`) cannot end an episode when ambient RMS sits above −55 dBFS (observed on EAR-001 ≈ −51 dBFS after a tone). Relative END freezes a pre-event baseline and ends when level returns near that baseline.

**v2 A3 (Secondary Ambient Exit)** addresses a remaining field failure: a **quiet frozen baseline** followed by a **raised but stable ambient** (still above absolute exit and above primary relative entry) left episodes `ACTIVE` indefinitely under v1 alone.

## Enabling

| Kconfig | Default | Meaning |
| --- | --- | --- |
| `DBMAP_EAR_OBS_RELATIVE_END` | `y` | Master switch; when off, absolute-only behavior is unchanged |
| `DBMAP_EAR_OBS_REL_LATE_ARM` | `n` (Kconfig) / `y` in esp-ear `sdkconfig.defaults` | Late sustained contrast arm (requires Relative END) |
| `DBMAP_EAR_OBS_REL_SECONDARY_AMBIENT` | `y` | v2 A3 secondary ambient exit (requires Relative END) |

`firmware/esp-ear/sdkconfig.defaults` enables Relative END, late arm, and A3 for EAR field builds. Absolute thresholds and hold times are unchanged; absolute END remains the fallback when Relative END cannot arm.

### Provisional parameters (field-test only)

| Kconfig | Default | Meaning |
| --- | --- | --- |
| `DBMAP_EAR_OBS_REL_RETURN_MARGIN_DB_X1000` | 4000 (4.0 dB) | Relative / secondary entry: `rms <= reference + margin` |
| `DBMAP_EAR_OBS_REL_EXIT_HOLD_MS` | 2000 | Hold while relative **or secondary** mode latched |
| `DBMAP_EAR_OBS_REL_CANCEL_HYST_DB_X1000` | 1500 (1.5 dB) | Cancel when `rms > reference + margin + hyst` |
| `DBMAP_EAR_OBS_REL_MIN_CONTRAST_DB_X1000` | 5000 (5.0 dB) | Arm when max(first two episode RMS) − baseline ≥ contrast |
| `DBMAP_EAR_OBS_REL_BASELINE_STALE_MS` | 15000 | Drop IDLE candidates older than this before freeze; 0 = off |
| `DBMAP_EAR_OBS_REL_AMB_JUMP_DB_X1000` | 10000 (10.0 dB) | Secondary median must be ≥ frozen baseline + jump |
| `DBMAP_EAR_OBS_REL_AMB_STABLE_WINDOWS` | 5 | Consecutive below-enter windows for secondary stability |
| `DBMAP_EAR_OBS_REL_AMB_STABLE_RANGE_DB_X1000` | 3000 (3.0 dB) | Max−min across those windows |

These are **not** calibrated fleet defaults. Retune only after controlled EAR-001 validation.

### Visibility (effective / compiled config)

| Path | What you see |
| --- | --- |
| Firmware-builder `GET …/dbmap-config` (effective) | Full Relative END keys under `dbmap_config.observation` (Kconfig defaults + `sdkconfig.defaults`) |
| Catalog / downloadable metadata `dbmap_config` | Defaults-file overrides — includes `EAR_OBS_RELATIVE_END` / `EAR_OBS_REL_SECONDARY_AMBIENT` when set in esp-ear defaults |
| Device hello/health `capabilities.observation` | Compiled Relative END + A3 keys when enabled |

---

## v1 behavior (unchanged)

### Baseline collection and freeze

- Collect up to **five** usable RMS samples only in **IDLE** while `rms < enter` (−45 dBFS).
- Do **not** collect in PENDING_ENTER, ACTIVE, or PENDING_EXIT.
- Store each sample’s `window_end` (`measured_at_us`).
- At **START**, freeze candidates with `t_end <= episode_start` (excludes the first above-enter window and later event windows).
- Baseline = **median**; **reliable** only if `n >= 3`.
- Apply staleness filter before freeze when configured.
- Unreliable / insufficient → Relative END stays unarmed; absolute exit only; **A3 also inactive**.
- After END (natural, secondary ambient, invalid-hold, force_end, reset_idle): clear freeze, arming, latch, secondary ambient state, and the candidate ring; resume IDLE collection.

### Arming and contrast

- Evaluated **only in ACTIVE** (and the START window is counted via episode-start bookkeeping).
- Contrast = `max(RMS of first two usable episode windows) − frozen_baseline`.
- Arm when baseline is reliable and `contrast >= min_event_contrast_db`.
- Arming decision runs only once both of the first two windows are recorded.
- **Late sustained arm** (`DBMAP_EAR_OBS_REL_LATE_ARM`, default off): if still unarmed, arm after **two consecutive** usable ACTIVE windows each satisfying `rms >= frozen_baseline + min_event_contrast`. Reset the streak on unusable samples, usable below-contrast windows, and forward `window_seq` gaps while ACTIVE. No streak updates in PENDING_EXIT. Once armed (init or late), stays armed for the episode.
- **Call order on ACTIVE:** first-two arm, optional late arm, then A3 ambient update, then exit-entry checks.
- Do **not** arm while already in PENDING_EXIT.
- Diagnostics: `late_arm`, `arm_src=none|init|late`, `c_init`, `c_late` on `diag rel` / `dbmap_activity_diag_t`.

### Exit-mode latch (v1 + A3)

At PENDING_EXIT entry, latch follows the **entry cause**:

- `relative` if Relative END is armed **and** the current window meets primary relative entry
  (`rms <= frozen_baseline + return_margin`);
- `secondary` if armed **and** secondary ambient is valid **and**
  `rms <= secondary_ambient + return_margin` (and primary relative entry is **not** met);
- `absolute` otherwise — including when armed but the window entered only via absolute exit
  (`rms < exit_threshold`) without meeting relative or secondary entry.

When primary relative and secondary entry are both true, the latch is **`relative`** (primary wins).
The latch is fixed for the whole attempt. Returning to ACTIVE clears the latch; arming may continue on later ACTIVE windows. Secondary validity may persist across a cancel until END/cleanup.

### PENDING_EXIT semantics

#### Relative latch

| Action | Condition |
| --- | --- |
| Enter (from ACTIVE, armed) | `rms < -55` **OR** `rms <= baseline + return_margin` **OR** (A3 secondary entry) |
| Stay | `rms <= baseline + return_margin + cancel_hysteresis` |
| Cancel → ACTIVE | `rms > baseline + return_margin + cancel_hysteresis` |
| Hold | `RELATIVE_EXIT_HOLD_MS` |

#### Secondary latch (v2 A3)

| Action | Condition |
| --- | --- |
| Enter (from ACTIVE, armed, `secondary_ambient_valid`) | `rms <= secondary_ambient + return_margin` |
| Stay | `rms <= secondary_ambient + return_margin + cancel_hysteresis` |
| Cancel → ACTIVE | `rms > secondary_ambient + return_margin + cancel_hysteresis` |
| Hold | same as relative (`RELATIVE_EXIT_HOLD_MS`) |

#### Absolute latch (unarmed / feature off / absolute-only entry)

| Action | Condition |
| --- | --- |
| Enter | `rms < -55` |
| Stay | `rms < -55` |
| Cancel | `rms >= -55` |
| Hold | existing `EXIT_HOLD_MS` |

### Natural END hold

Natural END uses a continuity-aware accumulator (absolute, relative, and secondary):

- On enter `PENDING_EXIT`: elapsed = 0; anchor = current `(measured_at_us, window_seq)`.
- Only **qualifying usable stay** windows that are **sequence-consecutive** with the anchor and within `1.5 × window_ms` timestamp delta add to `exit_hold_elapsed_us`.
- Explicit unusable samples, forward `window_seq` gaps, backward sequences, and excessive timestamp jumps **invalidate the anchor** (pause). Accumulated elapsed is preserved; the next qualifying stay re-anchors with zero credit across the gap.
- Cancel → `ACTIVE` clears elapsed and anchor.
- `pending_since_us` remains diagnostic only and does not control natural END.
- `max_invalid_hold_ms` stays independent (still based on time since last usable sample).

### Invalid / unusable windows

- No exit entry, no cancel, no natural-END credit, no latch change, no A3 stability credit.
- Hold anchor is invalidated (pause); accumulated hold progress is kept.
- Existing `max_invalid_hold_ms` forced END unchanged.
- Invalid windows are never treated as quiet RMS.

---

## v2 A3 — Secondary Ambient Exit

### Algorithm (matches the offline design study)

While `ACTIVE` or `PENDING_EXIT`, after arming update:

1. Require `relative_end_enabled`, `secondary_ambient_enabled`, **frozen reliable baseline**.
2. If `rms >= enter` (−45): reset the stable-window accumulator (loud source must not seed ambient).
3. Else push `rms` into a ring of `ambient_stable_windows` (default 5).
4. When the ring is full: if `(max − min) <= ambient_stable_range_db` (default 3 dB) **and**
   `(median − frozen_baseline) >= ambient_jump_db` (default 10 dB), set
   `secondary_ambient_valid` with `secondary_ambient_dbfs = median`.
5. **Never** rewrite `frozen_baseline_dbfs`.

Secondary entry/cancel use the same margin/hysteresis as primary relative, but against `secondary_ambient_dbfs`.

### Diagnostic identity

| Path | How it is identified |
| --- | --- |
| Detector emission | `termination = SECONDARY_AMBIENT`, `end_latch = secondary` |
| MQTT `termination_reason` | Still `natural` (protocol allow-list); distinguish via serial/diag latch until Hub contract expands |
| Serial `diag rel …` | `sec=1 sec_base=… sec_exit=… sec_cancel=…` and `latch=secondary` |

### Limitations / residual risks

- **RMS cannot tell ambient from a continuing quiet source.** A genuine sustained source that settles stably below enter and ≥ jump above the freeze (e.g. −50 after a −70 freeze) will be ended by A3. This is an accepted trade-off for closing quiet-baseline → raised-ambient sticks.
- A3 does **not** fix: stale/unreliable baseline, or weak first-two contrast when late arm is disabled.
- With late arm enabled, a single loud spike still does not arm (needs two consecutive qualifying windows); T15-class sticks remain possible.
- A3 does **not** activate when Relative END is unarmed or the freeze is unreliable.
- Raising `return_margin` to fix sticks remains unsafe (oscillation premature END); A3 avoids that retune.

### Accepted remaining v1 limitations (still true)

- Weak events that never meet contrast stay on absolute exit (may stick if ambient ≥ −55).
- An unarmed absolute PENDING_EXIT attempt does not become relative/secondary mid-attempt.
- Not a classifier; does not alter L0/L1/L2.

## Serial diagnostics (rate-limited)

With `CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG`, the existing ~1 Hz `ear_obs` snap still prints absolute `enter`/`exit` and SM `state`. A second line `diag rel …` adds Relative END + A3 runtime from `dbmap_activity_detector_fill_diag` / `dbmap_activity_format_rel_diag`:

- `armed`, frozen `base` / `base_n` / `base_ok`, `rel_exit` / `rel_cancel`, `contrast`
- `sec` / `sec_base` / `sec_exit` / `sec_cancel` when A3 is enabled
- latched exit mode (`latch` = `absolute` / `relative` / `secondary` / `none`)
- hold / pending counters

## Implementation pointers

- [`dbmap_activity_detector.h`](../../firmware/ear_components/ear_observation/include/dbmap_activity_detector.h)
- [`activity_detector.c`](../../firmware/ear_components/ear_observation/activity_detector.c)
- Host tests: `test_host/test_activity_relative_end.c`, `test_host/test_activity_detector.c`, `test_host/test_activity_secondary_ambient.c`
- Firmware-builder: `firmware-builder/tests/test_dbmap_config.py` (effective Relative END defaults)
