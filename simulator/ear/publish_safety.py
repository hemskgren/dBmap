"""Fail-closed transport policy for simulator and test Ear publishers."""

import argparse
import os
import re
import urllib.error
import urllib.request
from pathlib import Path
from urllib.parse import urlsplit
from uuid import uuid4

from lib.hub_cli import HubCliError, load_project_env

REPO_ROOT = Path(__file__).resolve().parents[2]
BROKER_HOST = "mqtt.ear-test.invalid"
BROKER_PORT = 8883
HUB_HOST = "hub.ear-test.invalid"
HUB_PORT = 8443
HUB_URL = f"https://{HUB_HOST}:{HUB_PORT}"
DEFAULT_CA = "shared/trust/ca.crt"


def load_simulator_environment() -> None:
    try:
        load_project_env(REPO_ROOT)
    except HubCliError as exc:
        raise ValueError(f"simulator configuration failed: {exc}") from exc
    configured_mqtt_destination()
    try:
        https_port = int(os.environ.get("DBMAP_HTTPS_PORT", "8443"))
    except ValueError as exc:
        raise ValueError("DBMAP_HTTPS_PORT must be an integer") from exc
    if not 1 <= https_port <= 65535:
        raise ValueError("DBMAP_HTTPS_PORT must be between 1 and 65535")


def resolve_repo_path(value: str | Path) -> Path:
    path = Path(value)
    return path if path.is_absolute() else REPO_ROOT / path


def add_ca_argument(parser: argparse.ArgumentParser, *, subcommand: bool = False) -> None:
    parser.add_argument(
        "--ca",
        default=argparse.SUPPRESS if subcommand else os.environ.get("DBMAP_CA_FILE", DEFAULT_CA),
        help=f"TLS CA certificate (default: {os.environ.get('DBMAP_CA_FILE', DEFAULT_CA)})",
    )


def add_mqtt_arguments(
    parser: argparse.ArgumentParser, *, subcommand: bool = False
) -> None:
    default = argparse.SUPPRESS if subcommand else None
    parser.add_argument(
        "--mqtt-host",
        default=default,
        help="Override the provisioned host (configured local or isolated test broker only)",
    )
    parser.add_argument(
        "--mqtt-port",
        type=int,
        default=default,
        help="Override the provisioned port (must match an authorized destination)",
    )


def configured_mqtt_destination() -> tuple[str, int]:
    host = os.environ.get("DBMAP_LOCAL_MQTT_HOST", "hub.local")
    try:
        port = int(os.environ.get("DBMAP_LOCAL_MQTT_PORT", "8883"))
    except ValueError as exc:
        raise ValueError("DBMAP_LOCAL_MQTT_PORT must be an integer") from exc
    if not host or not 1 <= port <= 65535:
        raise ValueError("configured local MQTT destination is invalid")
    return host, port


def resolve_mqtt_destination(
    endpoint: dict,
    *,
    mqtt_host: str | None = None,
    mqtt_port: int | None = None,
    live: bool = False,
) -> tuple[str, int]:
    configured_host, configured_port = configured_mqtt_destination()
    host = mqtt_host if mqtt_host is not None else endpoint.get("host")
    port = mqtt_port if mqtt_port is not None else endpoint.get("port")
    if (
        not isinstance(host, str)
        or not host
        or isinstance(port, bool)
        or not isinstance(port, int)
        or not 1 <= port <= 65535
    ):
        raise ValueError("MQTT destination host and port are invalid")
    if endpoint.get("use_tls") is not True:
        raise ValueError("simulator publishing requires TLS MQTT")
    if (host, port) == (BROKER_HOST, BROKER_PORT):
        require_live(live)
        return host, port
    if (host, port) == (configured_host, configured_port):
        return host, port
    raise ValueError(
        "MQTT destination is not authorized; it must match the configured "
        "DBMAP_LOCAL_MQTT_HOST/DBMAP_LOCAL_MQTT_PORT or the isolated TLS test broker "
        f"({BROKER_HOST}:{BROKER_PORT})"
    )


def resolve_common_mqtt_destination(
    credentials_list: list[dict],
    *,
    mqtt_host: str | None = None,
    mqtt_port: int | None = None,
    live: bool = False,
) -> tuple[str, int]:
    if not credentials_list:
        raise ValueError("at least one simulator Ear is required")
    destinations = {
        resolve_mqtt_destination(
            credentials.get("mqtt", {}),
            mqtt_host=mqtt_host,
            mqtt_port=mqtt_port,
            live=live,
        )
        for credentials in credentials_list
    }
    if len(destinations) != 1:
        raise ValueError("all selected simulated Ears must use the same MQTT destination")
    return next(iter(destinations))


def add_live_mqtt_argument(parser) -> None:
    parser.add_argument(
        "--live-mqtt", action="store_true",
        help="Opt in to the isolated TLS test broker; the configured local broker needs no flag",
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
    mqtt_host: str | None = None, mqtt_port: int | None = None,
) -> str:
    node_id = require_identity(credentials.get("node_id"), test_context=test_context)
    slug = node_id.lower()
    endpoint = credentials.get("mqtt", {})
    topics = credentials.get("topics", {})
    if not isinstance(endpoint, dict) or not isinstance(topics, dict):
        raise TypeError("publishing requires MQTT endpoint and topic mappings")
    resolve_mqtt_destination(
        endpoint, mqtt_host=mqtt_host, mqtt_port=mqtt_port, live=live
    )
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
    parsed = urlsplit(base_url)
    try:
        port = parsed.port
    except ValueError:
        port = None
    configured_hosts = {
        host
        for host in (
            os.environ.get("DBMAP_LAN_IP"),
            os.environ.get("DBMAP_LOCAL_MQTT_HOST", "hub.local"),
        )
        if host
    }
    try:
        configured_port = int(os.environ.get("DBMAP_HTTPS_PORT", "8443"))
    except ValueError as exc:
        raise ValueError("DBMAP_HTTPS_PORT must be an integer") from exc
    configured_path = os.environ.get("DBMAP_PUBLIC_BASE_PATH", "").rstrip("/")
    if base_url == HUB_URL:
        require_live(live)
        return
    if (
        parsed.scheme == "https"
        and parsed.hostname in configured_hosts
        and port == configured_port
        and parsed.path.rstrip("/") == configured_path
        and not parsed.username
        and not parsed.password
        and not parsed.query
        and not parsed.fragment
    ):
        return
    raise ValueError(
        "provisioning destination is not authorized; use the configured local Hub "
        "address, or the isolated HTTPS test Hub with --live-mqtt"
    )


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise urllib.error.HTTPError(req.full_url, code, msg, headers, fp)
