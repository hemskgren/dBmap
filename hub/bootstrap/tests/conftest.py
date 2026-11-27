from tempfile import TemporaryDirectory

import paho.mqtt.client as mqtt
import pytest
from app import policy
from app.config import settings


def pytest_configure(config: pytest.Config) -> None:
    # Apply before test modules can import app.main and initialize transports.
    patch = pytest.MonkeyPatch()
    directory = TemporaryDirectory(prefix="dbmap-hub-unit-")
    config.add_cleanup(directory.cleanup)
    config.add_cleanup(patch.undo)
    patch.setattr(settings, "database_url", f"sqlite:///{directory.name}/bootstrap.db")
    patch.setattr(settings, "mqtt_enabled", False)
    patch.setattr(settings, "mqtt_password", "")
    patch.setattr(settings, "mqtt_admin_password", "")
    patch.setattr(settings, "public_mqtt_monitor_enabled", False)

    def reject_live_mqtt(*_args, **_kwargs):
        raise AssertionError("Hub unit tests cannot connect to an MQTT broker")

    for method in ("connect", "connect_async", "reconnect"):
        patch.setattr(mqtt.Client, method, reject_live_mqtt)


def _test_policy(input_data: dict) -> bool:
    subject = input_data.get("subject", {})
    if not subject.get("authenticated"):
        return False
    if subject.get("role") == "admin":
        return True
    return subject.get("role") == "viewer" and input_data.get("action") in {
        "session.read",
        "devices.list_summary",
        "nodes.read_detail",
        "users.read",
        "groups.read",
        "firmware.catalog.read",
        "hub.status.read",
        "hub.logs.read",
        "observations.read",
    }


@pytest.fixture(autouse=True)
def use_test_policy(monkeypatch: pytest.MonkeyPatch) -> None:
    # API unit tests exercise request enforcement; Rego behavior has its own OPA tests.
    monkeypatch.setattr(policy, "evaluate", _test_policy)


@pytest.fixture(autouse=True)
def disable_hub_monitor_thread(monkeypatch: pytest.MonkeyPatch) -> None:
    # Keep unit tests deterministic; monitor cycles are exercised explicitly.
    monkeypatch.setattr(settings, "hub_monitor_enabled", False)
    from app import main

    monkeypatch.setattr(main, "start_device_monitor_thread", lambda: None)
