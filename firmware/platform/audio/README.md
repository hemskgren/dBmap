# Sensor Platform acoustic DSP

Host-testable L0 / L1 / L2 algorithms with no EAR/DBMAP product dependencies.

| Layer | Sources | Kconfig |
| --- | --- | --- |
| L0 | `sp_acoustic_l0.c` | `CONFIG_SP_AUDIO_L0` |
| L1 | `sp_acoustic_l1.c` | `CONFIG_SP_AUDIO_L1` (depends on L0) |
| L2 | `sp_acoustic_l2.c` | `CONFIG_SP_AUDIO_L2` (depends on L1; fused FFT via L1) |

PCM retention for analysis windows is `CONFIG_SP_MIC_PCM_RETAIN` (implemented in
`platform/mic`). Retained channel count follows live NVS `mic_count` (1–4).

Multi-channel analysis (`sp_audio_analyze_*`) runs every build-enabled DSP layer
on every active microphone, reusing one L1 workspace serially (~22.5 KiB).

Products select these options (EAR: `DBMAP_EAR_MIC_L0` / `OBS_L1` / `OBS_L2`
`select` the Platform symbols). Microphone hardware uses generic `SP_MIC_*`
Kconfig (not EAR-named). Host tests and golden fixtures live under `test_host/`.
Compatibility aliases: `ALIASES.md`.
