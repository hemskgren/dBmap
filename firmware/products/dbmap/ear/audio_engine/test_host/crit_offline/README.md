# Critical Path — offline AE replay (real PCM evidence)

Host-side evaluation of experimental Critical Path **detection** and
**continuation/match** rules using firmware `acoustic_l0` + `ae_signature_build`.

This does **not** enable Critical detection in production EAR firmware.
Thresholds exercised here (`crest >= 12/14/16`, `peak >= -6`, `D <= 4/8`, …)
are **investigated only** — not production decisions.

## Build

From repository root:

```bash
cc -std=c11 -Wall -Wextra -O2 -g \
  -I firmware/products/dbmap/ear/audio_engine/include \
  -I firmware/products/dbmap/ear/ear_acoustic/include \
  firmware/products/dbmap/ear/ear_acoustic/acoustic_l0.c \
  firmware/products/dbmap/ear/audio_engine/ae_signature.c \
  firmware/products/dbmap/ear/audio_engine/test_host/crit_offline_replay.c \
  -lm -o /tmp/crit_offline_replay
```

Or: `sh firmware/products/dbmap/ear/audio_engine/test_host/crit_offline/run.sh`

## Input formats (no silent normalize / AGC)

| Input | Requirement | L0 `fs_bits` |
| --- | --- | --- |
| WAV PCM 16-bit | mono or stereo, **exactly 16 kHz** | 16 |
| WAV PCM 24-bit | same | 24 |
| WAV PCM 32-bit | same | 31 |
| WAV IEEE float32 | same; ±1.0 = full scale | 31 |
| Raw `s32le_fs24` / `device` | int32 LE, 16 kHz mono (device-like) | 24 |
| Raw `s16le` | int16 LE mono 16 kHz | 16 |
| Raw `s24_in_s32le` | 24-bit sample in low 24 bits of int32 | 24 |
| Raw `s32le` | int32 LE, FS bits 31 | 31 |

Stereo WAV requires explicit `--channel 0|1|mix` (no silent downmix).
Wrong sample rate is **rejected**; convert with the script below.

`--gain-db N` is the only level transform. Default is `0` (original level preserved).
Every non-zero gain must be recorded in metadata / manifest as a **derived variant**.

### Convert to reference mono 16 kHz (explicit)

Requires `ffmpeg`. Does not apply gain:

```bash
sh firmware/products/dbmap/ear/audio_engine/test_host/crit_offline/convert_to_mono16k.sh \
  /path/to/source.wav \
  firmware/products/dbmap/ear/audio_engine/test_host/crit_offline/evidence/converted/id_mono16k.wav
```

Produces PCM s16le mono 16 kHz WAV. Document the conversion in the recording metadata
(`conversion_command`, `pcm_format`, `sample_rate_hz`).

## Commands

```bash
/tmp/crit_offline_replay list-scenarios
/tmp/crit_offline_replay synth impulse_single --mode both
/tmp/crit_offline_replay uart dbmap-usb-console-ear-001-0.8.14-after-boot.txt --mode detect
/tmp/crit_offline_replay wav path.wav --origin controlled_playback --role distractor \
  --label door --recording-id door_001 --csv /tmp/crit_offline_out/door_001.csv
/tmp/crit_offline_replay wav path.wav --gain-db -12 --recording-id door_001_g-12 \
  --origin controlled_playback --role distractor --label door
/tmp/crit_offline_replay raw path.raw --format s32le_fs24 --csv /tmp/out.csv
/tmp/crit_offline_replay dataset \
  firmware/products/dbmap/ear/audio_engine/test_host/crit_offline/evidence/manifest.csv \
  --out-dir /tmp/crit_offline_out
```

Evaluation sections:

- **A. DETECTION** — candidate trigger sweeps (crest / peak / clip).
- **B. CONTINUATION / MATCH** — frozen START fingerprint vs later windows
  (`ae_signature_distance`), with crest floor. Reports `quiet_match_count`
  (matches without a crest≥12 candidate) so background similarity is not
  mistaken for event evidence.

`--mode detect|match|both` runs the sections independently.

## Labeled evidence layout

See `evidence/METADATA.md` and `evidence/manifest.csv`.

```
evidence/
  METADATA.md          schema + collection guidance
  manifest.csv         batch index (paths + gain variants)
  metadata/*.json      optional rich sidecars
  fixtures/            checked-in smoke audio (synthetic only)
  recordings/          place real WAV/PCM here (gitignored)
  converted/           explicit ffmpeg outputs (gitignored except .gitkeep)
```

## Datasets currently available

| Source | Origin | Notes |
| --- | --- | --- |
| Built-in synth scenarios | synthetic | Pipeline checks only — not Critical evidence |
| `evidence/fixtures/synth_impulse_smoke.wav` | synthetic | Format/smoke fixture |
| UART L0 soak log | uart | Ambient candidates; **unlabeled** |
| Field / playback / licensed targets & distractors | — | **Missing — add under `recordings/`** |

Until labeled field or controlled-playback recordings exist, do **not** promote
any crest/peak/distance value to a production trigger.

## Missing evidence (required before production decisions)

Safely obtained / licensed recordings, clearly labeled:

**Targets:** impulsive events; gunshot-like and explosion-like **datasets or
controlled playback** (never stage real gunfire/explosions for this work).

**Distractors:** speech, doors/impacts, traffic, engines, wind, mic handling,
ordinary environmental transients.

Each entry needs metadata (`origin`, equipment, gain, format). Keep
synthetic / controlled_playback / field separated in all reports.
Distance/attenuation notes are contextual only — not detector thresholds.
