import json
import ssl
from io import BytesIO

import pytest
from app.config import settings
from app.policy import PolicyUnavailable, evaluate


def test_policy_client_posts_input_and_returns_explicit_allow(monkeypatch) -> None:
    calls = []
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ca_paths = []

    def make_context(cafile):
        ca_paths.append(cafile)
        return context

    def urlopen(request, context, timeout):
        calls.append((request, context, timeout))
        return BytesIO(json.dumps({"result": True}).encode())

    monkeypatch.setattr("app.policy.urllib.request.urlopen", urlopen)
    monkeypatch.setattr("app.policy.ssl.create_default_context", make_context)
    monkeypatch.setattr(settings, "opa_decision_url", "https://opa.test/v1/data/dbmap/authz/allow")
    monkeypatch.setattr(settings, "opa_ca_file", "/certs/ca.crt")
    monkeypatch.setattr(settings, "opa_timeout_s", 1.5)
    input_data = {"action": "session.read"}

    assert evaluate(input_data) is True

    request, context, timeout = calls[0]
    assert request.full_url == "https://opa.test/v1/data/dbmap/authz/allow"
    assert context.verify_mode == ssl.CERT_REQUIRED
    assert context.check_hostname is True
    assert ca_paths == ["/certs/ca.crt"]
    assert json.loads(request.data) == {"input": input_data}
    assert timeout == 1.5


def test_policy_client_denies_explicit_false(monkeypatch) -> None:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    monkeypatch.setattr("app.policy.ssl.create_default_context", lambda cafile: context)
    monkeypatch.setattr(
        "app.policy.urllib.request.urlopen",
        lambda request, context, timeout: BytesIO(json.dumps({"result": False}).encode()),
    )

    assert evaluate({"action": "nodes.create"}) is False


def test_policy_client_fails_closed_when_opa_has_no_decision(monkeypatch) -> None:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    monkeypatch.setattr("app.policy.ssl.create_default_context", lambda cafile: context)
    monkeypatch.setattr(
        "app.policy.urllib.request.urlopen",
        lambda request, context, timeout: BytesIO(json.dumps({}).encode()),
    )

    with pytest.raises(PolicyUnavailable, match="no decision"):
        evaluate({"action": "nodes.create"})


def test_policy_client_rejects_http_endpoint(monkeypatch) -> None:
    monkeypatch.setattr(settings, "opa_decision_url", "http://opa.test/v1/data/allow")

    with pytest.raises(PolicyUnavailable, match="must use HTTPS"):
        evaluate({"action": "nodes.create"})
