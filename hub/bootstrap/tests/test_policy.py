import json
from io import BytesIO

import pytest
from app.config import settings
from app.policy import PolicyUnavailable, evaluate


def test_policy_client_posts_input_and_returns_explicit_allow(monkeypatch) -> None:
    calls = []

    def urlopen(request, timeout):
        calls.append((request, timeout))
        return BytesIO(json.dumps({"result": True}).encode())

    monkeypatch.setattr("app.policy.urllib.request.urlopen", urlopen)
    monkeypatch.setattr(settings, "opa_decision_url", "http://opa.test/v1/data/dbmap/authz/allow")
    monkeypatch.setattr(settings, "opa_timeout_s", 1.5)
    input_data = {"action": "session.read"}

    assert evaluate(input_data) is True

    request, timeout = calls[0]
    assert request.full_url == "http://opa.test/v1/data/dbmap/authz/allow"
    assert json.loads(request.data) == {"input": input_data}
    assert timeout == 1.5


def test_policy_client_denies_explicit_false(monkeypatch) -> None:
    monkeypatch.setattr(
        "app.policy.urllib.request.urlopen",
        lambda request, timeout: BytesIO(json.dumps({"result": False}).encode()),
    )

    assert evaluate({"action": "nodes.create"}) is False


def test_policy_client_fails_closed_when_opa_has_no_decision(monkeypatch) -> None:
    monkeypatch.setattr(
        "app.policy.urllib.request.urlopen",
        lambda request, timeout: BytesIO(json.dumps({}).encode()),
    )

    with pytest.raises(PolicyUnavailable, match="no decision"):
        evaluate({"action": "nodes.create"})
