"""Fail-closed transport policy for simulator and test Ear publishers."""

import re
import urllib.error
import urllib.request
from uuid import uuid4

BROKER_HOST = "mqtt.ear-test.invalid"
BROKER_PORT = 8883
HUB_HOST = "hub.ear-test.invalid"
HUB_PORT = 8443
HUB_URL = f"https://{HUB_HOST}:{HUB_PORT}"
TEST_CA = "/test-ca.crt"


def add_live_mqtt_argument(parser) -> None:
    parser.add_argument(
        "--live-mqtt", action="store_true",
        help="Allow TLS MQTT publishing on the isolated Ear test stack",
    )


def require_live(live: bool) -> None:
    if live is not True:
        raise ValueError("live publishing requires explicit --live-mqtt opt-in")


def require_identity(node_id: object, *, test_context: bool = False) -> str:
    prefix = "TST-EAR" if test_context is True else "SIM-EAR"
    if not isinstance(node_id, str) or re.fullmatch(
        rf"{prefix}-[0-9]{{3,}}", node_id
    ) is None:
        raise ValueError(f"publishing requires a canonical {prefix}-* identity")
    return node_id


def validate_publish(
    credentials: dict, payload: dict, kind: str, *,
    live: bool = False, test_context: bool = False,
) -> str:
    require_live(live)
    node_id = require_identity(credentials.get("node_id"), test_context=test_context)
    slug = node_id.lower()
    endpoint = credentials.get("mqtt", {})
    topics = credentials.get("topics", {})
    if not isinstance(endpoint, dict) or not isinstance(topics, dict):
        raise TypeError("publishing requires MQTT endpoint and topic mappings")
    if (
        endpoint.get("host") != BROKER_HOST
        or endpoint.get("port") != BROKER_PORT
        or endpoint.get("use_tls") is not True
    ):
        raise ValueError("publishing requires the isolated TLS MQTT destination")
    if (
        endpoint.get("username") != slug
        or endpoint.get("client_id", slug) != slug
        or credentials.get("client_id", slug) != slug
        or not isinstance(endpoint.get("password"), str)
        or not endpoint["password"]
    ):
        raise ValueError("MQTT username and client ID must match the credential identity")
    if topics.get(kind) != f"esp-ear/local/{slug}/{kind}":
        raise ValueError("MQTT topic must match the credential identity and report kind")
    identity = payload.get("identity") if kind == "observation" else payload
    if not isinstance(identity, dict) or identity.get("node_id") != node_id:
        raise ValueError("payload node identity must match the credential identity")
    return f"{slug}-{uuid4().hex[:12]}"


def validate_api_destination(base_url: str, *, live: bool = False) -> None:
    require_live(live)
    if base_url != HUB_URL:
        raise ValueError("provisioning requires the isolated HTTPS Hub destination")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise urllib.error.HTTPError(req.full_url, code, msg, headers, fp)
