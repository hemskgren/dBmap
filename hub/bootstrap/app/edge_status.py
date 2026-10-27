"""Health probes for Compose services the Hub can reach directly."""

import socket
import ssl
from concurrent.futures import ThreadPoolExecutor

from app.config import settings

_READ_LIMIT = 8192


def _public_path(suffix: str) -> str:
    base = settings.public_base_path.strip().rstrip("/")
    if base and not base.startswith("/"):
        base = f"/{base}"
    return f"{base}{suffix}" or suffix


def _content_length(headers: bytes) -> int | None:
    for line in headers.split(b"\r\n")[1:]:
        name, separator, value = line.partition(b":")
        if separator and name.lower() == b"content-length":
            try:
                return int(value.strip())
            except ValueError:
                return None
    return None


def read_http_response(read) -> bytes:
    """Read one HTTP response through the end of its body, capped at the probe limit."""
    payload = b""
    while b"\r\n\r\n" not in payload and len(payload) < _READ_LIMIT:
        block = read(_READ_LIMIT - len(payload))
        if not block:
            return payload
        payload += block
    head, separator, _rest = payload.partition(b"\r\n\r\n")
    if not separator:
        return payload
    length = _content_length(head)
    if length is None:
        while len(payload) < _READ_LIMIT:
            block = read(_READ_LIMIT - len(payload))
            if not block:
                break
            payload += block
        return payload
    target = min(len(head) + 4 + max(length, 0), _READ_LIMIT)
    while len(payload) < target:
        block = read(target - len(payload))
        if not block:
            break
        payload += block
    return payload


def https_status(host: str, port: int, path: str, server_hostname: str) -> int | None:
    """Return the HTTP status from one TLS request, or None when it cannot be completed."""
    try:
        context = ssl.create_default_context(cafile=settings.opa_ca_file)
        with (
            socket.create_connection(
                (host, port), timeout=settings.edge_probe_timeout_s
            ) as raw,
            context.wrap_socket(raw, server_hostname=server_hostname) as tls,
        ):
            tls.settimeout(settings.edge_probe_timeout_s)
            request = (
                f"GET {path} HTTP/1.1\r\nHost: {server_hostname}\r\nConnection: close\r\n\r\n"
            ).encode()
            tls.sendall(request)
            payload = read_http_response(tls.recv)
    except (OSError, TimeoutError, ssl.SSLError, ValueError, UnicodeError):
        return None
    line = payload.split(b"\r\n", 1)[0]
    parts = line.split()
    if len(parts) < 2 or not parts[0].startswith(b"HTTP/"):
        return None
    try:
        return int(parts[1])
    except ValueError:
        return None


def _nginx_status() -> int | None:
    return https_status(
        settings.nginx_probe_host,
        settings.nginx_probe_port,
        _public_path("/health"),
        settings.nginx_probe_name,
    )


def _web_status() -> str:
    code = https_status(
        settings.web_probe_host,
        settings.web_probe_port,
        "/health",
        settings.web_probe_host,
    )
    return "online" if code == 200 else "offline"


def _nginx_component_status() -> str:
    return "online" if _nginx_status() == 200 else "offline"


def _firmware_status() -> str:
    code = https_status(
        settings.firmware_probe_host,
        settings.firmware_probe_port,
        "/health",
        settings.firmware_probe_host,
    )
    if code == 200:
        return "online"
    if code is None:
        return "unavailable"
    return "offline"


def statuses() -> dict[str, str]:
    """Probe Nginx, the web container, and the optional firmware builder in parallel."""
    with ThreadPoolExecutor(max_workers=3) as pool:
        web = pool.submit(_web_status)
        nginx = pool.submit(_nginx_component_status)
        firmware = pool.submit(_firmware_status)
        return {
            "web": web.result(),
            "nginx": nginx.result(),
            "firmware-builder": firmware.result(),
        }


def detail(component: str) -> dict | None:
    if component == "nginx":
        code = _nginx_status()
        return {
            "service": "Nginx",
            "status": "online" if code == 200 else "offline",
            "listener": f"HTTPS on port {settings.nginx_probe_port}",
            "check": f"GET {_public_path('/health')} as {settings.nginx_probe_name}",
            "http_status": code,
        }
    if component == "web":
        code = https_status(
            settings.web_probe_host,
            settings.web_probe_port,
            "/health",
            settings.web_probe_host,
        )
        return {
            "service": "Web",
            "status": "online" if code == 200 else "offline",
            "listener": f"HTTPS on port {settings.web_probe_port} inside the Compose network",
            "browser_route": "HTTPS through Nginx",
            "check": f"GET https://{settings.web_probe_host}/health",
            "http_status": code,
        }
    if component == "firmware-builder":
        state = _firmware_status()
        return {
            "service": "Firmware builder",
            "status": state,
            "profile": "firmware-build",
            "check": f"GET https://{settings.firmware_probe_host}:{settings.firmware_probe_port}/health",
            "message": (
                "Optional builder is not running."
                if state == "unavailable"
                else "The builder health check answered."
                if state == "online"
                else "The builder health check failed."
            ),
        }
    return None
