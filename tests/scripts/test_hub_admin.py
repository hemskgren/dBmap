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


def test_format_device_list_matches_web_columns() -> None:
    text = hub_admin.format_device_list(
        [
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
    )

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


def test_set_desired_preserves_fields_not_explicitly_changed(monkeypatch) -> None:
    monkeypatch.setattr(
        hub_admin,
        "get_node",
        Mock(return_value={
            "desired": {
                "firmware_version": "0.1.0",
                "config_version": 3,
                "calibration_version": 7,
                "classifier_version": "rules-2",
                "payload": {"mode": "normal"},
            }
        }),
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
        Mock(return_value={
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
        }),
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
    monkeypatch.setattr(hub_admin, "get_node", Mock(return_value={"installation": None}))
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
