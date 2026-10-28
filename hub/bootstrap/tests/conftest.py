import pytest
from app import policy
from app.config import settings


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
    }


@pytest.fixture(autouse=True)
def use_test_policy(monkeypatch: pytest.MonkeyPatch) -> None:
    # API unit tests exercise request enforcement; Rego behavior has its own OPA tests.
    monkeypatch.setattr(policy, "evaluate", _test_policy)


@pytest.fixture(autouse=True)
def disable_hub_monitor_thread(monkeypatch: pytest.MonkeyPatch) -> None:
    # Keep unit tests deterministic; monitor cycles are exercised explicitly.
    monkeypatch.setattr(settings, "hub_monitor_enabled", False)
