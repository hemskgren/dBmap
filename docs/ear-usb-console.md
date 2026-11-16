# EAR USB serial console (multi-device)

Read-only UART log access for EAR boards over USB. Baud is **115200**. The Hub
is not on this path.

Use this when you need live `ear_obs` / AE / Platform lines from a physical EAR.
Always name the **exact** serial device when more than one ESP32 is plugged in.

## Pick the right `/dev/tty*`

Several EARs (or other ESP32 boards) often appear as `/dev/ttyACM0`,
`/dev/ttyACM1`, … (CH340 adapters commonly use `ttyACM*`; some boards use
`ttyUSB*`).

```bash
# List ports + stable USB IDs (preferred for multi-device labs)
ls -la /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
ls -la /dev/serial/by-id/

# Example by-id names (lab-dependent):
#   usb-1a86_USB_Single_Serial_<SERIAL>-if00 -> ../../ttyACM0
```

**Pass the port explicitly** to any tool (never assume `ttyACM0` is “the EAR”):

| Situation | What to do |
| --- | --- |
| Operator / agent told a port | Use that path only (e.g. `/dev/ttyACM0`) |
| Unclear which board | Ask, or match `by-id` serial to the labeled cable/board |
| Wrong port open | You will see another product’s logs or silence — switch port |

Linux permission: user must be in group `dialout` (see
[device registration](./device-registration-and-deployment.md)).

```bash
groups | tr ' ' '\n' | grep -x dialout
test -r /dev/ttyACM0 && echo readable
```

## Open the console

Set `PORT` to the chosen device.

### Quick read (no ESP-IDF)

```bash
PORT=/dev/ttyACM0   # or /dev/ttyACM1, /dev/ttyUSB0, …
stty -F "$PORT" 115200 raw -echo
timeout 5 cat "$PORT"
```

Useful for a short smoke check that EAR lines appear (`ear_obs:`, `ae:`, etc.).

### ESP-IDF monitor (preferred for interactive sessions)

This repo’s IDF install is typically under `$HOME/esp/esp-idf` (same tree the
firmware builder / local Ear builds use). `idf.py` is **not** on PATH until the
environment is sourced:

```bash
PORT=/dev/ttyACM0
. "$HOME/esp/esp-idf/export.sh"
cd firmware/esp-ear
idf.py -p "$PORT" monitor
```

Leave the monitor with `Ctrl+]`.

Flash + monitor (only when flashing was requested):

```bash
idf.py -p "$PORT" flash monitor
```

Alternatives: `screen "$PORT" 115200` or `picocom -b 115200 "$PORT"`.

## What “EAR log” looks like

With serial diagnostics enabled on the image, expect recurring lines such as:

```text
I (…) ear_obs: diag snap … rms_dbfs=… state=ACTIVE …
I (…) ear_obs: L0 win rms=… B0=… B1=… …
```

Exact tags depend on the flashed build (`serial_diag` / AE diag overlays). See
[firmware/esp-ear/README.md](../firmware/esp-ear/README.md) for field meanings.

## Agents / automation notes

- **Read-only** console attach is fine when the task needs live firmware evidence.
- Do **not** flash, erase, reboot, or reconfigure over serial unless the user
  explicitly asked (see `AGENTS.md` operational safety).
- Cursor’s default command sandbox often **hides** `/dev/ttyACM*`. Reading the
  console requires unrestricted / device-capable execution (e.g. “all”
  permissions), then the steps above with an **explicit** `PORT`.
- If the user names a port (`/dev/ttyACM1`, a `by-id` path, etc.), use that and
  do not silently switch to another device.
- When multiple adapters are present and no port was given, **ask** which board
  to open before attaching.

## Related

- Ear app / diag capture: [firmware/esp-ear/README.md](../firmware/esp-ear/README.md)
- Flash & dialout: [device-registration-and-deployment.md](./device-registration-and-deployment.md)
- USB vs Hub topology: [connectivity.md](./connectivity.md)
