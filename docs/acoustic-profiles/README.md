# Acoustic Profile Registry

Living registry of **host-derived acoustic profiles** built from Audio Engine
(`ae_signature_t`) evidence. Profiles are **not** device Observations and do
**not** auto-promote Candidates.

## Profiles

| ID | Name | Version | Status | Location |
| --- | --- | --- | --- | --- |
| AS-001 | Helicopter | 0.1.0-experimental | experimental / host-verified + provisional `as.profile` workflow match | [AS-001/](./AS-001/) |
| AS-002 | Faran över (siren tone) | 0.1.0-experimental | experimental / Core Signature Engine (gates + streak); host AE + EAR-001 indoor bench | [AS-002/](./AS-002/) |

## Entry layout (per profile)

```text
AS-NNN/
  PROFILE.md           human summary
  profile.json         machine-readable profile + phase stats
  stimulus/            METADATA.md (paths into local-media/)
  artifacts/           signatures.csv, waveform/spectrogram PNGs, plots

local-media/           entire directory gitignored — put/move WAVs here
  AS-NNN/
    *.wav
```

**Do not commit audio.** Move recordings into `docs/acoustic-profiles/local-media/`
(that whole directory is in `.gitignore`). Track only metadata and derived AE
CSV / plots.

Required fields (see each `profile.json`):

- Signature ID, name, description, profile version
- Source recording / stimulus (synthetic vs field)
- Derived AE feature data (from real L0/L1/L2 → `ae_signature_build`)
- Background comparison
- Validation status, last updated

## Device Desired / Reported (schema only — not implemented here)

Acoustic profiles stay separate from on-device version reporting. Future Device
Desired / Reported may carry (without embedding profile blobs):

| Field | Intent |
| --- | --- |
| Firmware version | Product image identity |
| Configuration version | Desired/config generation |
| Calibration version | Mic / level calibration package |
| Classifier version | On-device classifier package that may *reference* profile IDs |

This registry does **not** implement Device Desired/Reported emission.

## Extraction tool

Host extractor (firmware DSP, no invented features):

`firmware/products/dbmap/ear/audio_engine/test_host/as_profile_extract.c`

Live EAR signature capture (bench overlay, compare to host CSV):  
[`docs/diagnostics/ear-ae-sig-capture-as001.md`](../diagnostics/ear-ae-sig-capture-as001.md).
