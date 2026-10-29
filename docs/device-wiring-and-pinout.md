# Device wiring and pinout

Hardware GPIO maps for **ESP-Output** (ESP32) and **ESP-Ear** (ESP32-S3). Firmware Kconfig defaults match this document; treat any pin marked **placeholder** as unverified until checked on the physical board.

Microphone and capability JSON use **0-based** indexes (`index: 0` is the first mic). Do not number the first mic as 1 in wiring labels or Hub telemetry.

## ESP-Output (ESP32 + MAX98357 + relay)

Target image: `esp-output` / catalog `output-esp32`. Classic ESP32 (not S3).

| Function | GPIO | Notes | Status |
| --- | --- | --- | --- |
| Relay coil drive | **16** | Active-high; confirm module polarity before connecting | In firmware (`RELAY_1`) |
| Onboard LED | **2** | Diagnostic blink during relay / Output test only — not a commanded output | In firmware |
| MAX98357 BCLK | **26** | I2S bit clock | In firmware (`output_audio`) |
| MAX98357 LRC / WS | **25** | I2S word select | In firmware |
| MAX98357 DIN | **22** | MCU **DOUT** → amp DIN | In firmware |
| MAX98357 SD_MODE | *(none)* | Default leave floating / always on (`-1`) | Optional |

Audio playback does **not** use the relay pin. Play/stop commands drive the I2S amp path above (Helix → MAX98357).

### Output electrical notes

- Relay: verify active-high vs active-low on the module; add a flyback diode if the coil is discrete.
- LED on GPIO2 is only a bench cue while the relay is asserted or during `output-test`; operators wiring production Outputs can ignore it.
- Do not reuse 16 / 26 / 25 / 22 for other peripherals on the same board without a firmware change.

## ESP-Ear (ESP32-S3 N16R8 + INMP441)

Target image: `esp-ear` / catalog `ear-esp32-s3`. Hardware profile string: `esp32-s3-n16r8`.

### Mic indexing

| Logical mic | JSON `index` | Role (planned) |
| --- | ---: | --- |
| Mic 0 | **0** | First / primary (wired today) |
| Mic 1 | 1 | Second on same DIN (INMP441 L/R pair) |
| Mic 2 | 2 | Third (second DIN pair) |
| Mic 3 | 3 | Fourth (second DIN pair) |

Current firmware captures and reports **index 0 only**. Schema already uses `microphones[]` so 2–4 mics can be added without a Hub protocol break.

### Shared I2S bus (preferred for up to 4× INMP441)

INMP441 is I2S MEMS with an L/R select pin. Best practice on one controller:

1. Share **BCLK** and **WS** across all mics.
2. Put two mics on one **DIN** by tying one L/R to GND (left) and one to VDD (right).
3. Add a second **DIN** for the next stereo pair → four mics total.

| Net | GPIO | Mic mapping | Status |
| --- | ---: | --- | --- |
| I2S BCLK | **4** | Shared | Placeholder (Kconfig) |
| I2S WS / LRCLK | **5** | Shared | Placeholder (Kconfig) |
| I2S DIN0 | **6** | Mic **0** (L) + reserved Mic **1** (R) | Placeholder; only L/mic0 used today |
| I2S DIN1 | **7** | Reserved Mic **2** (L) + Mic **3** (R) | Reserved — not read yet |
| I2S MCLK | — | Unused (`-1`) | — |
| Mic 0 L/R | GND | Left slot → `index` 0 | Unverified on board |
| Mic 1 L/R | VDD | Right slot → `index` 1 | Future |
| Mic 2 L/R | GND | Left on DIN1 → `index` 2 | Future |
| Mic 3 L/R | VDD | Right on DIN1 → `index` 3 | Future |

Also wire each INMP441: **VDD** 3.3 V, **GND**, **SCK**→BCLK, **WS**→WS, **SD**→its DIN. Decouple VDD near the mic.

### ESP32-S3 pin caution (N16R8)

On octal-flash / octal-PSRAM modules, **do not** use the SPI0/1 flash–PSRAM GPIO set for I2S (commonly in the mid-20s / low-30s depending on module). Prefer the reserved 4–7 map above until a board schematic freezes the assignment. Avoid strapping and USB-JTAG pins (0, 3, 19, 20, 45, 46) unless the design explicitly allows it.

### Ear electrical notes

- Levels in Hub telemetry are **dBFS**, not calibrated SPL.
- Slot format assumed today: 32-bit Philips, 24-bit left-justified data (`>> 8`) — confirm on a scope before trusting absolute levels.
- Room and placement differences between Ears are expected; they do not by themselves mean a defective mic.

## Source of truth in firmware

| Device | Relay / LED | Audio I2S |
| --- | --- | --- |
| Output | `firmware/esp-output/main/output_command.c` (`GPIO_NUM_16`, `GPIO_NUM_2`) | `firmware/output_components/output_audio/Kconfig` |
| Ear | — | `firmware/ear_components/ear_mic/Kconfig` |

When changing pins, update Kconfig (or the Output command constants), rebuild/flash, and keep this document in sync.
