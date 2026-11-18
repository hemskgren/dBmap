# AS-002 stimulus metadata

**Audio lives only under the gitignored tree**  
`docs/acoustic-profiles/local-media/` (entire directory ignored).

```text
docs/acoustic-profiles/local-media/AS-002/
  02-faran-over-.mp3                 # MCF original filename (moved from repo root)
  AS002_faran_over_mcf_original.mp3  # same bytes, stable alias
  AS002_faran_over_48k_mono.wav      # mono PCM s16le 48 kHz working copy
  AS002_faran_over_mono16k.wav       # mono PCM s16le 16 kHz for AE extract
  AS002_faran_over_siren_mono16k.wav # 16 kHz crop of continuous siren only (3–30 s)
```

Do **not** commit audio. Do **not** leave MP3s in the repo root.

| Key | Value |
| --- | --- |
| recording_id | AS002_faran_over_mcf |
| synthetic | false |
| outdoor_siren_field | false |
| provenance | MCF public sample URL (see PROFILE.md / `tests/audio/faran-over.json`) |
| original_file | `local-media/AS-002/02-faran-over-.mp3` |
| original_format | MP3 stereo 44100 Hz, 38.58 s, ~128 kb/s |
| ae_working_file | `local-media/AS-002/AS002_faran_over_mono16k.wav` |
| siren_only_file | `local-media/AS-002/AS002_faran_over_siren_mono16k.wav` |
| conversion | ffmpeg stereo→mono; resample; **no gain/AGC** |
| phases_s | spoken_intro 0–3; **siren_continuous 3–30**; spoken_outro 30–35.5; silence 35.5–38.58 |

Spoken announcements are **not** part of the siren signature. There is **no** silence inside the 3–30 s siren interval.
