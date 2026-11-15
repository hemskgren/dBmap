# Sensor Platform capabilities (map)

Product-independent infrastructure. EAR policies belong under
[products/dbmap-ear/](../products/dbmap-ear/).

Primary architecture references:
[Sensor Platform architecture](../../sensor-platform-architecture.md),
[ADR 0001](../../adr/0001-sensor-platform-architecture.md).

## Implemented (verified in tree)

| Capability | Summary | Source |
| --- | --- | --- |
| Observation episode registry | Bounded concurrent episode slots; UUID minted at allocate; never reuse id after release | `firmware/platform/observation/include/sp_episode_registry.h` — `SP_OBS_EPISODE_SLOTS` (**2**), `sp_episode_registry_*` |
| Observation sequence allocator | Durable block reservation; numbers never reused; gaps OK after reboot | `firmware/platform/observation/include/sp_observation_seq.h` — `sp_obs_seq_*` |
| Mic acquisition | I2S/PCM windows, channel snapshots, background RMS helpers, capture diag | `firmware/platform/mic/` (`ear_mic.c`, `sp_mic_pcm.h`, Kconfig) |
| Acoustic DSP (L0/L1/L2) | Host-testable combined-scene L0/L1/L2 libraries; build-time `SP_AUDIO_L*` | `firmware/platform/audio/` (`sp_acoustic_l*.c`, `Kconfig`) |
| Analysis PCM retain | SPIRAM double-buffer handoff for product DSP | `CONFIG_SP_MIC_PCM_RETAIN` in `platform/audio/Kconfig`; fill path in `platform/mic` |
| Shared resource UART diag | CPU MHz, free/min heap, SPIRAM free, task stack HWM | `firmware/platform/node_common/include/sp_diag.h` — `sp_diag_log_resources` |
| Platform SemVer | Compile-time string from `firmware/platform/VERSION` | `sp_version.h` / `sp_version.c` |
| Device lifecycle / MQTT helpers | Provision, OTA, connectivity (shared `node_common`) | `firmware/platform/node_common/` |

### Episode registry capacity

| Item | Value | Class | Source |
| --- | --- | --- | --- |
| Concurrent episode slots | 2 | `configured` and Stage‑1 `architectural_limit` | `#define SP_OBS_EPISODE_SLOTS 2` in `sp_episode_registry.h` |

Slots are interchangeable (no semantic index). Product code may bind fewer than
this limit; it must not silently treat this as CE capacity or Critical capacity.

### Sequence allocation

Per-node monotonic `sequence_number` for Observation MQTT dedup with `node_id`
([protocol](../../protocol.md)). Allocator is Platform; publish helpers consume
a number when allocate returns (even if MQTT later fails).

## Partially product-coupled today

Platform owns reusable acoustic DSP under `firmware/platform/audio/`. EAR product
runtime (`ear_acoustic_runtime.c`), AE signatures, Observation, and Candidate
policy remain under `firmware/products/dbmap/ear/`. Temporary
`dbmap_acoustic_l*.h` aliases map historical names to `sp_acoustic_*`
(see `firmware/platform/audio/ALIASES.md`). Microphone Kconfig menu symbols
remain EAR-named while selecting Platform `SP_MIC_PCM_RETAIN` / `SP_AUDIO_L*`.

## Not implemented (Platform future)

Listed so the Map does not imply they exist. See also
[audio-and-candidate-engine.md](../../architecture/audio-and-candidate-engine.md) §0.5
and [ADR 0003](../../adr/0003-critical-observation-path-and-shared-evidence.md) (**Proposed**).

| Topic | Status |
| --- | --- |
| Generic Analysis Scheduler / Resource Manager | Not implemented (`proposed` direction in ADR 0003) |
| Tier / priority analysis policy | Not implemented |
| Contract catalog / multi-engine bus | Not implemented |
| Hub-persisted Event/Track entities | Not implemented (protocol Event ID reserved) |
| Critical Observation Path | **EAR product** proposal (ADR 0003) — must **not** become Platform-owned policy |

Evidence acquisition/retention vs deferred analysis scheduling is an
**architectural principle** for future Platform work; it is not a shipped
scheduler in this tree. Platform must not depend on EAR Critical types.
