import hashlib
import importlib.util
import json
import os
import sys
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import pytest

SCRIPT_PATH = Path(__file__).resolve().parents[3] / "scripts" / "hub_firmware.py"
sys.path.insert(0, str(SCRIPT_PATH.parent))
SCRIPT_SPEC = importlib.util.spec_from_file_location("hub_firmware", SCRIPT_PATH)
assert SCRIPT_SPEC is not None and SCRIPT_SPEC.loader is not None
hub_firmware = importlib.util.module_from_spec(SCRIPT_SPEC)
SCRIPT_SPEC.loader.exec_module(hub_firmware)


def _firmware_zip(path: Path) -> bytes:
    manifest = {
        "target": "esp-output-esp32",
        "node_type": "output",
        "chip": "esp32",
        "hardware_revision": "OUTPUT-DEV-V1",
        "firmware_version": "1.2.3",
        "esp_idf_version": "v5.4.2",
        "configuration_version": 0,
    }
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("manifest.json", json.dumps(manifest))
    return path.read_bytes()


def test_create_metadata_matches_zip_and_normalizes_build_date(tmp_path):
    bundle_path = tmp_path / "esp-output-test.zip"
    bundle = _firmware_zip(bundle_path)

    metadata_path = hub_firmware.create_metadata(bundle_path, "2026-10-13T10:00:00+02:00")
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))

    assert metadata_path.name == "esp-output-test.json"
    assert metadata["firmware_id"] == "esp-output-test"
    assert metadata["build_date_utc"] == "2026-10-13T08:00:00Z"
    assert metadata["platform"] == "esp32"
    assert metadata["artifact_sha256"] == hashlib.sha256(bundle).hexdigest()


def test_upload_checks_catalog_then_posts_pair_as_candidate(tmp_path):
    bundle_path = tmp_path / "esp-output-test.zip"
    _firmware_zip(bundle_path)
    metadata_path = hub_firmware.create_metadata(bundle_path, "2026-10-13T08:00:00Z")
    ca_path = tmp_path / "ca.crt"
    ca_path.write_text("test CA", encoding="utf-8")
    candidate = {"firmware_id": "esp-output-test", "status": "candidate"}

    with patch.object(
        hub_firmware,
        "_request",
        side_effect=[(404, {"detail": "not found"}), (201, candidate)],
    ) as request:
        result = hub_firmware.upload(
            SimpleNamespace(
                ca=str(ca_path),
                bundle=str(bundle_path),
                metadata=str(metadata_path),
            ),
            "https://hub.local:8443",
            "x" * 32,
        )

    assert result == candidate
    assert request.call_count == 2
    assert request.call_args_list[0].args[3:5] == (
        "GET",
        "/api/v1/firmware/catalog/esp-output-test",
    )
    upload_call = request.call_args_list[1]
    assert upload_call.args[3:5] == ("POST", "/api/v1/firmware/catalog")
    assert b'filename="esp-output-test.zip"' in upload_call.args[5]
    assert b'filename="esp-output-test.json"' in upload_call.args[5]


def test_list_reads_catalog_and_validates_response(tmp_path):
    ca_path = tmp_path / "ca.crt"
    ca_path.write_text("test CA", encoding="utf-8")
    entries = [{"firmware_id": "firmware-1", "status": "candidate"}]

    with patch.object(hub_firmware, "_request", return_value=(200, entries)) as request:
        result = hub_firmware.list_firmware(str(ca_path), "x" * 32, "https://hub.local:8443")

    assert result == entries
    assert request.call_args.args[3:5] == ("GET", "/api/v1/firmware/catalog")


def test_no_arguments_prints_help(capsys, monkeypatch):
    monkeypatch.setattr("sys.argv", ["hub_firmware.py"])

    assert hub_firmware.main() == 0
    assert "usage:" in capsys.readouterr().out


def test_hub_host_defaults_to_environment(monkeypatch):
    monkeypatch.setenv("DBMAP_LAN_IP", "192.0.2.10")

    args = hub_firmware.build_parser().parse_args(["list"])

    assert args.command == "list"
    assert args.hub_host == "192.0.2.10"
    assert args.hub_port == 8443
    assert args.ca == "shared/trust/ca.crt"


def test_project_env_defaults_are_loaded_without_overriding_environment(tmp_path, monkeypatch):
    env_file = tmp_path / ".env"
    env_file.write_text(
        "DBMAP_LAN_IP=192.0.2.10\n"
        "DBMAP_ADMIN_TOKEN='private-token'\n"
        "DBMAP_HTTPS_PORT=9443\n",
        encoding="utf-8",
    )
    monkeypatch.delenv("DBMAP_LAN_IP", raising=False)
    monkeypatch.delenv("DBMAP_ADMIN_TOKEN", raising=False)
    monkeypatch.setenv("DBMAP_HTTPS_PORT", "10443")

    hub_firmware.load_project_env(tmp_path)
    args = hub_firmware.build_parser().parse_args(["list"])

    assert args.hub_host == "192.0.2.10"
    assert args.hub_port == 10443
    assert os.environ["DBMAP_ADMIN_TOKEN"] == "private-token"


@pytest.mark.parametrize(
    ("firmware_id", "build_date_utc"),
    [
        ("bad$id", "2026-10-13T08:00:00Z"),
        ("valid-id", "2026-10-13T08:00:00"),
        ("valid-id", "not-a-date"),
    ],
)
def test_create_metadata_rejects_invalid_id_or_build_date(tmp_path, firmware_id, build_date_utc):
    bundle_path = tmp_path / f"{firmware_id}.zip"
    _firmware_zip(bundle_path)

    with pytest.raises(hub_firmware.HubFirmwareError):
        hub_firmware.create_metadata(bundle_path, build_date_utc)
