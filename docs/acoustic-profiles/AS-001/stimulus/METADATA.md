# AS-001 stimulus metadata

**Audio lives only under the gitignored tree**  
`docs/acoustic-profiles/local-media/` (entire directory ignored).

Place / move files here (example layout):

```text
docs/acoustic-profiles/local-media/AS-001/
  AS001_heli_bg_48k_original.wav    # 48 kHz mono PCM s16le, 32 s
  AS001_heli_bg_mono16k.wav         # 16 kHz working copy for AE extract
```

Do **not** put WAVs in the repo root or under tracked `stimulus/` paths.

| Key | Value |
| --- | --- |
| recording_id | AS001_heli_bg |
| synthetic | true |
| authentic_aircraft | false |
| model_claim | none (helicopter-like only) |
| original_file | `local-media/AS-001/AS001_heli_bg_48k_original.wav` |
| original_format | PCM s16le mono 48000 Hz, 32.0 s |
| ae_working_file | `local-media/AS-001/AS001_heli_bg_mono16k.wav` |
| ae_format | PCM s16le mono 16000 Hz |
| conversion | mean-decimate factor 3 (48→16); gain_db = 0 |
| phases_s | bg_pre 0–5; heli 5–27 (approach 5–16, peak 16–22, recede 22–27); bg_post 27–32 |

Historical drop name (also keep only under `local-media/`):  
`dbmap_AS001_helicopter_approach_test.wav`.
