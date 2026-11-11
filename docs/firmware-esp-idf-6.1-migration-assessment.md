# ESP-IDF 6.1 migration assessment (Phase 2 — not started)

Status: **assessment only**. Phase 1 remains on **ESP-IDF v5.4.4**. Do not add a second builder image until this assessment is reviewed and approved.

## Current pin

| Item | Value |
| --- | --- |
| Docker base | `espressif/idf:v5.4.4@sha256:9ce51d4ace496845678065dc86f8e55fb9c25fc9f01f45597042afa7ed546926` |
| Builder metadata | `idf_version: v5.4.4` for `ear-esp32-s3` and `output-esp32` |
| Host docs | Clone/export of `v5.4.4` |

## Migration impact (expected)

| Area | Likely impact |
| --- | --- |
| I2S (`esp_driver_i2s` STD channel API) | Moderate — verify channel init/enable and clock config vs 6.1 docs |
| Wi-Fi / netif / event loop | Low–moderate — review deprecations in `esp_wifi` / `esp_netif` |
| MQTT (`esp-mqtt`) | Low if component API stable; re-run connect/reconnect tests |
| OTA / `app_update` / rollback | Moderate — confirm partition + `esp_ota_*` / app desc behaviour |
| mbedTLS / HTTPS Desired client | Moderate — cert bundle and TLS defaults often change across majors |
| Vendored `esp-ear/components/mdns` | Moderate — esp-protocols notes IDF v6 test layout; may need component bump |
| FreeRTOS / newlib | Low for current usage patterns |
| PSRAM / SPIRAM (S3 OCT) | Verify sdkconfig defaults still apply under 6.1 |

## Expected source-code changes

- Rebuild and fix compile breaks in `platform/mic`, `platform/node_common`, product Ear modules.
- Refresh vendored mdns or migrate to managed component compatible with 6.1.
- Re-validate sdkconfig.defaults (flash size, SPIRAM, MQTT, cert bundle).
- Update builder Dockerfile digest + `idf_version` checks **after** approval (keep 5.4.4 available).

## Docker / storage

- New image `espressif/idf:v6.1.x` is large (multi-GB with tools). Plan disk headroom beside the retained 5.4.4 image.
- Prefer a **separate** Compose service or profile (e.g. `firmware-build-idf61`) rather than replacing 5.4.4 until soak completes.

## Risks / benefits / rollback

| | |
| --- | --- |
| Benefits | Longer support window; newer driver fixes; alignment with Espressif current stable |
| Risks | Silent timing/I2S regressions; TLS/OTA edge cases; mdns breakage; dual-image CI cost |
| Rollback | Keep building/flashing 5.4.4 artifacts; leave Desired on known-good 0.8.x built with 5.4.4; remove 6.1 profile if needed |

## Recommendation

Complete EAR-004/005 hardware verification of Step 1 on **5.4.4**, then spike a 6.1.x patch release in an isolated builder profile before any catalog promotion.
