# Recording metadata format

Use one JSON sidecar per recording under `metadata/<recording_id>.json`,
and list replay rows (including gain variants) in `manifest.csv`.

## Required fields

| Field | Description |
| --- | --- |
| `recording_id` | Stable ID (also used in CSV filenames) |
| `source` | URL, dataset name, lab session, or file provenance |
| `label` | Short event/distractor label (`impulse_clap`, `speech`, …) |
| `role` | `target` \| `distractor` \| `ambient` \| `synthetic` \| `unknown` |
| `origin` | `field` \| `controlled_playback` \| `synthetic` \| `uart` |
| `sample_rate_hz` | After any documented conversion (replay requires 16000) |
| `pcm_format` | e.g. `wav_s16le_mono`, `s32le_fs24` |
| `applied_gain_db` | `0` for original; non-zero ⇒ derived variant |
| `original_level_notes` | Capture gain, limiter, known overload, etc. |

## Recommended fields

| Field | Description |
| --- | --- |
| `equipment` | Recorder / interface |
| `mic_config` | Mic model, mono/stereo, mounting |
| `attenuation_notes` | Playback SPL, barriers, approximate distance (**context only**) |
| `background_notes` | Noise floor, indoor/outdoor, quality issues |
| `conversion_command` | Exact ffmpeg/sox command if converted |
| `derived_from` | Parent `recording_id` for gain variants |
| `license` | License / redistribution notes |
| `start_seq_hint` | Optional window seq for continuation START |
| `event_notes` | What happens when in the file |

**Do not** treat estimated distance in metres as a detector threshold.

## Example sidecar

See `metadata/synth_impulse_smoke.json`.

## manifest.csv columns

```
recording_id,audio_path,label,role,origin,applied_gain_db,pcm_format,channel,start_seq_hint,notes
```

- Paths may be relative to the repository root or absolute.
- Duplicate `recording_id` with different `applied_gain_db` for level variants
  (prefer unique ids such as `id_g-12`).
- `pcm_format`: `wav` \| `s32le_fs24` \| `s16le` \| `s24_in_s32le` \| `s32le`
- `channel`: `mono` \| `0` \| `1` \| `mix`
- Leave `start_seq_hint` empty to seed START from the first experimental crest≥12 window.

## Collection guidance (safe)

- Prefer licensed datasets, lab playback of safe sounds, or existing archives.
- Do **not** stage real gunfire or explosions for this investigation.
- Mark `controlled_playback` vs `field` explicitly.
- Preserve original levels; create attenuated variants only via documented `applied_gain_db`.
