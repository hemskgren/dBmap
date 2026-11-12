# Firmware Sensor Platform Step 1 — implementation report

Branch: `firmware/sensor-platform-step1` (from `main` after PR #45).

**Execution boundary:** source/build verification in this report. Physical flash/OTA and on-device checks on EAR-004/005 are operator follow-up.

---

## 1. What changed

- Established `firmware/platform/` (`node_common`, `mic`, `observation`) and `firmware/products/dbmap/ear/` (`ear_acoustic`, `ear_observation`).
- Split former `ear_mic` / `ear_observation` by ownership (not wholesale moves).
- Platform C APIs / headers / log tags use **`sp_`** (Sensor Platform); product keeps `dbmap_*`. Kconfig `CONFIG_DBMAP_*` unchanged. USB provision wire tokens moved to **`SP_PROVISION_*`** (see `docs/device-provisioning-design.md`; web accepts legacy `DBMAP_PROVISION_*`).
- Ear boot: Platform mic/I2S **before** Wi-Fi (after USB gates); observation after Desired.
- Provisioned USB configure window shortened **10 s → 7 s** (`SP_USB_PROVISIONED_CONFIG_WINDOW_S`); recovery window still 180 s.
- Ear series base **0.8.0** (`sdkconfig.defaults`); Output remains **0.7.0**.
- Builder staging updated for `platform/` + `products/dbmap/ear` paths; ESP-IDF **v5.4.4** image unchanged.
- Docs: architecture ownership, this report, IDF 6.1 assessment.

## 2. Platform vs application separation

| Owner | May contain | Must not contain |
| --- | --- | --- |
| Platform | Device lifecycle, mic HW/I2S RX, generic audio metrics/diagnostics, Observation Foundation, generic audio reported values | Drone/VMA meaning, L0–L2 analysis, activity policy, Ear Observation JSON |
| `products/dbmap/ear` | Acoustic L0–L2, RMS activity, acoustic Observation payload/publisher | Platform depending on it |

Dependency direction: product → platform only.

## 3. Code structure and interfaces

```text
firmware/platform/
  node_common/     # Wi-Fi, MQTT, OTA, NVS, health enricher table, …
  mic/             # I2S RX, levels, PCM hand-off (dbmap_mic_pcm.h)
  observation/     # seq, episode registry, generic gate / MQTT wait helpers
firmware/products/dbmap/ear/
  ear_acoustic/    # L0–L2 + ear_acoustic_runtime (dbmap_ear_acoustic.h)
  ear_observation/ # activity, payload, publisher, ear_obs_ota_policy
firmware/esp-ear/main/  # orchestrates platform then product boot
```

Key interfaces:

- Platform mic: `dbmap_ear_mic_start`, levels/channel/stream, `dbmap_mic_*` PCM API, health enricher for `audio.*`.
- Product acoustic: `dbmap_ear_acoustic_register`, `dbmap_ear_mic_l0_poll` / get_l0|l1|l2_* (historical names kept).
- Platform observation: `dbmap_obs_seq_*`, `dbmap_episode_registry_*`, gate/MQTT wait helpers.
- Product observation: `dbmap_ear_observation_*`, `dbmap_obs_ota_action_for_state`.

## 4. File assignment (from former trees)

| Former location | Now | Layer |
| --- | --- | --- |
| `ear_mic` I2S/metrics/background/channel | `platform/mic` | Platform |
| `acoustic_l0/l1/l2` + analysis poll/getters | `products/dbmap/ear/ear_acoustic` | Product |
| `observation_seq`, `episode_registry`, gate helpers | `platform/observation` | Platform |
| activity, payload, `ear_observation.c`, OTA activity policy | `products/dbmap/ear/ear_observation` | Product |
| `node_common` | `platform/node_common` | Platform |

## 5. Build results (ESP-IDF 5.4.4)

| Check | Result |
| --- | --- |
| Host IDF | `ESP-IDF v5.4.4` |
| `idf.py set-target esp32s3` + `idf.py build` | **Success** — App version **0.8.0** |
| Components resolved | `node_common`, `mic`, `observation`, `ear_acoustic`, `ear_observation` |
| Builder unit tests | `firmware-builder/tests` — 28 passed |
| Boot-order host test | `test_ear_boot_order: ok` |
| Docker builder image | Unchanged `espressif/idf:v5.4.4@sha256:…` (not rebuilt this session; daemon unavailable) |

## 6. Startup sequence and I2S-before-Wi-Fi evidence

**Source contract**

1. USB config gates  
2. `platform_boot`: `starting Platform mic/I2S before Wi-Fi` → mic start  
3. `platform_boot`: `starting Wi-Fi after mic/I2S` → `dbmap_wifi_start`  
4. MQTT / OTA / Desired  
5. `dbmap_app`: observation after Desired  

Host predicates: mic allowed without network; obs only after Desired ([`ear_boot_order`](../firmware/esp-ear/main/ear_boot_order.c)).

**Hardware-pending:** confirm on EAR-004 (optional EAR-005) that console shows `platform_boot` mic lines before `wifi_init` / Wi-Fi associate, and that capture continues if Wi-Fi fails.

## 7. Deviations, risks, remaining tests

- `dbmap_ota_register_acoustic_close` name remains Ear-flavoured in platform (generic hook; rename later).
- Ear RX I2S stays in `mic`; Output TX still uses `node_common` `net_i2s` (dual stacks).
- Observation JSON remains acoustic-shaped for Ear publish (pluggable envelope later).
- `output_components` path not moved under `products/` in this step.
- On-device DMA soak, MQTT/OTA after new boot order: **hardware-pending**.

## 8. Firmware version and EAR-004/005 prep

| Item | Value |
| --- | --- |
| Series base | `0.8.0` in `firmware/esp-ear/sdkconfig.defaults` |
| Builder target | `ear-esp32-s3` / hardware `esp32-s3-n16r8` (not keyed by node id) |
| Intended devices | Primary **EAR-004**; optional **EAR-005** |
| Do not disturb | EAR-001/002 working 0.7.x images until operators choose Desired/OTA |

Build for verification: enable Compose profile `firmware-build` **or** host `idf.py build` as above; flash/OTA separately.

## 9. ESP-IDF 6.1

Assessment only: [firmware-esp-idf-6.1-migration-assessment.md](./firmware-esp-idf-6.1-migration-assessment.md). No second container installed.
