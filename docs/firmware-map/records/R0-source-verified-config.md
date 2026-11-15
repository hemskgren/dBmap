# R0 source-verified configuration snapshot

**Value class for all rows below:** `configured` or `source-verified` (not device-measured).  
**Git baseline:** `e271c60` + R0 instrumentation branch.  
**Composition:** `dbmap-ear` × `esp32-s3-n16r8` × IDF `v5.4.4` × builder `ear-esp32-s3`.

| Item | Value | Source |
| --- | --- | --- |
| Sample rate | 16000 Hz | `CONFIG_SP_MIC_SAMPLE_RATE_HZ` |
| Window | 1000 ms | `CONFIG_SP_MIC_WINDOW_MS` |
| PCM sample type | `int32_t` (4 B) | `ear_mic.c` alloc |
| PCM cap samples/ch | 16176 | `expected + expected/100 + 16` |
| PCM SPIRAM mono total | 129408 B | `sp_mic_pcm_bytes_total` @ 1 ch |
| PCM SPIRAM stereo total | 258816 B | same @ 2 ch |
| L1 workspace | ≈22.5 KiB shared serial | `sp_audio_analyze.h` |
| AE history | 32 | `AE_HISTORY_CAPACITY` |
| CE candidates | 8 | `CE_MAX_CANDIDATES` |
| Platform episode slots | 2 | `SP_OBS_EPISODE_SLOTS` |
| Manager event Q | 8 | `EAR_OBS_MGR_EVENT_Q` |
| Critical active Obs | 1 | `ear_critical_path` |
| `sp_mic` / `ear_obs` stack | 6144 B each | task creates |
| I2S DMA defaults | desc=6, frame=240 | IDF `I2S_CHANNEL_DEFAULT_CONFIG`; EAR does not override |
| CPU / tick for this R0 snapshot | 160 MHz / 100 Hz | Historical R0 via `sdkconfig.defaults.r0_ref` (or older local `sdkconfig`) |
| Product CPU / tick (current defaults) | 240 MHz / 100 Hz | `firmware/esp-ear/sdkconfig.defaults` — not this R0 row |

Platform version file: `firmware/platform/VERSION` (0.2.0 at baseline).
