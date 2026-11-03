# Ear L0 performance optimization

**Status:** Stage A (instrumentation) + Stage B (float hot path) implemented in tree.  
**Physical S3 before/after:** not claimed until flashed and measured at 160 MHz.

## Stage A — instrumentation

Inside `dbmap_l0_analyze_window()`:

| Counter | Span |
| --- | --- |
| `stage_reset_us` | `dbmap_l0_filter_bank_reset` |
| `stage_sample_loop_us` | PCM sample loop |
| `stage_post_us` | RMS/peak/crest/clip + band dBFS + HL |

Existing `analyze_us_last/max` unchanged (outer timer in `dbmap_ear_mic_l0_poll`).

Serial (~5 s):

```
L0 perf … us_last=… us_max=… … stage_reset=… stage_loop=… stage_post=…
```

### Host stage sanity (not S3 evidence)

One 16 kHz / 1 s tone window (`test_acoustic_l0_parity`):

| reset | loop | post | loop fraction |
| ---: | ---: | ---: | ---: |
| ~0 µs | ~532 µs | ~4 µs | **0.993** |

Confirms sample loop dominates total analyze time as expected.

## Stage B — float hot path

Changed in the sample loop / window post-process only:

- FS normalize, `sum_sq`, `band_sum_sq` → `float`
- dBFS / HL helpers for the window use `log10f` / `sqrtf`
- Biquad topology, coefficients, reset-per-window, band plan unchanged
- Interval aggregation still uses double energy sums for T*/means stability

### Parity tolerances (`test_acoustic_l0_parity`)

| Field | Tolerance |
| --- | --- |
| `valid`, `clipped`, `clipped_sample_count`, `hl_valid` | **exact** |
| rms / peak / crest | 0.05 dB (0.15 dB if level &lt; −80 dBFS) |
| band_dbfs | 0.10 dB (0.25 dB if band &lt; −80 dBFS) |
| hl_db | 0.15 dB when `hl_valid` |
| band_energy_linear | rel 2e-3 or abs 1e-14 |
| derived band fractions | 0.01 abs when total &gt; 1e-9 |

Cases: ambient, per-band tones, two-tone HL, near-floor, silence, near-FS, impulse, clip sample.  
Result: **ok**.

## Hardware verification @ 160 MHz (required)

1. Flash current `esp-ear` with `SERIAL_DIAG=y`, L0/L1 on; **do not** change CPU/PM.
2. Run ≥500 analysis windows (~9+ minutes).
3. Capture several `L0 perf` / `L1 perf` / `cpu` lines.
4. Record:

| Metric | Pre-opt (prior) | Post-opt (fill) |
| --- | --- | --- |
| L0 us_last / us_max | ~161–168 ms | |
| stage_reset / loop / post | n/a | |
| loop / us_last fraction | n/a | expect ≈1 |
| L1 us_last / us_max | ~43–48 ms | must not regress materially |
| overwrite | 0 | must stay 0 |
| freq_mhz | 160 | 160 |

Do not treat host µs as ESP32-S3 performance.
