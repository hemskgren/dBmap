"""Contract checks for the Hub Web USB console dialog."""

from pathlib import Path

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
    assert "Console open. Press Reset on the board if logs do not appear." in html
    assert "USB console open at 115200 baud" not in html
    assert "USB_APP_SERIAL_BAUD_RATE" not in html


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
