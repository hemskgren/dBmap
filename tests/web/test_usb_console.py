"""Contract checks for the Hub Web USB console dialog."""

import shutil
import subprocess
from pathlib import Path

import pytest

HUB_INDEX = Path(__file__).resolve().parents[2] / "web" / "index.html"


def test_usb_console_log_capacity_is_bounded_at_460_kib() -> None:
    html = HUB_INDEX.read_text(encoding="utf-8")
    assert "USB_CONSOLE_LOG_KEEP_CHARS = 460 * 1024" in html
    assert "USB_CONSOLE_LOG_TRIM_AT_CHARS" in html
    assert "slice(-USB_CONSOLE_LOG_KEEP_CHARS)" in html
    assert "slice(-160000)" not in html
    assert "length > 200000" not in html
    assert "Live serial at 115200 baud" in html
    assert "In-browser log retention is about 460" in html
    assert "Log buffer ~460 KiB" not in html
    assert "setUsbConsoleOpenStatus" in html
    assert "noteUsbConsoleChunk" in html
    assert "USB_CONSOLE_NODE_ID_RE" in html
    assert "USB_CONSOLE_HUB_ID_RE" in html
    assert "Identifying…" in html
    assert "Last connected" in html
    assert "usb-console-notice-aside" in html
    assert "USB console open at 115200 baud" not in html
    assert "USB_APP_SERIAL_BAUD_RATE" not in html
    assert "Console open. Press Reset on the board if logs do not appear." not in html
    assert "resolveUsbConsoleDeviceId" not in html
    assert "Console open · ${id}. Reset board if no logs." not in html


def test_usb_console_identity_regex_accepts_current_and_legacy_boot_lines() -> None:
    """Mirror the Hub web parsers so regressions are caught outside the browser."""
    import re

    node_re = re.compile(
        r"(?:DBMAP_DEVICE_ID=|(?:^|[^\w])node_id=)([A-Z0-9][A-Z0-9_-]{0,63})\b"
    )
    hub_re = re.compile(
        r"(?:DBMAP_HUB_ID=|(?:^|[^\w])hub_id=)([A-Za-z0-9][A-Za-z0-9._-]{0,63})\b"
    )

    current = "I (1234) platform_boot: node_id=EAR-001\nI (1235) platform_boot: hub_id=local-01\n"
    legacy = "DBMAP_DEVICE_ID=EAR-002\nDBMAP_HUB_ID=hub.example\n"
    noise = "mqtt payload node_id_field=EAR-999 my_node_id=BAD\n"

    assert node_re.search(current).group(1) == "EAR-001"
    assert hub_re.search(current).group(1) == "local-01"
    assert node_re.search(legacy).group(1) == "EAR-002"
    assert hub_re.search(legacy).group(1) == "hub.example"
    assert node_re.search(noise) is None


def test_usb_console_has_no_reset_device_control() -> None:
    html = HUB_INDEX.read_text(encoding="utf-8")
    assert "Reset device" not in html
    assert "usb-console-reset" not in html
    assert 'id="usb-console-clear"' in html
    assert 'id="usb-console-download"' in html


def test_usb_console_dialog_is_wider_than_shared_dialog_chrome() -> None:
    html = HUB_INDEX.read_text(encoding="utf-8")
    assert "#usb-console-dialog" in html
    assert "min(96vw, 1280px)" in html
    assert "#usb-console-output" in html
    assert "white-space: pre;" in html


def test_usb_console_has_persistent_multi_term_highlight() -> None:
    html = HUB_INDEX.read_text(encoding="utf-8")
    assert 'id="usb-console-find-0"' in html
    assert 'id="usb-console-find-1"' in html
    assert 'id="usb-console-find-2"' in html
    assert "usbConsoleBuffer" in html
    assert "collectUsbConsoleHighlights" in html
    assert "mark.usb-hl-0" in html
    assert "usb-console-hint" in html
    assert "usb-console-find" in html


def test_usb_console_has_buffer_meter() -> None:
    html = HUB_INDEX.read_text(encoding="utf-8")
    assert 'id="usb-console-meter"' in html
    assert "updateUsbConsoleMeter" in html
    assert "formatUsbConsoleAge" in html
    assert "setInterval(updateUsbConsoleMeter, 5000)" in html
    assert "usb-console-meter-full" in html


def test_usb_console_session_in_javascript() -> None:
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node.js required to run the USB console session test")
    html = HUB_INDEX.read_text(encoding="utf-8")
    start = html.index("      let usbConsoleSession = null;")
    end = html.index("      function saveDeploymentSession", start)
    subprocess.run(
        [node, str(Path(__file__).with_name("usb_console_session.cjs"))],
        input=html[start:end], text=True, check=True, timeout=10,
    )
