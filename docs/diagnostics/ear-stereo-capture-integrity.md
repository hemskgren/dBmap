# Ear Stage 1 — Stereo Capture Integrity

Bench procedure to verify dual-INMP441 capture on one Ear (DIN0 Philips stereo)
without changing Observation / activity behavior. Firmware exposes per-channel
and stream APIs (`dbmap_ear_mic_get_channel`, `dbmap_ear_mic_get_stream`); primary
`dbmap_ear_mic_get_levels()` remains mic 0 for the detector.

## Prerequisites

- EAR with two INMP441s: shared BCLK/WS/DIN0; mic 0 L/R → GND (left); mic 1 L/R → VDD (right). See [`docs/device-wiring-and-pinout.md`](../device-wiring-and-pinout.md).
- NVS `microphone_count=2`.
- USB console on a laptop.
- Existing 1 kHz WAV test tone and a speaker that can be moved next to each mic.
- Capture-diag UART enabled (`CONFIG_DBMAP_EAR_MIC_CAPTURE_DIAG` in
  `sdkconfig.defaults`, or the `sdkconfig.defaults.capture_diag` overlay).
  Without it, UART only shows mic0 `ear_obs` snaps and Test A cannot map L/R.

```bash
cd firmware/esp-ear
# Defaults already enable capture_diag; use overlay if rebuilding an older sdkconfig:
idf.py fullclean   # required if sdkconfig already exists without the flag
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.capture_diag" set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.capture_diag" build
```

After flash/OTA, **verify the image** before Test A: boot must log
`capture_diag UART enabled (~5s): …`. If that line is missing, dual-channel
UART is not compiled in — rebuild with defaults/overlay (do not rely on mic0
`ear_obs` snaps).

Watch UART for early `ear_mic` `win#…` lines (first ~30 windows) and periodic
`capture_diag` lines (~5 s). Per-channel lines include matching `window_seq`.

Do **not** use Observation `signal_level_dbfs` / `ear_obs` `diag snap` for
channel identity — they follow mic 0 only.

## Electrical safety (Test C)

| Do | Don’t |
| --- | --- |
| Power the Ear **fully off** before touching wires | Hot-plug / live disconnect |
| Open **only the SD (data)** lead of **one** mic | Short SD to 3V3 or GND |
| Insulate the open SD wire | Leave tinned ends near VDD |
| Leave the other mic fully wired (VDD, GND, SCK, WS, SD) | Disturb shared BCLK/WS/DIN0 of the live mic |
| Reboot after the wiring change | Expect `sample_count=0` on the open-SD channel |

Shared clocks still advance both stereo slots. An open SD typically yields
silence or floating noise on that slot while the firmware still extracts a
sample count. **`available_microphone_count` is not expected to fall** merely
because one SD data wire is disconnected — diagnose with per-channel
`microphone_status`, `channel_presence`, and RMS/peak.

## Test A — Physical channel identity

1. Boot with `mic_count=2` and capture diag enabled.
2. Play 1 kHz at moderate level.
3. Hold the speaker ~2–5 cm from **mic 0** for 10–15 s; note `capture_diag ch0` vs `ch1` RMS.
4. Move the speaker to **mic 1**; repeat.

**Pass**

- Near mic 0: `ch0` RMS clearly higher than `ch1` (about ≥6 dB).
- Near mic 1: `ch1` RMS clearly higher than `ch0`.
- Both channels report `n` near `expect`.

**Fail**

- Louder channel is the opposite index (L/R swap / strap error).
- Both channels stay within ~2 dB through both placements.
- One channel has `n≈0` while clocks should be running.

## Test B — Continuity (≥30 s)

1. Steady 1 kHz at mid-distance, or quiet room.
2. Collect ≥30 s of `capture_diag` (plus early `win#` lines).

**Pass**

- Each sampled window: `ch0` and `ch1` share the same `window_seq`.
- Both `n` within ~10% of `expect` for the run.
- `incomplete` usually 0; no sustained non-zero every window.
- Transport not stuck in `error`; no chronic `read_errors`.

**Fail**

- Divergent `window_seq` between channels.
- Repeated large underfill or one channel starving.
- Recurring incomplete stereo frames or persistent transport failure.

## Test C — One SD open (power off), reboot

1. Power off.
2. Disconnect **only** mic 1’s SD data wire; insulate it.
3. Power on; capture ≥20 s of console.
4. Optional second pass: restore mic 1, disconnect mic 0 SD instead.

**Pass**

- Live mic: healthy fill and plausible `signal_present` / `active` (or ambient-like) status.
- Open-SD mic: independently bad (`disconnected_suspected`, `noise_suspected`, and/or `silent`) — not filtered or hidden.
- Common `window_seq`; incomplete not newly chronic.
- Activity / Observation still follow mic 0 when mic 0 remains connected.

**Fail**

- Live mic dies because shared DIN/clocks were disturbed.
- Open-SD channel looks identical to the healthy channel.
- Detector path breaks solely because the secondary mic is open (when mic 0 is still good).

## API reference (firmware)

| API | Role |
| --- | --- |
| `dbmap_ear_mic_get_levels` | Primary mic 0 levels for Observation / activity (unchanged contract) |
| `dbmap_ear_mic_get_channel` | Per-channel diagnostics (index 0 = left, 1 = right when stereo) |
| `dbmap_ear_mic_get_stream` | live/configured/available counts, `slot_mode`, incomplete frames, common `window_seq` |

Host tests: `firmware/ear_components/ear_mic/test_host/test_ear_mic_channel_api.c`.

## Out of scope

Four-microphone capture, DIN1 / second I2S RX, PCM retention, TDOA/DOA, MQTT schema
changes, detector threshold or boot-order changes.
