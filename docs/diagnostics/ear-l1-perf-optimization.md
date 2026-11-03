# Ear L1 performance optimization (ESP32-S3)

**Status:** Software path + diagnostics implemented; **physical EAR baseline / A/B not yet measured in this environment** (no serial device attached).  
**Contract:** [`../ear-l1-signature-contract.md`](../ear-l1-signature-contract.md) unchanged (FFT 1024, Hann, 16 kHz, 80–7600 Hz, features/schema).

## Phase 0 — Diagnostics (implemented)

When `CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG=y`, ~5 s counters now include:

```
ear_obs: L0 perf analyze_n=… us_last=… us_max=… overwrite=… pcm_cap=… …
ear_obs: L1 perf analyze_n=… us_last=… us_max=…
ear_obs: cpu freq_mhz=… configured_mhz=… pm=… free_heap=… free_spiram=…
```

- L1 counters are the existing `dbmap_ear_mic_get_l1_perf()` wall-clock times around `dbmap_l1_analyze_window` only (not L0, not ring/MQTT).
- CPU MHz is runtime `esp_clk_cpu_freq()` plus `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` and whether `CONFIG_PM_ENABLE` is set.
- **Do not** treat the historical `L0 perf … us_last≈163–174 ms` line as L1.

### Physical baseline procedure (≥500 windows)

1. Flash build with serial diag + L1 on (current `sdkconfig.defaults`).
2. Leave board running ≥9 minutes at 1 s windows (`analyze_n ≥ 500`).
3. Capture several `L0 perf` / `L1 perf` / `cpu` lines.
4. Record overwrite, I2S health (timeouts/errors), heap/SPIRAM.

| Field | Baseline (physical) | Notes |
| --- | --- | --- |
| CPU configured / runtime MHz | **160 / TBD** | sdkconfig: `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`; `CONFIG_PM_ENABLE` unset |
| L1 us_last / us_max | **TBD** | fill from `L1 perf` |
| L0 us_last / us_max | **TBD** (~160–169 ms historically) | fill from `L0 perf`; L0 not optimized here |
| PCM overwrite | **TBD** | must stay 0 |
| free_heap / free_spiram | **TBD** | from `cpu` line |

## Phase 1 — Software optimization (implemented; host-validated)

### Changes

| Change | Expected benefit | Accuracy | RAM |
| --- | --- | --- | --- |
| Hot path `float` instead of `double` | Avoid soft-double on S3 FPU (SP only) | Feature Hz within existing host tolerances; near-floor validity may differ by ε | No heap; BSS workspace grows |
| Precomputed bit-reversal + twiddle LUT | Remove `cosf`/`sinf` and incremental twiddle error from inner FFT | Same radix-2 DFT definition | ~+6 KiB in `dbmap_l1_workspace_t` (~22 KiB total static) |
| Integer peak/abs for clip scan | Cheaper clip detect | Same −0.05 dBFS rule via `log10f` once | None |
| Float feature extraction (`sqrtf`/`logf`) | Less soft-double in band pass | Small spectral-leakage deltas vs double | None |

**Not implemented yet (pending device numbers):** esp-dsp FFT, SPIRAM→internal frame copy, CPU affinity. Add only if L1 remains ≥100 ms after this path.

### Host measurements (x86_64 `-O2`, not S3)

| Build | µs / 1 s window (50–80 iter avg) | 1 kHz dominant |
| --- | --- | --- |
| Pre-opt (recovered double + online twiddles) | ~839 | 1000 Hz |
| Post-opt (float + LUT) | ~258 | 1000 Hz |

≈**3.3×** host speedup. S3 soft-double penalty is larger than on host — **device speedup must be measured**; do not treat 3.3× as on-device fact.

Host unit tests: `test_acoustic_l1` → `ok`. Tone centroid/dominant stay on-bin; bandwidth differs by a few Hz vs double (spectral leakage / float), within prior test gates.

### Device before/after

| Config | L1 us_last | L1 us_max | overwrite |
| --- | ---: | ---: | ---: |
| Original (pre float/LUT) | TBD | TBD | TBD |
| Software optimized | TBD | TBD | TBD |

## Phase 2 — CPU frequency (investigation only; default unchanged)

| Item | Finding |
| --- | --- |
| Configured | **160 MHz** (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160`) |
| Available | 80 / 160 / **240** MHz on ESP32-S3 |
| Dynamic FS / PM | `CONFIG_PM_ENABLE` **not set** — frequency should stay at configured value |
| Wi-Fi / I2S | Raising to 240 increases power/thermal; Wi-Fi timing usually OK; verify I2S/sample counts and overwrite |

**Production default not changed.** To A/B 240 MHz (requires explicit approval before merging to fleet defaults):

```
# temporary local sdkconfig / defaults overlay only
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
# CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160 is not set
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240
```

Keep software build identical; measure L1/L0/overwrite/runtime `freq_mhz` for ≥500 windows. Prefer lowest MHz that meets the L1 budget with margin.

## Phase 3 — Comparison table (fill from EAR-001)

| Configuration | CPU MHz | L1 last | L1 max | L0 max | PCM overwrite |
| --- | ---: | ---: | ---: | ---: | ---: |
| Original baseline | 160 | TBD | TBD | TBD | TBD |
| Software optimized | 160 | TBD | TBD | TBD | TBD |
| Higher CPU frequency | 240 | TBD | TBD | TBD | TBD |
| Combined (opt + 240) | 240 | TBD | TBD | TBD | TBD |

**100 ms L1 target:** not confirmed on device until `L1 perf` rows are filled.

## Separate finding (out of scope)

L0 analyze remains ~160–170 ms wall-clock on S3 (soft-double IIR over SPIRAM PCM). That is independent of L1 and already exceeds 100 ms alone. Do not fold L0 work into this L1 track.

## Remaining risks

1. Physical L1 time still unknown after opt — may still need esp-dsp or 240 MHz.
2. Combined L0+L1 wall time in `ear_obs` can approach or exceed the 1 s window even if L1 &lt; 100 ms.
3. Float near energy-floor validity edge cases — monitor `invalid_window_count` on device.
4. Larger static L1 workspace (~22 KiB internal BSS).
5. Declaring success from host tests or `idf.py build` alone is invalid for this task.
