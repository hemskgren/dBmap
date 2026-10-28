"""Contract checks for the browser USB chip-inspect helper."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "web" / "usb-chip-inspect.js"
BUNDLE = ROOT / "web" / "esptool-js" / "bundle.js"


def test_usb_chip_inspect_helper_exports_expected_surface() -> None:
    source = HELPER.read_text(encoding="utf-8")
    assert "export async function inspectEspChip" in source
    assert "export async function resetEspIntoApp" in source
    assert "HardReset" in source
    assert "export function normalizeChipMac" in source
    assert "export function classifyUsbInspectOutcome" in source
    assert "export class UsbChipInspectError" in source
    for code in (
        "unsupported",
        "timeout",
        "cancelled",
        "port_unavailable",
        "busy",
        "failed",
    ):
        assert code in source
    assert BUNDLE.is_file()
    assert (ROOT / "web" / "esptool-js" / "LICENSE").is_file()
    assert (ROOT / "web" / "esptool-js" / "UPSTREAM.txt").is_file()


def test_registration_ui_mentions_chip_variant_and_flash_before_create() -> None:
    html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
    assert "Inspect USB chip" in html
    assert "Flash firmware (then register)" in html
    assert "Chip variant" in html
    assert "usb-chip-inspect.js" in html
    assert "pulseSerialReset" in html
    assert "DOWNLOAD_BOOT" in html or "restarting on its own" in html
    assert "Triggering reboot" not in html
    assert "chip_variant" in html
    assert "registration-finalize-button" in html
    assert 'id="device-registration-panel"' in html
    assert "Save this one-time bootstrap token now" not in html
    assert "usb_metadata" not in html
    assert "dbmap-wifi-ssid" in html
    assert 'id="registration-complete-dialog"' in html
    flash_at = html.index('id="registration-flash-step"')
    created_at = html.index('id="created-token"')
    provision_at = html.index('id="registration-provision-step"')
    assert flash_at < created_at < provision_at
