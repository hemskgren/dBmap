# AS-002 — Faran över siren tone (experimental)

| Field | Value |
| --- | --- |
| ID | AS-002 |
| Name | Faran över (Swedish public warning siren tone) |
| Profile version | 0.1.0-experimental |
| Status | Experimental / host AE extract + EAR-001 indoor bench CSVs |
| Reference | MCF Faran över MP3 + office playback captures (~3 m) |
| Target | Recognize the **shared continuous siren sound** (not full message timing) |
| Last updated | 2026-11-17 |

**Not** a VMA classifier. **Not** an Observation creator. No firmware
`as.profile` matcher is wired for AS-002 in this change (docs/profile only).

Machine profile: [`profile.json`](./profile.json)  
Host AE (siren crop): [`artifacts/signatures.csv`](./artifacts/signatures.csv)  
Device bench CSVs: `artifacts/device_ear001_vol{35,55,75}.csv`

---

## 1. Source material and provenance

| Item | Location |
| --- | --- |
| Original MP3 (moved from repo root) | `docs/acoustic-profiles/local-media/AS-002/02-faran-over-.mp3` |
| Stable alias (identical bytes) | `…/AS002_faran_over_mcf_original.mp3` |
| Provenance URL | `https://www.mcf.se/contentassets/ffc078def2284745ae47c8282f83839f/02-faran-over-.mp3` |
| Also referenced by | `tests/audio/faran-over.json` (HA playback test recipe) |

AS-001 helicopter WAVs remain under `local-media/AS-001/` (found and left in place).

### Timing of the MCF recording (authoritative for this profile)

| Phase | Time (s) | Role |
| --- | --- | --- |
| Spoken intro | 0–3 | **Excluded** from signature |
| Continuous siren | 3–30 | **Signature** (~27 s uninterrupted; ≈ “30 s” message body) |
| Spoken outro | 30–35.5 | **Excluded** |
| Trailing silence | 35.5–38.58 | **Excluded** |

There is **no pause or silence during the siren portion**. Voice is not a
siren interruption.

Host AE working files: mono 16 kHz full clip + siren-only crop (`-ss 3 -to 30`).

---

## 2. Relationship to VMA

| | Faran över (this profile) | VMA (related) |
| --- | --- | --- |
| Timbre | Same characteristic monotonic public-warning siren | Same family |
| Pattern | ~30 s **continuous** tone | ~7 s tone / ~14 s silence, repeating |
| This task | Profile the **common sound** | Do **not** redesign / add VMA classifier |

Longer-term: temporal structure can distinguish message types once the shared
siren core is recognized. Keep recognition of the tone separate from
classification of the complete warning message.

---

## 3. Device bench recordings (supplied)

All three captures: EAR-001 firmware **0.8.23**, indoor office, ~**3 m**, AE
signature CSV (`channel=0` only). Playback volume is **nominal** media volume,
not calibrated SPL.

| Recording | Nominal volume | Artifact |
| --- | --- | --- |
| 1 | 35% | `artifacts/device_ear001_vol35.csv` (+ console `dbmap-usb-console-ear-001-0.8.23-vma-faranover-1.txt` in repo root) |
| 2 | 55% | `artifacts/device_ear001_vol55.csv` |
| 3 | 75% | `artifacts/device_ear001_vol75.csv` |

### Comparison (siren-dominated windows)

| Metric | vol35 (weak) | vol55 | vol75 | Host siren crop |
| --- | --- | --- | --- | --- |
| RMS dBFS (mean) | ≈ −27.5 | ≈ −40.5 | ≈ −34.1 | ≈ −12.9 |
| Flatness (mean) | ≈ 0.47 | ≈ 0.16 | ≈ 0.12 | ≈ 0.14 |
| Dominant Hz (typ.) | 343.75 | 343.75 | 687.5 | 687.5 (occasional 343.75) |
| Band shape | B0 > B1 (noise-like) | B1 > B0 | B1 > B0 | B1 > B0 |
| Early 5–7 s tonal? | **No** | **Yes** | **Yes** | **Yes** |

Notes:

- Absolute RMS does **not** track the 35/55/75% labels monotonically (vol35
  louder than vol55 in these CSVs). Use measured features, not nominal %.
- Level-dependent: RMS/peak. More robust when SNR allows: **low flatness**,
  midband (B1/B2) vs B0, stable dominant in {343.75, 687.5} Hz bins.
- AS-001 matcher never fired (`as001_matched=0`) — expected (different class).

### L/R (from console `win#` lines, not CSV)

| Rec | Mean L−R (dB) | Comment |
| --- | --- | --- |
| vol35 | ≈ +21 | Severe imbalance (R much quieter) — geometry/capture caveat |
| vol55 | ≈ −1.9 | Both mics track; small bias |
| vol75 | ≈ −0.7 | Both mics track |

CSV export is mono channel 0; stereo diagnostics come from `sp_mic` logs.

---

## 4. Host AE extraction

```bash
# After placing 16 kHz mono siren crop under local-media/AS-002/
/tmp/as_profile_extract \
  docs/acoustic-profiles/local-media/AS-002/AS002_faran_over_siren_mono16k.wav \
  docs/acoustic-profiles/AS-002/artifacts/signatures.csv
```

| Item | Result |
| --- | --- |
| Windows | 27 × 1000 ms @ 16 kHz (siren crop) |
| Quality | All OK |
| Spectral / dynamics | Valid |
| Pair | Unavailable (mono) |

Artifacts: `waveform.png`, `spectrogram_0_4kHz.png` (siren crop).

---

## 5. Recognition within 5–7 seconds (hypothesis → evidence)

**Hypothesis:** ~5–7 s of continuous siren is enough to recognize the common
tone without waiting for the full ~30 s.

| Condition | Verdict |
| --- | --- |
| Clean host crop | **Supported** (every 1 s window already tonal midband) |
| EAR vol55 / vol75 office | **Supported** (first 7 contiguous windows already low flatness + stable dominant) |
| EAR vol35 office | **Not supported** for spectral recognition in this capture |

Full 30 s is **not** required when SNR is adequate.

---

## 6. ESP32 realism assessment

| Question | Assessment |
| --- | --- |
| Can EAR hear the MCF tone indoors at moderate/loud playback? | **Yes** (vol55/75) |
| Is a clean theoretical 687.5 Hz-only template enough at low SNR? | **No** — vol35 loses spectral contrast |
| Shared signature with VMA? | **Plausible** on timbre; temporal pattern is separate |
| Firmware matcher / Observation promotion? | **Not implemented** for AS-002 here |

### Additional measurements needed

- Outdoor / louder real siren or calibrated SPL levels.
- Dual-channel AE CSV (L and R), not only ch0.
- Explicit VMA 7/14 pattern capture for temporal discrimination work later.
- Agent could not attach `/dev/ttyACM*` — no new physical captures in this session.

---

## 7. Architectural notes

- Follows acoustic-profile registry layout (same as AS-001).
- Does **not** add a second firmware siren detector or change VMA product logic.
- Spoken audio excluded from signature analysis.
- Profile describes the **common continuous siren sound**, not message identity.
