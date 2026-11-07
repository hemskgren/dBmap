import sys
from argparse import Namespace
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

import pytest

spec = spec_from_file_location(
    "dbmap_hub_admin",
    Path(__file__).parents[2] / "scripts" / "hub_admin.py",
)
assert spec is not None and spec.loader is not None
hub_admin = module_from_spec(spec)
spec.loader.exec_module(hub_admin)


def _sample_devices() -> list[dict]:
    return [
        {
            "node_id": "OUT-007",
            "node_type": "output",
            "status": "online",
            "provisioning_state": "provisioned",
            "lifecycle_state": "active",
            "owner_display_name": None,
            "installation": {"latitude": 59.91, "longitude": 10.75},
        },
        {
            "node_id": "EAR-001",
            "node_type": "ear",
            "status": "offline",
            "provisioning_state": "pending",
            "lifecycle_state": "deactivated",
            "owner_display_name": "Ops",
            "installation": None,
        },
    ]


def test_format_device_list_matches_web_columns() -> None:
    text = hub_admin.format_device_list(_sample_devices())

    lines = text.splitlines()
    assert lines[0].startswith("Device")
    assert "Type" in lines[0]
    assert "Status" in lines[0]
    assert "Provisioning" in lines[0]
    assert "Lifecycle" in lines[0]
    assert "Assigned owner" in lines[0]
    assert "Location" in lines[0]
    assert lines[1].startswith("EAR-001")
    assert "EAR" in lines[1]
    assert "offline" in lines[1]
    assert "Ops" in lines[1]
    assert "Not set" in lines[1]
    assert lines[2].startswith("OUT-007")
    assert "OUTPUT" in lines[2]
    assert "online" in lines[2]
    assert "Unassigned" in lines[2]
    assert "59.91, 10.75" in lines[2]
    assert "Reported state" not in text
    assert "Installation (Hub-managed)" not in text


def test_format_device_list_detail_shows_reported_and_installation() -> None:
    nodes = {
        "EAR-001": {
            "reported": {
                "firmware_version": "0.2.0",
                "hardware_revision": "EAR-V1",
                "config_version": 3,
                "calibration_version": 1,
                "classifier_version": "rules-2",
                "uptime_s": 120,
            },
            "installation": None,
        },
        "OUT-007": {
            "reported": {
                "firmware_version": "1.0.0",
                "hardware_revision": "OUT-V1",
                "config_version": 0,
                "calibration_version": 0,
                "classifier_version": "",
                "uptime_s": 0,
            },
            "installation": {
                "latitude": 59.91,
                "longitude": 10.75,
                "orientation_deg": 180.0,
                "height_m": 12.5,
            },
        },
    }
    text = hub_admin.format_device_list_detail(_sample_devices(), nodes)
    summary = hub_admin.format_device_list(_sample_devices())
    assert text.startswith(summary)
    assert "=== EAR-001 ===" in text
    assert "=== OUT-007 ===" in text
    assert "Reported state (device)" in text
    assert "Installation (Hub-managed)" in text
    assert "Firmware version" in text
    assert "0.2.0" in text
    assert "Hardware revision" in text
    assert "EAR-V1" in text
    assert "Configuration version" in text
    assert "Calibration version" in text
    assert "Classifier version" in text
    assert "Uptime (s)" in text
    assert "120" in text
    assert "Latitude" in text
    assert "59.91" in text
    assert "Longitude" in text
    assert "10.75" in text
    assert "Orientation (deg)" in text
    assert "180.0" in text
    assert "Height (m)" in text
    assert "12.5" in text


def test_format_device_list_detail_missing_values_are_em_dash() -> None:
    nodes = {
        "EAR-001": {"reported": {}, "installation": None},
        "OUT-007": {
            "reported": {
                "firmware_version": "",
                "hardware_revision": "   ",
                "config_version": None,
                "calibration_version": None,
                "classifier_version": None,
                "uptime_s": None,
            },
            "installation": {"latitude": 1.0, "longitude": 2.0},
        },
    }
    text = hub_admin.format_device_list_detail(_sample_devices(), nodes)
    assert hub_admin.MISSING_VALUE == "—"
    # EAR-001 has no reported/installation fields populated.
    ear_block = text.split("=== EAR-001 ===", 1)[1].split("=== OUT-007 ===", 1)[0]
    assert ear_block.count("—") >= 6
    assert "Orientation (deg)" in ear_block
    assert "Height (m)" in ear_block
    out_block = text.split("=== OUT-007 ===", 1)[1]
    assert "Firmware version" in out_block
    assert "—" in out_block
    assert "1.0" in out_block
    assert "2.0" in out_block


def test_list_parser_accepts_detail_flag() -> None:
    parser = hub_admin.build_parser()
    args = parser.parse_args(["list", "--detail"])
    assert args.command == "list"
    assert args.detail is True
    default = parser.parse_args(["list"])
    assert default.detail is False


def test_set_desired_preserves_fields_not_explicitly_changed(monkeypatch) -> None:
    monkeypatch.setattr(
        hub_admin,
        "get_node",
        Mock(
            return_value={
                "desired": {
                    "firmware_version": "0.1.0",
                    "config_version": 3,
                    "calibration_version": 7,
                    "classifier_version": "rules-2",
                    "payload": {"mode": "normal"},
                }
            }
        ),
    )
    request = Mock(return_value={"node_id": "EAR-001"})
    monkeypatch.setattr(hub_admin, "request_json", request)
    args = Namespace(
        ca="ca.crt",
        node_id="EAR-001",
        firmware_version=None,
        config_version=4,
        calibration_version=None,
        classifier_version=None,
        payload_file=None,
    )

    result = hub_admin.set_desired(args, "https://hub.example", "test-token")

    assert result == {"node_id": "EAR-001"}
    assert request.call_args.args[4] == "/api/v1/nodes/EAR-001/desired"
    assert request.call_args.args[5] == {
        "firmware_version": "0.1.0",
        "config_version": 4,
        "calibration_version": 7,
        "classifier_version": "rules-2",
        "payload": {"mode": "normal"},
    }


def test_set_installation_preserves_unspecified_fields(monkeypatch) -> None:
    monkeypatch.setattr(
        hub_admin,
        "get_node",
        Mock(
            return_value={
                "installation": {
                    "latitude": 59.9,
                    "longitude": 10.7,
                    "floor": 7,
                    "height_m": 21.0,
                    "height_accuracy_m": 2.0,
                    "mount_type": "balcony",
                    "environment": "urban",
                    "orientation_deg": 180.0,
                }
            }
        ),
    )
    request = Mock(return_value={"node_id": "EAR-001"})
    monkeypatch.setattr(hub_admin, "request_json", request)
    args = Namespace(
        ca="ca.crt",
        node_id="EAR-001",
        latitude=None,
        longitude=None,
        floor=None,
        height_m=None,
        height_accuracy_m=None,
        mount_type=None,
        environment=None,
        orientation_deg=270,
    )

    hub_admin.set_installation(args, "https://hub.example", "test-token")

    assert request.call_args.args[5] == {
        "latitude": 59.9,
        "longitude": 10.7,
        "floor": 7,
        "height_m": 21.0,
        "height_accuracy_m": 2.0,
        "mount_type": "balcony",
        "environment": "urban",
        "orientation_deg": 270,
    }


def test_set_installation_requires_existing_or_updated_coordinates(monkeypatch) -> None:
    monkeypatch.setattr(
        hub_admin, "get_node", Mock(return_value={"installation": None})
    )
    args = Namespace(
        ca="ca.crt",
        node_id="EAR-001",
        latitude=None,
        longitude=None,
        floor=2,
        height_m=None,
        height_accuracy_m=None,
        mount_type=None,
        environment=None,
        orientation_deg=None,
    )

    with pytest.raises(ValueError, match="latitude and longitude are required"):
        hub_admin.set_installation(args, "https://hub.example", "test-token")
