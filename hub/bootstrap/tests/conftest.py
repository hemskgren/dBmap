import pytest
from app import policy


def _test_policy(input_data: dict) -> bool:
    subject = input_data.get("subject", {})
    if not subject.get("authenticated"):
        return False
    if subject.get("role") == "admin":
        return True
    return subject.get("role") == "viewer" and input_data.get("action") in {
        "session.read",
        "devices.list_summary",
    }


@pytest.fixture(autouse=True)
def use_test_policy(monkeypatch: pytest.MonkeyPatch) -> None:
    # API unit tests exercise request enforcement; Rego behavior has its own OPA tests.
    monkeypatch.setattr(policy, "evaluate", _test_policy)
