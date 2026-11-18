# EAR AE signature capture (AS-001 calibration)

Bounded, compile-time opt-in UART capture of Audio Engine signatures for
comparing host-extracted AS-001 WAV features with live EAR-001 microphone
features.

## Build metadata / firmware-map key

| Key | Where | Values |
| --- | --- | --- |
| `AE_SIG_CAPTURE` | `capabilities.observation` on hello/health/keepalive | `true` / `false` |

Source: `CONFIG_DBMAP_EAR_AE_SIG_CAPTURE`. Kconfig symbol default is **n**; EAR
`sdkconfig.defaults` sets it **y**. Hub List config key: `EAR_AE_SIG_CAPTURE`
(under `config.product.dbmap.observation`).

## Build

Normal EAR defaults already enable capture:

```bash
cd firmware/esp-ear
idf.py fullclean   # if an old sdkconfig lacks the flag
idf.py set-target esp32s3
idf.py build
```

Optional explicit overlay (same effect): `sdkconfig.defaults.ae_sig_capture`.

Firmware builder / Hub **List config** shows the effective value after the
`firmware-builder` image is rebuilt (it embeds `dbmap_config.py`).

Flash/OTA as usual. Confirm health JSON shows
`capabilities.observation.AE_SIG_CAPTURE: true`.

## Runtime behavior

1. On AE start: capture session arms after `AE_SIG_CAPTURE_SETTLE_N` (2) published
   signatures.
2. Next signatures are stored in a fixed RAM buffer (`AE_SIG_CAPTURE_CAPACITY` = 48).
3. UART prints `AE_SIG_CSV_BEGIN` (includes CSV header), then lines
   `AE_SIG_CSV <row>`, then `AE_SIG_CSV_END` with `stored` / `drained` / `overflow`.
4. Hot path only copies feature fields (+ pure `as_profile_match` for AS-001
   columns). Formatting/logging is drained a few lines per AE poll.
5. When the buffer is full, further signatures increment `overflow` and are not
   stored. Capture stops accepting new rows.

Does **not** change AE math, CE, P2, Observation lifecycle, or MQTT.

## Extract CSV

```bash
uv run python scripts/ae_sig_capture_extract.py dbmap-usb-console.txt -o /tmp/ear_sigs.csv --require-complete
```

## Recommended EAR-001 test

1. Build/flash EAR (defaults already enable capture; or use overlay
   `sdkconfig.defaults.ae_sig_capture`).
2. Attach USB console; reboot EAR-001.
3. Confirm `ae_sig_capture enabled` and `AE_SIG_CAPTURE: true`.
4. Within ~50 s of AE start, play `AS001_heli_bg_mono16k.wav` (or the 48 kHz
   original) near the left mic (AE uses channel 0 / L0).
5. Wait for `AE_SIG_CSV_END` (overflow should be 0).
6. Save the console log; run the extract script.
7. Compare host `signatures.csv` vs EAR CSV (`rms_dbfs`, bands, `dominant_hz`,
   `flatness`, `as001_*` columns).
