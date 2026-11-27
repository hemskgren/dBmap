import json
import ssl
import urllib.error
import urllib.request

from app.config import settings


class PolicyUnavailable(RuntimeError):
    pass


def evaluate(decision_input: dict) -> bool:
    request = urllib.request.Request(
        settings.opa_decision_url,
        data=json.dumps({"input": decision_input}).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    context = ssl.create_default_context(cafile=settings.opa_ca_file)
    try:
        with urllib.request.urlopen(
            request,
            context=context,
            timeout=settings.opa_timeout_s,
        ) as response:
            payload = json.load(response)
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as exc:
        raise PolicyUnavailable("Operations authorization service unavailable") from exc
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PolicyUnavailable(
            "Operations authorization returned invalid JSON"
        ) from exc
    if isinstance(payload, dict) and isinstance(payload.get("result"), bool):
        return payload["result"]
    if isinstance(payload, bool):
        return payload
    raise PolicyUnavailable("Operations authorization returned an invalid decision")
