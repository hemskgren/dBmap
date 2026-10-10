import json
import ssl
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

from app.config import settings


class PolicyUnavailable(RuntimeError):
    """Raised when OPA cannot return a definite authorization decision."""


def health_summary() -> dict[str, Any]:
    """Read OPA health only when a caller opens the policy-engine details."""
    decision = urllib.parse.urlparse(settings.opa_decision_url)
    health_url = urllib.parse.urlunparse(
        decision._replace(path="/health", params="", query="plugins=true", fragment="")
    )
    request = urllib.request.Request(health_url, method="GET")
    context = ssl.create_default_context(cafile=settings.opa_ca_file)
    status_code: int | None = None
    health_error: str | None = None
    try:
        with urllib.request.urlopen(request, context=context, timeout=settings.opa_timeout_s) as response:
            status_code = response.status
            body = json.load(response)
            if isinstance(body, dict):
                health_error = body.get("error") if isinstance(body.get("error"), str) else None
    except urllib.error.HTTPError as exc:
        status_code = exc.code
        try:
            body = json.load(exc)
        except (UnicodeDecodeError, ValueError):
            body = {}
        if isinstance(body, dict) and isinstance(body.get("error"), str):
            health_error = body["error"]
        else:
            health_error = "OPA reported an unhealthy state."
    except (urllib.error.URLError, TimeoutError, OSError, UnicodeError, ValueError):
        health_error = "OPA health endpoint is unavailable."

    return {
        "service": "Open Policy Agent",
        "status": "healthy" if status_code == 200 else "unavailable" if status_code is None else "unhealthy",
        "endpoint_scheme": decision.scheme,
        "decision_endpoint": decision.path,
        "health_check": "/health?plugins=true",
        "http_status": status_code,
        "plugin_health": "healthy" if status_code == 200 else "check failed",
        "message": health_error,
    }


def evaluate(input_data: dict[str, Any]) -> bool:
    """Ask OPA for an authorization decision and fail closed on every error."""
    endpoint = urllib.parse.urlparse(settings.opa_decision_url)
    if endpoint.scheme != "https" or not endpoint.hostname:
        raise PolicyUnavailable("authorization policy endpoint must use HTTPS")
    try:
        request = urllib.request.Request(
            settings.opa_decision_url,
            data=json.dumps({"input": input_data}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        context = ssl.create_default_context(cafile=settings.opa_ca_file)
        with urllib.request.urlopen(
            request, context=context, timeout=settings.opa_timeout_s
        ) as response:
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
