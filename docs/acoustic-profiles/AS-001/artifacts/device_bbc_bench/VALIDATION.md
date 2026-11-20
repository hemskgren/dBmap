# AS-001 BBC bench validation (6 captures)

**Source fixture:** `bbc_helicopter_nhu0509724.mp3` (Sonos playback)  
**Firmware under test for capture:** 0.8.32 (pre-relative-bg)  
**Offline replay:** post-change AS-001 rules (relative bg + BBC spectral centers)

Captures cover ~first 45 s (`AE_SIG_CSV` capacity 48). Audio file is longer.

## Capture inventory

| EAR | Vol | CSV | Console |
| --- | ---: | --- | --- |
| EAR-001 | 0.25 | `bbc-heli-1.csv` | `consoles/…-ear-001-…-bbc_heli-1.txt` |
| EAR-001 | 0.45 | `bbc-heli-2.csv` | `…-bbc_heli-2.txt` |
| EAR-001 | 0.65 | `bbc-heli-3.csv` | `…-bbc_heli-3.txt` |
| EAR-002 | 0.25 | `bbc-heli-ear2-1.csv` | `…-ear-002-…-bbc_heli-1.txt` |
| EAR-002 | 0.45 | `bbc-heli-ear2-2.csv` | `…-bbc_heli-2.txt` |
| EAR-002 | 0.65 | `bbc-heli-ear2-3.csv` | `…-bbc_heli-3.txt` |

## Live capture (0.8.32) — what actually happened

| EAR | Vol | Peak RMS | as001_matched | Spectral-gate-ok (old AS-001) | Hub Observation / promote |
| --- | ---: | ---: | ---: | ---: | --- |
| 001 | 0.25 | −41.0 | **0 / 48** | 5 | none (`ce: promote` absent) |
| 001 | 0.45 | −31.0 | **0 / 48** | 2 | none |
| 001 | 0.65 | −21.6 | **0 / 48** | 0 | none (2 windows ≥ −22 abs floor, still spectral reject) |
| 002 | 0.25 | −43.5 | **0 / 48** | 7 | none |
| 002 | 0.45 | −33.7 | **0 / 48** | 6 | none |
| 002 | 0.65 | −25.8 | **0 / 48** | 0 | none |

**Rejection cause (live):** every window `as001_rej_elig=1`. Old absolute
`rms_min=−22` plus synthetic spectral gates (dom 420–620, flat 0.15–0.45) do
not fit BBC office playback (peak flatness often **&lt; 0.12**, dominant often
**~125–250 Hz**).

**AS-002 / Hub path:** no Observation START in these consoles. Do not treat
high RMS as AS-001 detection.

## Offline replay (new AS-001)

Rules: bg EMA α=0.85 on non-match; margin **3.5 dB**; spectral gates
flat ∈ [0.05, 0.32], dom ∈ [100, 450]; band+spectral distance ≤ 6; **no**
absolute RMS distance term.

| EAR | Vol | AS-001 match windows | First match seq | Early false pos (first ≤5 wins) |
| --- | ---: | ---: | ---: | --- |
| 001 | 0.25 | 7 | 29 | none |
| 001 | 0.45 | 37 | 16 | none |
| 001 | 0.65 | 35 | 14 | none |
| 002 | 0.25 | 4 | 38 | none |
| 002 | 0.45 | 15 | 19 | none |
| 002 | 0.65 | 31 | 12 | none |

### Consistency notes

- Detection strengthens with volume on both EARs (0.25 weakest, 0.45/0.65 strong).
- EAR-002 @ 0.25 is sparse (4 windows) but non-zero — expected weaker mic path.
- Pre-event office background does not match (relative margin + spectral).
- **Limitation:** offline replay ≠ flashed firmware proof. Re-flash 0.8.32+ with
  classifier 1.1 and re-capture to confirm live `as001_matched` / CE annotation.

## Evidence classes

| Class | Content |
| --- | --- |
| CSV audio evidence | `rms_dbfs`, bands, flatness, dominant, etc. |
| Live AS-001 matcher (0.8.32) | `as001_*` columns — all reject |
| Offline AS-001 (this change) | reconstructed from CSV + new rules |
| AS-002 / Hub | no promote/START in these consoles |
