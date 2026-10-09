import json
import urllib.error
import urllib.request
from typing import Any

from app.config import settings


class PolicyUnavailable(RuntimeError):
    """Raised when OPA cannot return a definite authorization decision."""


def evaluate(input_data: dict[str, Any]) -> bool:
    """Ask OPA for an authorization decision and fail closed on every error."""
    try:
        request = urllib.request.Request(
            settings.opa_decision_url,
            data=json.dumps({"input": input_data}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=settings.opa_timeout_s) as response:
            body = json.load(response)
    except (
        urllib.error.URLError,
        TimeoutError,
        OSError,
        UnicodeError,
        ValueError,
    ) as exc:
        raise PolicyUnavailable("authorization policy service is unavailable") from exc

    result = body.get("result") if isinstance(body, dict) else None
    if not isinstance(result, bool):
        raise PolicyUnavailable("authorization policy service returned no decision")
    return result
