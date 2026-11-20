# AS-001 stimulus metadata

**Audio lives only under the gitignored tree**  
`docs/acoustic-profiles/local-media/` (entire directory ignored).

```text
docs/acoustic-profiles/local-media/AS-001/
  bbc_helicopter_nhu0509724.mp3     # authoritative source (BBC NHU0509724)
  AS001_bbc_heli_48k_mono.wav       # 48 kHz mono PCM s16le working copy
  AS001_bbc_heli_mono16k.wav        # 16 kHz mono for host AE extract
  # Retired synthetic fixture (kept for archival comparison only):
  AS001_heli_bg_48k_original.wav
  AS001_heli_bg_mono16k.wav
```

Do **not** put WAVs in the repo root or under tracked `stimulus/` paths.

| Key | Value |
| --- | --- |
| recording_id | bbc_helicopter_nhu0509724 |
| synthetic | false |
| authentic_aircraft | unknown (BBC library recording; not claimed as model ID) |
| model_claim | none (helicopter-like only) |
| original_file | `local-media/AS-001/bbc_helicopter_nhu0509724.mp3` |
| original_format | MP3 stereo 44100 Hz, ~81.7 s, ~128 kb/s |
| ae_working_file | `local-media/AS-001/AS001_bbc_heli_mono16k.wav` |
| ae_format | PCM s16le mono 16000 Hz |
| conversion | ffmpeg stereo→mono; resample; **no gain / AGC** |
| retired_fixture | `AS001_heli_bg_*` synthetic heli-over-background (pre-2026-11-20) |

Device-bench calibration captures (Sonos playback of the BBC MP3):  
`docs/acoustic-profiles/AS-001/artifacts/device_bbc_bench/`.
