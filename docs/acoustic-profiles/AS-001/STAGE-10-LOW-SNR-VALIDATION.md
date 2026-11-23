# Stage 10.1 low-volume and low-SNR validation

## Method

This was an offline 16 kHz, mono PCM replay on a host. The firmware L0/L1/L2 DSP and AE signature builder fed the production Signature Engine listener, Candidate Engine (CE), Promotion Policy and Observation Manager with the default CE settings and a recording START callback. Each trial restarted the engines and used a one-second analysis window. Physical EAR capture, acoustic SPL, and live MQTT delivery were outside the run.

The sources were the first 24 seconds of `AS001_bbc_heli_mono16k.wav` and seconds 4–28 of `AS001_heli_bg_mono16k.wav`. The latter has background embedded in the source, so its reported SNR is for the whole clip versus the added background. The injected backgrounds were low-level white noise, stronger steady white noise, synthetic low-frequency traffic with moving envelopes, variable broadband/low-frequency noise, and the local full Faran över recording with low-level noise. There was no isolated real traffic or environmental recording in the supplied media. Negative controls also included a 550 Hz tone, the BBC file's unlabelled two-second lead-in, and the repository's synthetic impulse fixture.

The base sweep used eight seconds of background pre-roll, a 24-second source, and 24 seconds of background after the source. Two deterministic realizations per condition used seeds 1 and 2. The five levels were +20, +10, +5, 0 and −10 dB injected SNR. Source and background RMS were measured separately over the source interval before summing and quantizing to signed 16-bit PCM. For example, −62 dBFS source with −67 dBFS background gives +5 dB injected SNR. At very low levels, 16-bit quantization limits what the smallest component contributes after mixing. The report uses digital dBFS and injected SNR throughout.

Across 170 runs, 4,494 of 4,512 analyzed source or control windows were quality OK and spectral-valid. Signature Engine evaluated AS-001 on all 4,512, including DEGRADED windows. Two deliberately strong competing-source trials at −15 dBFS source/+40 dB SNR clipped 1,330 samples combined and were excluded from the reliability comparison. Other trials had zero clipped samples.

## Candidate tracking, matching, and admission

The table reports two-run ranges for selected unclipped conditions. `Created/updated` counts CE births and hard associations during source windows. `Track` is the most source windows associated with one candidate ID. These counters include generic acoustic tracks and cannot identify the source by themselves. `Owned hits` counts the most AS-001 matches associated with a single CE track. Policy and START counts are per complete trial.

| Source and background | Source / background (dBFS) | SNR (dB) | CE created / updated | Track | AS-001 hits / owned hits | Policy AS-001 / AS-002 → START |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| BBC, quiet | −25 / −65 | +40 | 2–3 / 16 | 17 | 4 / 3 | 1 / 0 → 1 |
| BBC, quiet | −62 / −97 | +35 | 3 / 19 | 16 | 2 / 2 | 1 / 0 → 1 |
| BBC, quiet | −74 / −109 | +35 | 2 / 17 | 16 | 0 / 0 | 0 / 0 → 0 |
| BBC, quiet | −62 / −67 | +5 | 3–6 / 18–19 | 12 | 0 / 0 | 0 / 0 → 0 |
| BBC, quiet | −67 / −67 | 0 | 3–5 / 18–21 | 11–12 | 0 / 0 | 0 / 0 → 0 |
| BBC, quiet | −77 / −67 | −10 | 0–5 / 17–23 | 13–21 | 0 / 0 | 0 / 0 → 0 |
| BBC, steady broadband | −28 / −48 | +20 | 6–7 / 16–17 | 12 | 0 / 0 | 0 / 0 → 0 |
| BBC, traffic-shaped | −30 / −45 | +15 | 2–3 / 18–21 | 16 | 3 / 2–3 | 1 / 0 → 1 |
| BBC, traffic-shaped | −35 / −45 | +10 | 3 / 21 | 10–12 | 1 / 1 | 0 / 0 → 0 |
| BBC, variable | −28 / −48 | +20 | 5–6 / 16–17 | 12 | 1 / 1 | 1 / 0 → 1 |
| BBC, Faran över competitor | −25 / −45 | +20 | 2–3 / 21 | 16 | 5 / 5 | 1 / 1 → 2 |
| BBC, Faran över competitor | −40 / −45 | +5 | 1–3 / 19–20 | 16–17 | 0 / 0 | 0 / 1 → 1 |

Across 62 helicopter-with-background runs, AS-001 had zero matches and zero promotions, although CE acquired generic tracks. AS-002 promoted in 26 of those runs. 14 were mixes with an added siren, and the others show that this helicopter-labelled recording can satisfy the siren gates even without added siren audio.

For this bench, repeatable AS-001 detection meant two or more matches on the same candidate and an admitted AS-001 Observation in both realizations. +15 dB was the lowest tested SNR meeting that criterion in traffic-shaped background, while +10 dB yielded one match per run and zero admissions. The competing-siren condition met the criterion at +20 dB. Quiet-background replay met it at +35 dB at several absolute levels, including −62 dBFS source. Stronger broadband through +20 dB did not meet it. The variable-noise +20 dB result produced an Observation on one owned match per run, below the repeatability criterion. These results support those specific validation points and supply no site-wide SNR threshold.

## Interruption and capacity

A separate sequence repeated two BBC seconds four times, removed the source for four seconds, then repeated the same cue again. The 16 source windows remained on one CE ID in each run at +5 dB and −62 dBFS source, with zero AS-001 matches, promotions or STARTs. At +40 dB, both runs rematched the same promoted ID and generation. Each produced eight AS-001 hits and one START. This demonstrates weak track continuity in the controlled interruption. Attribution of a weak track to a helicopter in an uncontrolled scene still needs class-specific evidence.

The 16 background and non-helicopter controls produced zero AS-001 matches or promotions. Quiet and broadband controls created 15–19 generic tracks over 56 seconds, with the longest single ID covering 9–10 associated windows. The traffic-shaped and tonal controls each kept one generic ID through all 56 windows without a helicopter classification. The competing-siren control produced one AS-002 START per run, consistent with its input.

Peak occupancy reached the eight-slot capacity. The strong continuity run with seed 1 had a single birth drop at window 51 in the background tail. Extending that run reproduced the same drop, then expiration freed slots at windows 55 and 59 and allowed new births at 61 and 62. The Stage 9 full-table regression also passed. These observations distinguish temporary capacity pressure from a table that cannot recover.

## Eligibility and next validation

- AS-001's current RMS condition is 3.5 dB above the background EMA, with spectral and distance gates. The old absolute −22 dBFS requirement would reject some matches seen here. In the same nominal +35 dB replay, −62 dBFS still matched and promoted twice across the two runs. At −74 dBFS CE tracked the cue but AS-001 did not match.
- CE has an independent −80 dBFS threshold for **birth** from the AE window's mixed RMS. Existing tracks can update below it. With a −84 dBFS source clip, one window at −78.552 dBFS created a candidate, and one at −81.803 dBFS updated it in seed 1.
- A diagnostic copy of AS-001 with its level requirement bypassed, using the same bands and spectral shape/distance gates, found the same matches on all 9,528 input windows, including pre-roll and tails. The measured weak-source failures therefore do not support removing the RMS condition. A spectrum-relative-to-background or persistent harmonic feature could improve class-specific weak evidence. Neither was validated by this replay. Tracking across windows already works for an ambiguous source, while repeated class-specific evidence would need to be evaluated against the long-lived tonal and traffic controls before influencing promotion.
- Keep the current Candidate birth and Observation promotion settings for physical validation. Use +15 dB traffic-shaped, +20 dB competing-siren, and +35 dB quiet as repeatable BBC test points alongside the near-threshold and negative controls. Collect real background-only recordings and lower-level playback per EAR before choosing operational thresholds. Check that AS-002 detections on the helicopter-with-background clip correspond to its actual audible content, and measure harmonic persistence before changing Candidate classification. No drone clip was available, and no drone profile was introduced.
