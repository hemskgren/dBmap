# Sensor Platform (firmware)

Shared device foundation used by product firmwares (dBmap Ear today; future Nose / Monitor).

## Version

Authoritative SemVer for this tree is the single-line file [`VERSION`](./VERSION)
(currently **0.2.0**). The firmware builder injects it as `config.sp.version` in
build metadata. It is **independent** of the product `firmware_version`
(Ear/Output sdkconfig / catalog top-level field).

### SemVer policy (Platform)

| Bump | When |
| --- | --- |
| **MAJOR** | Breaking change to a published Platform API, wire contract, or health/observation foundation that existing product firmwares must change to consume |
| **MINOR** | Backward-compatible Platform capability (new API, optional field, new acquire mode) that existing callers can ignore |
| **PATCH** | Bug fix or internal change with no intentional contract change |

Do not bump Platform version for product-only Engine/Observation policy changes
under `firmware/products/`.

## Naming

Public Platform C APIs, headers, and ESP_LOG tags use the short prefix **`sp_`** (Sensor Platform), e.g. `sp_wifi_start`, `sp_mic.h`, tag `sp_mqtt`.

Keep unchanged:

- Kconfig / sdkconfig `CONFIG_DBMAP_*` (build and Hub catalog)
- dBmap product APIs under `firmware/products/dbmap/` (`dbmap_ear_*`, acoustic L0–L2, …)

USB serial provisioning wire tokens are **platform-owned**:

- Current emit: `SP_PROVISION_READY`, `SP_PROVISION_RESULT:` (see `sp_serial_protocol.h`)
- Legacy (no longer emitted): `DBMAP_PROVISION_*` — Hub web UI still accepts them so
  older flashed images can be provisioned during transition (`sp.version` **0.2.0**)

## Components

| Component | Responsibility | Used by |
| --- | --- | --- |
| `node_common` | Identity, NVS, USB serial config, Wi-Fi, MQTT, provision, Desired, OTA, health enricher table, time, generic I2S TX helper | Ear, Output |
| `mic` | Microphone hardware lifecycle, I2S RX acquisition (≤4 via DIN0+DIN1 dual STD RX), per-channel metrics + background hist, PCM window hand-off (primary), audio reported-values enricher | Ear only |
| `observation` | Observation Foundation: sequence allocator, episode registry, generic emit-gate / MQTT wait helpers | Ear only |

Product `CMakeLists.txt` must list only the platform components they need (`esp-output` → `node_common`; `esp-ear` → `node_common` + `mic` + `observation`). Do not set `EXTRA_COMPONENT_DIRS` to this whole directory for Output — IDF would still compile `mic`/`observation` and fail without Ear Kconfig.

## Must not contain

- Drone / VMA / aircraft meaning
- Acoustic L0–L2 product schemas or analysis
- Ear activity-detector policy or Ear Observation JSON assembly
- Product-specific workflows

Product modules depend on this tree. This tree must not depend on `firmware/products/*`.
