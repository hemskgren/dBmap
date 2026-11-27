from __future__ import annotations

import http.client
import json
import socket
import ssl
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlsplit
from urllib.request import Request, urlopen
from uuid import uuid4

import paho.mqtt.client as mqtt

from app import container_runtime
from app.config import settings


def https_status(
    host: str, port: int, path: str, server_name: str
) -> tuple[str, str | None]:
    try:
        context = ssl.create_default_context(cafile=settings.opa_ca_file)
        with (
            socket.create_connection(
                (host, port),
                timeout=settings.probe_timeout_s,
            ) as raw,
            context.wrap_socket(
                raw,
                server_hostname=server_name,
            ) as tls,
        ):
            tls.settimeout(settings.probe_timeout_s)
            request = (
                f"GET {path} HTTP/1.1\r\n"
                f"Host: {server_name}\r\n"
                "Connection: close\r\n\r\n"
            ).encode()
            tls.sendall(request)
            payload = b""
            while b"\r\n\r\n" not in payload and len(payload) < 8192:
                block = tls.recv(8192)
                if not block:
                    break
                payload += block
        line = payload.split(b"\r\n", 1)[0]
        parts = line.split()
        if len(parts) < 2:
            return "offline", "Invalid HTTP response"
        status = int(parts[1])
        return ("ok", None) if status == 200 else ("offline", f"HTTP {status}")
    except (OSError, http.client.HTTPException, ssl.SSLError, ValueError) as exc:
        return "offline", f"{type(exc).__name__}: {str(exc)[:160]}"


def probe_api() -> tuple[str, str | None]:
    state, reason = https_status(
        settings.hub_api_host,
        settings.hub_api_port,
        "/health",
        "hub-bootstrap",
    )
    return state, reason


def probe_database() -> tuple[str, str | None]:
    details, error = hub_component_details("database")
    if details is None:
        return "unknown", error or "Hub database diagnostic is unavailable"
    if details.get("status") == "online":
        return "ok", None
    status = details.get("status")
    return "unknown", f"Hub database diagnostic reports {status or 'unknown'}"


def hub_api_json(
    path: str,
    *,
    response_limit: int = 262_144,
) -> tuple[dict | None, str | None]:
    if not settings.admin_token:
        return None, "Hub diagnostics credentials are not configured"
    if not path.startswith("/api/v1/") or response_limit < 1:
        raise ValueError("Hub diagnostic path or response limit is invalid")
    try:
        context = ssl.create_default_context(cafile=settings.opa_ca_file)
        request = Request(
            f"https://{settings.hub_api_host}:{settings.hub_api_port}{path}",
            headers={"Authorization": f"Bearer {settings.admin_token}"},
            method="GET",
        )
        with urlopen(
            request,
            context=context,
            timeout=settings.probe_timeout_s,
        ) as response:
            payload = response.read(response_limit + 1)
            if len(payload) > response_limit:
                return None, "Hub diagnostic exceeded its response limit"
            parsed = json.loads(payload)
        if not isinstance(parsed, dict):
            return None, "Hub diagnostic returned an invalid response"
        return parsed, None
    except HTTPError as exc:
        if exc.code in (401, 403):
            return None, "Hub diagnostic was not authorized"
        return None, f"Hub diagnostic returned HTTP {exc.code}"
    except (URLError, OSError, TimeoutError, ssl.SSLError) as exc:
        return None, f"Hub diagnostic is unavailable: {type(exc).__name__}"
    except (UnicodeDecodeError, json.JSONDecodeError):
        return None, "Hub diagnostic returned invalid JSON"


_HUB_COMPONENTS = {
    "hub-api",
    "database",
    "nginx",
    "web",
    "mosquitto",
    "opa",
}


def hub_component_details(component: str) -> tuple[dict | None, str | None]:
    if component not in _HUB_COMPONENTS:
        raise ValueError("Hub component is not allowlisted")
    return hub_api_json(
        f"/api/v1/hub/components/{quote(component, safe='')}/details"
    )


def probe_runtime(service: str) -> tuple[dict | None, str | None]:
    if service not in container_runtime.RUNTIME_SERVICES:
        raise ValueError("Operations service is not allowlisted for runtime diagnostics")
    try:
        return container_runtime.inspect_runtime(service), None
    except (
        container_runtime.ContainerNotFound,
        container_runtime.ContainerRuntimeUnavailable,
    ) as exc:
        return None, str(exc)


def inspect_monitored_runtimes() -> dict[str, dict]:
    return container_runtime.inspect_monitored_runtimes()


def fetch_service_logs(service: str) -> tuple[dict | None, str | None]:
    if service not in container_runtime.RUNTIME_SERVICES:
        raise ValueError("Operations service is not allowlisted for log retrieval")
    try:
        return container_runtime.read_tail(service), None
    except (
        container_runtime.ContainerNotFound,
        container_runtime.ContainerRuntimeUnavailable,
    ) as exc:
        return None, str(exc)


def probe_nginx() -> tuple[str, str | None]:
    path = f"{settings.public_base_path.rstrip('/')}/health"
    base = settings.nginx_probe_name
    return https_status(
        settings.nginx_probe_host,
        settings.nginx_probe_port,
        path,
        base,
    )


def probe_nginx_local() -> tuple[str, str | None]:
    path = f"{settings.public_base_path.rstrip('/')}/api/v1/nginx/health"
    state, reason = https_status(
        settings.nginx_probe_host,
        settings.nginx_probe_port,
        path,
        settings.nginx_probe_name,
    )
    return state, reason


def probe_web() -> tuple[str, str | None]:
    return https_status(
        settings.web_probe_host,
        settings.web_probe_port,
        "/health",
        settings.web_probe_host,
    )


def probe_opa() -> tuple[str, str | None]:
    state, reason = https_status(
        settings.opa_host,
        settings.opa_port,
        "/health?plugins=true",
        settings.opa_host,
    )
    return state, reason


def probe_mqtt_broker_diagnostics() -> tuple[str, str | None, dict[str, object]]:
    diagnostics: dict[str, object] = {
        "endpoint": f"{settings.mqtt_host}:{settings.mqtt_port}",
        "transport": "TLS",
        "protocol": "MQTT 3.1.1",
        "check": "CONNECT/CONNACK",
        "connack_code": None,
        "connack_name": None,
    }

    def finish(state: str, reason: str | None):
        diagnostics["status"] = state
        if reason:
            diagnostics["reason"] = reason
        return state, reason, diagnostics

    if not settings.mqtt_password:
        return finish("unknown", "MQTT credentials are not configured")
    try:
        context = ssl.create_default_context(cafile=settings.mqtt_ca_file)
    except (OSError, ssl.SSLError) as exc:
        return finish(
            "unknown",
            f"MQTT CA trust is unavailable: {type(exc).__name__}",
        )

    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"dbmap-operations-core-{uuid4().hex[:12]}",
        clean_session=True,
        protocol=mqtt.MQTTv311,
        reconnect_on_failure=False,
    )
    client.username_pw_set(settings.mqtt_username, settings.mqtt_password)
    client.tls_set_context(context)
    result: tuple[str, str | None] | None = None

    def on_connect(_client, _userdata, _flags, reason_code, _properties):
        nonlocal result
        connack_code = getattr(reason_code, "value", None)
        diagnostics["connack_code"] = (
            connack_code if isinstance(connack_code, int) else None
        )
        diagnostics["connack_name"] = reason_code.getName()
        result = (
            ("ok", None)
            if not reason_code.is_failure
            else (
                "offline",
                f"MQTT CONNACK rejected: {reason_code.getName()}",
            )
        )

    client.on_connect = on_connect
    try:
        if (
            client.connect(
                settings.mqtt_host,
                settings.mqtt_port,
                keepalive=30,
            )
            != mqtt.MQTT_ERR_SUCCESS
        ):
            return finish("offline", "MQTT CONNECT could not be sent")
        import time

        end = time.monotonic() + settings.probe_timeout_s
        while result is None:
            remaining = end - time.monotonic()
            if remaining <= 0:
                return finish("offline", "MQTT CONNACK timed out")
            if client.loop(timeout=remaining) != mqtt.MQTT_ERR_SUCCESS:
                return finish("offline", "MQTT connection closed before CONNACK")
        return finish(*result)
    except (OSError, ssl.SSLError) as exc:
        return finish("offline", f"{type(exc).__name__}: {str(exc)[:160]}")
    finally:
        sock = client.socket()
        try:
            client.disconnect()
        finally:
            if sock is not None:
                sock.close()


def probe_mqtt_broker() -> tuple[str, str | None]:
    state, reason, _diagnostics = probe_mqtt_broker_diagnostics()
    return state, reason


def probe_public_api() -> tuple[str, str | None]:
    if not settings.public_hub_url:
        return "unknown", "Public Hub URL is not configured"
    parsed = urlsplit(settings.public_hub_url)
    if parsed.scheme != "https" or not parsed.hostname:
        return "unknown", "Invalid public Hub URL"
    try:
        port = parsed.port or 443
        context = ssl.create_default_context(cafile=settings.opa_ca_file)
        with (
            socket.create_connection(
                (parsed.hostname, port),
                timeout=settings.probe_timeout_s,
            ) as raw,
            context.wrap_socket(raw, server_hostname=parsed.hostname) as tls,
        ):
            tls.settimeout(settings.probe_timeout_s)
            request = (
                f"GET {parsed.path.rstrip('/')}/health HTTP/1.1\r\n"
                f"Host: {parsed.hostname}\r\n"
                "Connection: close\r\n\r\n"
            ).encode()
            tls.sendall(request)
            payload = b""
            while b"\r\n\r\n" not in payload and len(payload) < 8192:
                block = tls.recv(8192)
                if not block:
                    break
                payload += block
            head, separator, body = payload.partition(b"\r\n\r\n")
            if not separator:
                return "offline", "Public API returned invalid HTTP response"
        line = head.split(b"\r\n", 1)[0]
        parts = line.split()
        if len(parts) < 2:
            return "offline", "Public API returned invalid HTTP status"
        status = int(parts[1])
        if status != 200:
            return "offline", f"Public API returned HTTP {status}"
        payload = json.loads(body)
        if (
            isinstance(payload, dict)
            and payload.get("status") == "ok"
            and payload.get("service") == "hub-bootstrap"
        ):
            return "ok", None
        return "offline", "Public API did not return Hub health JSON"
    except (
        OSError,
        http.client.HTTPException,
        ssl.SSLError,
        json.JSONDecodeError,
    ) as exc:
        return "offline", f"{type(exc).__name__}: {str(exc)[:160]}"


def probe_public_mqtt() -> tuple[str, str | None]:
    if not settings.public_mqtt_use_tls:
        return "unknown", "Public MQTT TLS is not enabled"
    if not settings.public_mqtt_host or not settings.mqtt_password:
        return "unknown", "Public MQTT endpoint or credentials are not configured"
    try:
        context = ssl.create_default_context(cafile=settings.mqtt_ca_file)
    except (OSError, ssl.SSLError) as exc:
        return "unknown", f"Public MQTT CA trust is unavailable: {type(exc).__name__}"

    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"dbmap-operations-public-{uuid4().hex[:12]}",
        clean_session=True,
        protocol=mqtt.MQTTv311,
        reconnect_on_failure=False,
    )
    client.username_pw_set(settings.mqtt_username, settings.mqtt_password)
    client.tls_set_context(context)
    result: tuple[str, str | None] | None = None

    def on_connect(_client, _userdata, _flags, reason_code, _properties):
        nonlocal result
        result = (
            ("ok", None)
            if not reason_code.is_failure
            else ("offline", f"MQTT CONNACK rejected: {reason_code.getName()}")
        )

    client.on_connect = on_connect
    import time

    try:
        if (
            client.connect(
                settings.public_mqtt_host,
                settings.public_mqtt_port,
                keepalive=30,
            )
            != mqtt.MQTT_ERR_SUCCESS
        ):
            return "offline", "Public MQTT CONNECT could not be sent"
        end = time.monotonic() + settings.probe_timeout_s
        while result is None:
            remaining = end - time.monotonic()
            if remaining <= 0:
                return "offline", "Public MQTT CONNACK timed out"
            if client.loop(timeout=remaining) != mqtt.MQTT_ERR_SUCCESS:
                return "offline", "Public MQTT connection closed before CONNACK"
        return result
    except (OSError, ssl.SSLError) as exc:
        return "offline", f"{type(exc).__name__}: {str(exc)[:160]}"
    finally:
        sock = client.socket()
        try:
            client.disconnect()
        finally:
            if sock is not None:
                sock.close()
