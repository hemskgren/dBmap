"""Contract checks for Sensor Platform USB serial provisioning tokens."""

from __future__ import annotations

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HUB_INDEX = ROOT / "web" / "index.html"
SERIAL_C = ROOT / "firmware" / "platform" / "node_common" / "serial_config.c"
SERIAL_H = ROOT / "firmware" / "platform" / "node_common" / "include" / "sp_serial_protocol.h"
PLATFORM_README = ROOT / "firmware" / "platform" / "README.md"

# Mirrors web/index.html USB_PROVISION_*_RE (keep in sync).
USB_PROVISION_READY_RE = re.compile(
    r"(?:^|\r?\n)(?:SP_PROVISION_READY|DBMAP_PROVISION_READY)(?=\r?\n|$)"
)
USB_PROVISION_RESULT_RE = re.compile(
    r"(?:SP_PROVISION_RESULT|DBMAP_PROVISION_RESULT):(\{[^\r\n]*\})"
)


def test_firmware_emits_platform_tokens_only() -> None:
    header = SERIAL_H.read_text(encoding="utf-8")
    source = SERIAL_C.read_text(encoding="utf-8")
    assert 'SP_PROVISION_READY_TOKEN "SP_PROVISION_READY"' in header
    assert 'SP_PROVISION_RESULT_PREFIX "SP_PROVISION_RESULT:"' in header
    assert "puts(SP_PROVISION_READY_TOKEN)" in source
    assert "SP_PROVISION_RESULT_PREFIX" in source
    assert 'puts("DBMAP_PROVISION_READY")' not in source
    assert "DBMAP_PROVISION_RESULT:" not in source


def test_web_accepts_sp_and_legacy_dbmap_tokens() -> None:
    html = HUB_INDEX.read_text(encoding="utf-8")
    assert "USB_PROVISION_READY_RE" in html
    assert "USB_PROVISION_RESULT_RE" in html
    assert "SP_PROVISION_READY" in html
    assert "DBMAP_PROVISION_READY" in html
    assert "SP_PROVISION_RESULT" in html
    assert "DBMAP_PROVISION_RESULT" in html
    assert "matchUsbProvisionReady" in html
    assert "parseUsbProvisionResultLine" in html
    # New path must not wait only on the product-prefixed token.
    assert "waitForSerialLine(reader, /DBMAP_PROVISION_READY/" not in html
    assert "waitForSerialLine(reader, USB_PROVISION_READY_RE" in html


def test_ready_regex_matches_sp_and_legacy() -> None:
    assert USB_PROVISION_READY_RE.search("SP_PROVISION_READY\n")
    assert USB_PROVISION_READY_RE.search("noise\nDBMAP_PROVISION_READY\r\n")
    assert not USB_PROVISION_READY_RE.search("SP_PROVISION_READYISH\n")
    assert not USB_PROVISION_READY_RE.search("NOT_READY\n")


def test_result_regex_parses_sp_and_legacy_json() -> None:
    for prefix in ("SP_PROVISION_RESULT", "DBMAP_PROVISION_RESULT"):
        line = f'{prefix}:{{"request_id":"abc-1","status":"accepted","error":""}}\n'
        match = USB_PROVISION_RESULT_RE.search(line)
        assert match is not None
        payload = json.loads(match.group(1))
        assert payload["request_id"] == "abc-1"
        assert payload["status"] == "accepted"


def test_result_regex_rejects_truncated_json() -> None:
    assert USB_PROVISION_RESULT_RE.search("SP_PROVISION_RESULT:{not-json}\n") is not None
    # Caller must JSON.parse; contract is prefix + brace-delimited payload.
    bad = "SP_PROVISION_RESULT:accepted\n"
    assert USB_PROVISION_RESULT_RE.search(bad) is None


def test_platform_readme_documents_sp_wire_tokens() -> None:
    text = PLATFORM_README.read_text(encoding="utf-8")
    assert "SP_PROVISION_READY" in text
    assert "SP_PROVISION_RESULT:" in text
    assert "DBMAP_PROVISION_*" in text  # legacy note
