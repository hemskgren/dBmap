# AS-001 — Helicopter (experimental)

## Observation assessment (V1)

Promoted AS-001 Observations emit `acoustic.assessment` (`ear_hyp_v2`).

Lifecycle (same progressive deepen pattern as AS-002, different taxonomy):

| Report | `path` |
| --- | --- |
| START | `[AIRCRAFT]` |
| first PERIODIC (and later) | `[AIRCRAFT, Helicopter]` |
| END (if no PERIODIC yet) | `[AIRCRAFT, Helicopter]` |

Implemented in `ear_cls_as001` + OM deepen before PERIODIC/END emit — not via
AS-002 siren / ALARM progressive evidence. Per-node confidence is independent
(root AIRCRAFT ≥ leaf Helicopter).

| Field | Value |
| --- | --- |
| ID | AS-001 |
| Name | Helicopter |
| Profile version | 0.2.0-experimental |
| Status | Experimental / device-bench calibrated (BBC NHU0509724) |
| Reference | BBC helicopter recording `bbc_helicopter_nhu0509724.mp3` |
| Target | Detect a helicopter-like acoustic event **above measured background** |
| Classifier | `EAR_CLASSIFIER_VERSION` **1.1** (relative-bg eligibility) |
| Last updated | 2026-11-20 |

**Not** aircraft model identification. Distance match annotates Candidates;
promotion remains product-policy gated.

Machine profile: [`profile.json`](./profile.json)  
Device-bench CSVs: [`artifacts/device_bbc_bench/`](./artifacts/device_bbc_bench/)

---

## 1. Stimulus

| Property | Value |
| --- | --- |
| Original | `docs/acoustic-profiles/local-media/AS-001/bbc_helicopter_nhu0509724.mp3` |
| Format | MP3 → mono PCM 16-bit working WAVs (48 kHz + 16 kHz) |
| Conversion | No gain / AGC |
| Nature | Real-world library recording (not synthetic rotor + noise) |
| Retired | `AS001_heli_bg_*` synthetic fixture |

Preserve the original MP3. Do not add synthetic noise or generated rotor layers.

---

## 2. Level qualification (relative background)

AS-001 reuses the **AS-002 principle** of comparing signal level to a measured
background EMA — **not** AS-002’s siren/VMA spectral gates.

| Rule | Value |
| --- | --- |
| Background EMA | α = 0.85 on non-matching usable windows |
| Margin | `rms >= bg_ema + 3.5 dB` |
| Absolute `rms_min` | **not** used for eligibility |
| Distance | Band-relative + spectral only (amplitude-invariant) |

Ordinary office noise fails the relative margin and/or spectral gates.

---

## 3. Spectral / band template (device-bench)

Centers from elevated windows across six EAR captures (vol 0.25 / 0.45 / 0.65 ×
EAR-001 / EAR-002) of the BBC MP3:

| Feature | Center | Gate |
| --- | --- | --- |
| band_rel | ≈ [−4.21, −3.52, −13.10, −32.77] | (distance) |
| dominant_hz | ≈ 230 | 100–450 |
| flatness | ≈ 0.141 | 0.05–0.32 |
| centroid_hz | ≈ 800 | (distance) |
| `match_distance_max` | 6.0 | |

---

## 4. Device-bench validation (firmware offline replay)

See [`artifacts/device_bbc_bench/VALIDATION.md`](./artifacts/device_bbc_bench/VALIDATION.md).

Summary: AS-001 detects on **all six** captures after relative-bg retune; no
early false positives on pre-event office background in the CSV window.

---

## 5. Extraction (optional host AE)

```bash
# working WAVs already under local-media/AS-001/
/tmp/as_profile_extract \
  docs/acoustic-profiles/local-media/AS-001/AS001_bbc_heli_mono16k.wav \
  docs/acoustic-profiles/AS-001/artifacts/signatures.csv
```

Firmware matcher parameters are calibrated from **device-bench** CSVs of the
same BBC file (office Sonos path), not from digital full-scale peak alone.
