# Investigation: EAR-001 onboard LED behaviour

**Status:** Source investigation only (no firmware or config changes).  
**Scope:** Current `esp-ear` / `ear_components` / `node_common` tree and pinout docs.  
**Question:** What controls the green and blue LEDs the operator sees blinking on EAR-001?

---

## Summary table

| LED | GPIO | Control location | Blink pattern | Trigger / meaning |
| --- | --- | --- | --- | --- |
| **Green** | **Not defined in this repo** | **No application control found** | **Unknown** (not implemented in Ear firmware) | **Not an Ear status indicator in software** — any green light is outside `esp-ear` application code |
| **Blue** | **Not defined in this repo** | **No application control found** | **Unknown** (not implemented in Ear firmware) | **Not an Ear status indicator in software** — any blue light is outside `esp-ear` application code |

There is **no** Ear LED driver, status task, timer, state machine, Kconfig pin, or polarity setting in the current firmware.

---

## Confirmed from source

### 1. Ear firmware does not drive LEDs

Searched with no matches in Ear application paths:

| Path | Result |
| --- | --- |
| `firmware/esp-ear/main/` | No `gpio_set_level` / `gpio_config` / LED APIs |
| `firmware/ear_components/` | No LED / blink / status-GPIO code (I2S GPIOs only) |
| `firmware/components/node_common/` | No LED GPIO control |
| `firmware/esp-ear/sdkconfig.defaults` | No LED / RGB / blink options |

`app_main` (`firmware/esp-ear/main/app_main.c`) starts NVS, diag, serial config, Wi‑Fi, time, provision, MQTT, OTA/Desired, mic, and observation only. It never configures an LED GPIO.

Documented pinout (`docs/device-wiring-and-pinout.md`):

- Ear section lists **I2S only** (BCLK/WS/DIN placeholders 4/5/6/7).
- Source-of-truth table: Ear **Relay / LED = `—`**.
- Only Output documents an onboard LED (**GPIO2**).

### 2. No status→LED mapping on Ear

These subsystems publish serial logs / MQTT / Hub telemetry only — **no LED side effects** in code:

| Subsystem | Entry points (examples) | LED? |
| --- | --- | --- |
| Wi‑Fi | `dbmap_wifi_start` (`node_common`) | No |
| MQTT | `dbmap_mqtt_*` | No |
| Mic / I2S | `dbmap_ear_mic_start` (`ear_mic.c`) | No |
| Observation / L0–L1 | `dbmap_ear_observation_start` (`ear_observation.c`) | No |
| Health / hello | `dbmap_health_*` | No |
| Diag counters | `dbmap_diag_*` (`diag.c`) | No |
| OTA / Desired | `dbmap_ota_*` / `dbmap_desired_*` | No |
| USB serial config | `dbmap_serial_config_*` | No |

Therefore blinking on EAR-001 **cannot** be interpreted from this tree as Wi‑Fi reconnect, MQTT loss, audio activity, observation processing, or a firmware “error LED.”

### 3. Bootloader / app config

Ear sdkconfig defaults do not enable an IDF status-LED or RGB blinker. No board header assigns green/blue pins. No schematic for EAR-001 is in the repository.

### 4. Contrast — Output device only (not EAR-001)

The **only** onboard LED implementation in the firmware monorepo is ESP-Output:

| Item | Value |
| --- | --- |
| File | `firmware/esp-output/main/output_command.c` |
| GPIO | `ONBOARD_LED_GPIO = GPIO_NUM_2` |
| Init | `output_command_init()` — `gpio_config` output, level 0 |
| Drive | `gpio_set_level` from `update_outputs` / relay start-stop |
| Timer | `esp_timer` period `LED_UPDATE_INTERVAL_US = 50000` (50 ms) |
| Relay alert blink | ~1 s period, 50 % duty while relay timed on |
| `output-test` | Double pulse inside 2 s period |
| Polarity | Treated **active-high** (`1` = on) |

That path is **not linked into `esp-ear`** and does not explain EAR-001 LEDs.

---

## Answers to operator questions

| Question | Answer from code |
| --- | --- |
| Can both LEDs blink simultaneously? | **Application firmware does not blink either.** If both are blinking on the hardware, that concurrency is **not coordinated by Ear software**. |
| Expected during normal idle? | **Not as an Ear status feature.** Idle Ear app is not supposed to toggle green/blue LEDs. Steady power LEDs on a module could still be “normal hardware,” but that is **not confirmed in-repo**. |
| Warning / reconnect / error pattern? | **No Ear firmware pattern** maps blink cadence to warning, reconnect, or error. Do not treat observed blinks as Hub/MQTT fault codes from this image. |
| Hardware or bootloader vs app? | **App: does not control LEDs.** Bootloader: no LED config found in Ear defaults. **Hardware / module / USB / power circuitry** remain plausible unexplained sources. |

---

## Unresolved (cannot be closed from this repository)

1. **Physical identity of the green and blue LEDs** on EAR-001 (which net, which GPIO, active-high/low, discrete vs addressable RGB). No schematic / PCB silkscreen map in-repo.
2. **Whether EAR-001 is a custom PCB or a DevKit-class S3 module** with factory RGB (often GPIO48 on Espressif DevKitC-1) plus a separate power LED — hardware profile string is only `esp32-s3-n16r8`.
3. **Why the operator sees blinking** while running current Ear firmware — must be explained by measurement outside this tree (GPIO probe, schematic, module datasheet, or confirming the flashed image is actually `esp-ear`).
4. **Polarity** of any onboard LEDs — undefined for Ear.
5. Whether a **non-repo factory image**, USB-serial adapter LED, or enclosure power LED is what is being observed.

---

## Confirmed vs unresolved

### Confirmed

- Current **Ear application firmware does not assign, initialize, toggle, or blink** green or blue LEDs.
- Pinout docs intentionally leave Ear LED blank.
- No Wi‑Fi / MQTT / audio / observation / health / OTA LED indicator exists in Ear code.
- Observed dual-LED blinking is **not** an implemented Ear status language in this codebase.
- The only in-tree LED blinker is **ESP-Output GPIO2**, unrelated to Ear.

### Unresolved

- Hardware netlist / GPIO for the visible green and blue parts.
- Whether blink is power, USB, module RGB default, or another process.
- Any meaning of the observed pattern (period, duty, sync between colours).

**Practical next step (out of scope here):** identify the LED nets on the EAR-001 board (schematic or continuity), then optionally probe those GPIOs while Ear runs to prove they are not driven by the MCU under the current image.
