# Sensor Platform (firmware)

Shared device foundation used by product firmwares (dBmap Ear today; future Nose / Monitor).

## Naming

Public Platform C APIs, headers, and ESP_LOG tags use the short prefix **`sp_`** (Sensor Platform), e.g. `sp_wifi_start`, `sp_mic.h`, tag `sp_mqtt`.

Keep unchanged:

- Kconfig / sdkconfig `CONFIG_DBMAP_*` (build and Hub catalog)
- USB wire protocol tokens `DBMAP_PROVISION_*`
- dBmap product APIs under `firmware/products/dbmap/` (`dbmap_ear_*`, acoustic L0–L2, …)

## Components

| Component | Responsibility | Used by |
| --- | --- | --- |
| `node_common` | Identity, NVS, USB serial config, Wi-Fi, MQTT, provision, Desired, OTA, health enricher table, time, generic I2S TX helper | Ear, Output |
| `mic` | Microphone hardware lifecycle, I2S RX acquisition, generic audio metrics / diagnostics, PCM window hand-off, audio reported-values enricher | Ear only |
| `observation` | Observation Foundation: sequence allocator, episode registry, generic emit-gate / MQTT wait helpers | Ear only |

Product `CMakeLists.txt` must list only the platform components they need (`esp-output` → `node_common`; `esp-ear` → `node_common` + `mic` + `observation`). Do not set `EXTRA_COMPONENT_DIRS` to this whole directory for Output — IDF would still compile `mic`/`observation` and fail without Ear Kconfig.

## Must not contain

- Drone / VMA / aircraft meaning
- Acoustic L0–L2 product schemas or analysis
- Ear activity-detector policy or Ear Observation JSON assembly
- Product-specific workflows

Product modules depend on this tree. This tree must not depend on `firmware/products/*`.
