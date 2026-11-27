import json
import ssl
from io import BytesIO

from app import policy
from app.config import settings


def test_evaluate_posts_opa_input_envelope(monkeypatch) -> None:
    calls = []
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    endpoint = "https://opa.test/v1/data/dbmap/authz/allow"

    def urlopen(request, context, timeout):
        calls.append((request, context, timeout))
        return BytesIO(json.dumps({"result": True}).encode())

    monkeypatch.setattr(policy.urllib.request, "urlopen", urlopen)
    monkeypatch.setattr(policy.ssl, "create_default_context", lambda cafile: context)
    monkeypatch.setattr(settings, "opa_decision_url", endpoint)
    decision_input = {
        "subject": {"id": "local-admin", "role": "admin", "authenticated": True},
        "action": "operations.status.read",
        "resource": {"type": "operations", "id": None},
        "context": {},
    }

    assert policy.evaluate(decision_input) is True

    request, _context, _timeout = calls[0]
    assert json.loads(request.data) == {"input": decision_input}
    assert request.full_url == endpoint
