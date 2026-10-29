# ESP-Ear firmware

Initial Ear application for ESP32-S3 + one I2S MEMS microphone.

## Scope (first milestone)

- Reuses `node_common` for Wi-Fi, HTTPS provision, MQTT, OTA Desired poll, hello/health.
- Captures from **one** microphone model per image (currently **INMP441** via `ear_mic`).
- Attaches windowed RMS/peak **dBFS** levels on hello/health/keepalive reported state (same path as `wifi_rssi_dbm`). Hub shows them under **Additional reported values** until a dedicated Ear UI exists.
- JSON uses `audio.microphones[]` / `capabilities.microphones[]` so a later multi-mic Ear can add indexes without a schema break.
- Does **not** include Output playback, ML observations, or a separate audio MQTT topic.

## Reported audio shape (excerpt)

```json
{
  "wifi_rssi_dbm": -62,
  "capabilities": {
    "role": "ear",
    "hardware_profile": "esp32-s3-n16r8",
    "microphones": [{ "index": 0, "model": "INMP441" }]
  },
  "audio": {
    "units": "dBFS",
    "subsystem_status": "ok",
    "sample_rate_hz": 16000,
    "sample_format": "int32_from_i2s32_left_justified_24",
    "microphones": [
      {
        "index": 0,
        "model": "INMP441",
        "channels": 1,
        "rms_dbfs": -42.5,
        "peak_dbfs": -28.1,
        "sample_count": 16000,
        "window_ms": 1000,
        "status": "ok"
      }
    ]
  }
}
```

Levels are digital full-scale (**dBFS**), not calibrated dB SPL.

## Hardware

Full wiring (Output + Ear, reserved 4-mic map): [`docs/device-wiring-and-pinout.md`](../../docs/device-wiring-and-pinout.md).

| Item | Value | Status |
|------|-------|--------|
| Hardware profile | `esp32-s3-n16r8` | **Verified** (ESPConnect): ESP32-S3 QFN56 rev v0.2, 16 MB flash (`0x184068`), 8 MB PSRAM (AP Memory 3.3 V), dual-core up to 240 MHz |
| Mic model | INMP441 | One model per Ear image |
| Mic index | **0** (0-based) | First mic only today; indexes 1–3 reserved |
| I2S pins | BCLK=4, WS=5, DIN0=6; DIN1=7 reserved | Placeholder — verify free GPIOs on this board |
| Slot alignment | 32-bit Philips, left slot, 24-bit left-justified `>> 8` | Unverified — confirm on scope/logic analyzer |
| L/R strap (mic 0) | Left (GND) | Unverified on board |

## Build

Prefer the Hub **Firmware → Build reusable firmware** target `ear-esp32-s3` (series `0.5.*`).

The builder container has **no internet**. Ear vendors `espressif/mdns` **1.14.1** at `esp-ear/components/mdns` (local IDF component) so the registry is never contacted. Ear components live under `firmware/ear_components/` (not shared with Output’s Helix/LittleFS tree under `firmware/output_components/`).

After builder `server.py` / component-tree changes, recreate with a **rebuild**:

```bash
docker compose --profile firmware-build up -d --build --force-recreate firmware-builder nginx
```

Local IDF: place the Hub CA at `main/root_ca.pem` (gitignored `*.pem`, same as Output), then:

```bash
cd firmware/esp-ear
idf.py set-target esp32s3
idf.py build
```
