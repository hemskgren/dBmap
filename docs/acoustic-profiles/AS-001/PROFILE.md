# AS-001 — Helicopter (experimental)

| Field | Value |
| --- | --- |
| ID | AS-001 |
| Name | Helicopter |
| Profile version | 0.1.0-experimental |
| Status | Experimental / **host-verified only** |
| Reference | Synthetic helicopter-over-background stimulus |
| Target | Detect a helicopter-like acoustic event above background |
| Last updated | 2026-11-15 |

**Not** Bell 429 (or any model) identification. **Not** an Observation creator.
Provisional firmware matcher exists (`acoustic_profile` + CE annotation only);
it does **not** promote Candidates or create Observations.

Machine profile: [`profile.json`](./profile.json)  
Signatures (raw AE): [`artifacts/signatures.csv`](./artifacts/signatures.csv)

---

## 1. Stimulus validation

| Property | Value |
| --- | --- |
| Original | `docs/acoustic-profiles/local-media/AS-001/` (**entire `local-media/` gitignored**) |
| Format | WAV mono PCM 16-bit, **48 kHz**, 32.0 s |
| AE working copy | Same `local-media/AS-001/` tree (16 kHz mono) |
| Conversion | Exact 3:1 mean-decimate → **16 kHz** mono; **no gain / AGC** |
| Nature | **Synthetic** (not an authentic field / Bell 429 recording) |

### Timing (authoritative for this profile)

| Phase | Time (s) | Windows (1 s AE) |
| --- | --- | --- |
| Background before | 0–5 | seq 1–5 |
| Helicopter approaching | 5–16 | seq 6–16 |
| Helicopter strongest | 16–22 | seq 17–22 |
| Helicopter receding | 22–27 | seq 23–27 |
| Background after | 27–32 | seq 28–32 |

### Validation result

- Waveform shows clear approach → peak → recede envelope vs quiet ends.
- Spectrogram (0–4 kHz) shows **persistent low-frequency harmonic lines** during
  the helicopter interval, absent in background — consistent with a rotor-like
  synthetic, **not** broadband digital hash noise.
- 1 s RMS ≈ −34 dBFS (bg) vs ≈ −9 to −11 dBFS (peak).

Artifacts: `artifacts/waveform.png`, `artifacts/spectrogram_0_4kHz.png`.

---

## 2. Audio Engine extraction

Host tool (real Platform DSP + `ae_signature_build`):

```bash
cc -std=c11 -Wall -Wextra -O2 -g \
  -DCONFIG_SP_AUDIO_L0=1 -DCONFIG_SP_AUDIO_L1=1 -DCONFIG_SP_AUDIO_L2=1 \
  -I firmware/platform/audio/include \
  -I firmware/products/dbmap/ear/audio_engine/include \
  -I firmware/products/dbmap/ear/ear_acoustic/include \
  firmware/platform/audio/sp_acoustic_l0.c \
  firmware/platform/audio/sp_acoustic_l1.c \
  firmware/platform/audio/sp_acoustic_l2.c \
  firmware/products/dbmap/ear/audio_engine/ae_signature.c \
  firmware/products/dbmap/ear/audio_engine/test_host/as_profile_extract.c \
  -lm -o /tmp/as_profile_extract

/tmp/as_profile_extract \
  docs/acoustic-profiles/local-media/AS-001/AS001_heli_bg_mono16k.wav \
  docs/acoustic-profiles/AS-001/artifacts/signatures.csv
```


| Item | Result |
| --- | --- |
| Windows | 32 × 1000 ms @ 16 kHz |
| Signatures | 32 (`ae_signature_t` schema v1) |
| Quality | All **OK** |
| Spectral / dynamics | Valid on all windows |
| Pair metrics | **Unavailable** (mono → `AE_FLAG_PAIR_MISSING`) |

No invented RMS/band/spectral values — L0/L1/L2 implementations only.

---

## 3. Phase comparison (means from AE)

| Phase | n | RMS dBFS | B0 | B1 | B2 | dominant med Hz | flatness | env_cv |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| bg_pre | 5 | −34.3 | −36.4 | −40.4 | −47.2 | ~188 | 0.61 | 0.07 |
| heli_approach | 11 | −14.2 | −21.9 | −21.6 | — | ~547 | 0.32 | 0.16 |
| heli_peak | 6 | −10.0 | −18.2 | −17.4 | −26.9 | ~492 | 0.26 | 0.13 |
| heli_recede | 5 | −19.3 | −26.6 | −26.5 | — | ~484 | 0.40 | 0.21 |
| bg_post | 5 | −34.5 | −36.7 | −40.5 | −47.3 | ~125 | 0.61 | 0.09 |

### What distinguishes helicopter from background

Largest mean deltas (**heli_peak − bg_pre**):

| Feature | Δ | Interpretation |
| --- | --- | --- |
| RMS / peak / B0–B2 | ~+18…+24 dB | Much louder structured energy |
| centroid_hz | ≈ −788 Hz | Energy concentrates lower |
| bandwidth_hz | ≈ −669 Hz | Narrower than bg noise-like spread |
| dominant_hz | ≈ +333 Hz | Stable ~500 Hz tonal focus vs noisy bg |
| flatness | lower (~0.26 vs ~0.61) | More tonal / less noise-like |
| envelope_cv | higher in approach/recede | Dynamics of pass-by |

Plot: `artifacts/ae_feature_timeseries.png`.

Full distributions: `profile.json` → `phases`.

---

## 4. Verified vs not verified

**Verified (host):**

- Stimulus is a usable synthetic helicopter-like pass-by over background.
- AE signatures extracted end-to-end from PCM via firmware DSP.
- Background vs helicopter feature contrast is measurable and documented.

**Firmware (provisional):**

- Embedded AS-001 template matched by workflow module `as.profile` (classifier
  layer). Result is associated via `ce_get_classification`, not CE identity.
- Does **not** change Stage 1 identity or CE matching semantics.
- P2 experimental allow: Promotion Policy may authorize promote when AS-001
  matches (provisional helicopter-like annotation). Ambient energy alone never
  promotes. This is not verified aircraft detection.

**Not verified / not claimed:**

- Validated helicopter / Bell 429 recognition on device.
- Production-grade automatic promotion from profile match.
- Field-proven Observation creation from AS-001.
- Field recordings or stereo / pair / DoA cues (mono only).
- Production classifier thresholds.
