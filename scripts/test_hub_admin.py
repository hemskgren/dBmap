from argparse import Namespace
from unittest.mock import Mock

import pytest

import hub_admin


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

    result = hub_admin.set_desired(args, "https://hub.example:8443", "test-token")

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

    hub_admin.set_installation(args, "https://hub.example:8443", "test-token")

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
        hub_admin.set_installation(args, "https://hub.example:8443", "test-token")
