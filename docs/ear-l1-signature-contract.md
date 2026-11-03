# Observation protocol v1 — L1 Acoustic Signature

**Status:** Ear (optional), Hub, and simulator share this contract.  
**Protocol version:** **`1`** hierarchical envelope (`identity` / `context` / `acoustic` / `spatial`).

L1 lives at `acoustic.signature` with `schema: "l1"`. It is an **acoustic representation**, not a classifier.

When `DBMAP_EAR_OBS_L1` is disabled, firmware serializes `acoustic.signature` as JSON **null**.

## Scope

| In scope | Out of scope |
| --- | --- |
| Interval-scoped spectral features | Source classification (L2) |
| Same window association as L0 | Changing detector thresholds / lifecycle |
| FFT 1024 / Hann / 80–7600 Hz | Spectral rolloff, HNR, MFCC, embeddings |

Observation interval association (identical to L0):

`start < window_end ≤ end`

Only **valid, unclipped** windows contribute to feature means/stdevs. Associated but invalid/clipped windows increment `invalid_window_count`.

## FFT parameters

| Parameter | Value |
| --- | --- |
| Sample rate | 16 000 Hz |
| FFT size | 1024 |
| Window | Hann, non-overlapping frames across the 1 s PCM buffer |
| Analysis band | 80–7600 Hz (bins outside the band ignored) |

Per 1 s analysis window the firmware averages power spectra across complete non-overlapping frames (`⌊N/1024⌋` frames).

## Feature formulas (per window)

Let \(P[k]\) be the mean frame power at bin \(k\), \(f_k = k \cdot f_s / N\), and \(M[k]=\sqrt{P[k]}\). Sums are over bins with \(f_k \in [80,7600]\).

| Feature | Formula |
| --- | --- |
| **centroid_hz** | \(\sum f_k M[k] / \sum M[k]\) |
| **bandwidth_hz** | \(\sqrt{\sum (f_k-\mathrm{centroid})^2 M[k] / \sum M[k]}\) |
| **dominant_hz** | \(f_{k^*}\) where \(k^*=\arg\max P[k]\) in-band |
| **flatness** | \(\bigl(\prod M[k]\bigr)^{1/n} / \bigl(\frac{1}{n}\sum M[k]\bigr)\) clipped to \([0,1]\) (uses only bins with \(M[k]>10^{-30}\)) |

A window is **invalid** when total in-band power is below `1e-12` (FS-relative) or fewer than one FFT frame fits. A window is **clipped** when peak ≥ −0.05 dBFS; clipped windows are excluded from feature aggregates.

## Interval aggregation

For each feature \(x\) over \(n\) valid unclipped associated windows:

- `mean` = arithmetic mean of per-window values  
- `stdev` = population standard deviation when \(n \ge 2\); otherwise JSON **null**

Empty interval (`window_count=0`): publish a valid L1 object with null feature means/stdevs.

## JSON shape

```json
{
  "schema": "l1",
  "version": 1,
  "sample_rate_hz": 16000,
  "analysis_window_ms": 1000,
  "fft_size": 1024,
  "freq_min_hz": 80.0,
  "freq_max_hz": 7600.0,
  "window_count": 5,
  "invalid_window_count": 0,
  "centroid_hz": { "mean": 1840.2, "stdev": 210.5 },
  "bandwidth_hz": { "mean": 960.0, "stdev": 120.0 },
  "dominant_hz": { "mean": 1720.0, "stdev": 85.0 },
  "flatness": { "mean": 0.18, "stdev": 0.04 }
}
```

Empty example:

```json
{
  "schema": "l1",
  "version": 1,
  "sample_rate_hz": 16000,
  "analysis_window_ms": 1000,
  "fft_size": 1024,
  "freq_min_hz": 80.0,
  "freq_max_hz": 7600.0,
  "window_count": 0,
  "invalid_window_count": 0,
  "centroid_hz": { "mean": null, "stdev": null },
  "bandwidth_hz": { "mean": null, "stdev": null },
  "dominant_hz": { "mean": null, "stdev": null },
  "flatness": { "mean": null, "stdev": null }
}
```

## Configuration

| Kconfig | Effect |
| --- | --- |
| `DBMAP_EAR_OBS_L1=y` | Compute L1 in the observation/analysis task; publish L1 object (or empty) |
| `DBMAP_EAR_OBS_L1=n` | No L1 compute; `acoustic.signature: null` |

Requires `DBMAP_EAR_MIC_L0` and `DBMAP_EAR_OBS_L0` (shared PCM + history capacity). L1 aggregation state is independent of L0 formulas.

## Precision

- Hz fields: about one decimal place in serialized examples  
- Flatness: about three decimal places  
