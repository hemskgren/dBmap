/**
 * USB chip inspection for dBmap Device Registration.
 * Uses vendored esptool-js (Apache-2.0) bootloader sync — the same class of
 * ROM protocol ESPConnect uses — then always disconnects so app-serial /
 * ESP Web Tools can reopen the port.
 */
import { ESPLoader, HardReset, Transport } from "./esptool-js/bundle.js";

function sleep(ms) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

const PLATFORM_BY_CHIP_NAME = {
  ESP8266: "esp8266",
  ESP32: "esp32",
  "ESP32-S2": "esp32s2",
  "ESP32-S3": "esp32s3",
  "ESP32-C2": "esp32c2",
  "ESP32-C3": "esp32c3",
  "ESP32-C5": "esp32c5",
  "ESP32-C6": "esp32c6",
  "ESP32-C61": "esp32c61",
  "ESP32-H2": "esp32h2",
  "ESP32-P4": "esp32p4",
};

export class UsbChipInspectError extends Error {
  /**
   * @param {"unsupported"|"timeout"|"cancelled"|"port_unavailable"|"busy"|"failed"} code
   * @param {string} message
   */
  constructor(code, message) {
    super(message);
    this.name = "UsbChipInspectError";
    this.code = code;
  }
}

/**
 * Normalize a MAC string to uppercase colon-separated form, or null.
 * @param {string|null|undefined} value
 * @returns {string|null}
 */
export function normalizeChipMac(value) {
  if (!value || typeof value !== "string") return null;
  const hex = value.replace(/[^0-9a-fA-F]/g, "").toUpperCase();
  if (hex.length !== 12) return null;
  return hex.match(/.{2}/g).join(":");
}

/**
 * Classify inspect/identity outcomes for UI and tests.
 * @param {{ok?: boolean, code?: string, name?: string}|null|undefined} errorLike
 */
export function classifyUsbInspectOutcome(errorLike) {
  if (!errorLike) return "success";
  if (errorLike.code === "unsupported") return "unsupported";
  if (errorLike.code === "timeout") return "timeout";
  if (errorLike.code === "cancelled" || errorLike.name === "NotFoundError") return "cancelled";
  if (errorLike.code === "port_unavailable" || errorLike.code === "busy") return "port_unavailable";
  return "failed";
}

/**
 * Leave download/bootloader mode and boot the application firmware.
 * Uses esptool's HardReset (release RTS), not ClassicReset which holds IO0 for download.
 *
 * @param {SerialPort} port Open or closed port with setSignals support
 */
export async function resetEspIntoApp(port) {
  if (!port?.setSignals) return;
  await sleep(100);
  // Ensure IO0/DTR are not held for download, then pulse/release EN via RTS.
  await port.setSignals({ dataTerminalReady: false, requestToSend: false });
  await sleep(50);
  await port.setSignals({ dataTerminalReady: false, requestToSend: true });
  await sleep(100);
  await port.setSignals({ dataTerminalReady: false, requestToSend: false });
  await sleep(250);
}

/**
 * Read chip family, ESPConnect-style chip variant, and MAC via ROM bootloader.
 * Hard-resets into app firmware before closing so a follow-up serial listen can
 * see DBMAP_DEVICE_ID instead of DOWNLOAD_BOOT.
 *
 * @param {SerialPort} port
 * @param {{ timeoutMs?: number }} [options]
 * @returns {Promise<{chipFamily: string, chipName: string, chipVariant: string, macAddress: string, revision: number|null}>}
 */
export async function inspectEspChip(port, options = {}) {
  if (!port) {
    throw new UsbChipInspectError("port_unavailable", "No USB serial port is available.");
  }
  if (port.readable || port.writable) {
    throw new UsbChipInspectError(
      "busy",
      "The USB serial port is already open. Close the console or other USB tools first.",
    );
  }

  const timeoutMs = Number.isFinite(options.timeoutMs) ? options.timeoutMs : 15000;
  const transport = new Transport(port, false);
  const terminal = {
    clean() {},
    writeLine() {},
    write() {},
  };
  const loader = new ESPLoader({
    transport,
    baudrate: 115200,
    romBaudrate: 115200,
    terminal,
  });

  let settled = false;
  const work = (async () => {
    await loader.detectChip("default_reset", 7);
    if (!loader.chip) {
      throw new UsbChipInspectError(
        "unsupported",
        "Could not identify an Espressif chip on this USB serial port.",
      );
    }
    const chipName = loader.chip.CHIP_NAME || "unknown";
    const chipFamily = PLATFORM_BY_CHIP_NAME[chipName] || chipName.toLowerCase().replace(/\s+/g, "");
    let chipVariant = chipName;
    let revision = null;
    let macAddress = null;
    try {
      if (typeof loader.chip.getChipDescription === "function") {
        chipVariant = await loader.chip.getChipDescription(loader);
      }
      if (typeof loader.chip.getChipRevision === "function") {
        revision = await loader.chip.getChipRevision(loader);
      }
      if (typeof loader.chip.readMac === "function") {
        macAddress = normalizeChipMac(await loader.chip.readMac(loader));
      }
    } catch (error) {
      throw new UsbChipInspectError(
        "unsupported",
        error?.message || "Chip registers could not be read (secure download mode or unsupported hardware).",
      );
    }
    if (!macAddress) {
      throw new UsbChipInspectError("unsupported", "Chip MAC address could not be read.");
    }
    // Exit ROM download mode before releasing the port for app-serial identity.
    try {
      await new HardReset(transport).reset();
    } catch {
      try {
        await transport.setSignals?.(false, false);
      } catch {
        /* ignore */
      }
    }
    await sleep(300);
    settled = true;
    return {
      chipFamily,
      chipName,
      chipVariant,
      macAddress,
      revision: Number.isFinite(revision) ? revision : null,
    };
  })();

  const timeout = new Promise((_, reject) => {
    window.setTimeout(() => {
      reject(
        new UsbChipInspectError(
          "timeout",
          `Chip identification timed out after ${Math.round(timeoutMs / 1000)} seconds.`,
        ),
      );
    }, timeoutMs);
  });

  try {
    return await Promise.race([work, timeout]);
  } catch (error) {
    if (error instanceof UsbChipInspectError) throw error;
    if (error?.name === "NotFoundError") {
      throw new UsbChipInspectError("cancelled", "No USB serial port was selected.");
    }
    const message = error?.message || String(error);
    if (/sync|Couldn't sync|Failed to connect|connect/i.test(message)) {
      throw new UsbChipInspectError(
        "timeout",
        "Could not sync with the ESP bootloader. Check the USB cable and try again.",
      );
    }
    throw new UsbChipInspectError("failed", message);
  } finally {
    try {
      await transport.disconnect();
    } catch {
      /* port may already be closed */
    }
    try {
      if (port.readable || port.writable) await port.close();
    } catch {
      /* ignore */
    }
  }
}
