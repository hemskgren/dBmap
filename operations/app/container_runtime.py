"""Bounded Docker runtime diagnostics for allowlisted Compose services."""

import json
import logging
import os
import re
import socket
import threading
from http.client import HTTPConnection, HTTPException
from urllib.parse import quote

from app.config import settings

log = logging.getLogger("operations.container-runtime")

LOG_TAIL_LINES = 30
MAX_RESPONSE_BYTES = 1_000_000
_TARGETS = {
    "nginx": ("Nginx", "nginx"),
    "web": ("Web", "dbmap-web"),
    "hub-api": ("Hub API", "hub-bootstrap"),
    "opa": ("Policy engine", "opa"),
    "mqtt": ("MQTT broker", "mosquitto"),
    "operations": ("Operations", "operations"),
}
RUNTIME_SERVICES = tuple(_TARGETS)
_api_version: str | None = None
_api_lock = threading.Lock()


class UnknownService(LookupError):
    """Raised when a service is not allowlisted for runtime diagnostics."""


class ContainerNotFound(LookupError):
    """Raised when an allowlisted Compose service has no matching container."""


class ContainerRuntimeUnavailable(RuntimeError):
    """Raised when Docker cannot return bounded runtime diagnostics."""


class _UnixHTTPConnection(HTTPConnection):
    def __init__(self, socket_path: str, timeout: float) -> None:
        super().__init__("localhost", timeout=timeout)
        self._socket_path = socket_path

    def connect(self) -> None:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(self.timeout)
        sock.connect(self._socket_path)
        self.sock = sock


def _request(method: str, path: str) -> tuple[int, bytes]:
    if not os.path.exists(settings.docker_socket):
        raise ContainerRuntimeUnavailable(
            "Container runtime diagnostics are unavailable because Operations "
            "cannot reach the Docker engine."
        )
    connection = _UnixHTTPConnection(settings.docker_socket, timeout=5)
    try:
        connection.request(method, path)
        response = connection.getresponse()
        return response.status, response.read(MAX_RESPONSE_BYTES)
    except (OSError, TimeoutError, HTTPException) as exc:
        raise ContainerRuntimeUnavailable(
            "Container runtime diagnostics are unavailable because Operations "
            "cannot reach the Docker engine."
        ) from exc
    finally:
        connection.close()


def _api_prefix() -> str:
    global _api_version
    with _api_lock:
        if _api_version is None:
            status, body = _request("GET", "/version")
            if status != 200:
                raise ContainerRuntimeUnavailable(
                    "Container runtime diagnostics are unavailable because Operations "
                    "cannot reach the Docker engine."
                )
            try:
                version = json.loads(body)["ApiVersion"]
            except (
                UnicodeDecodeError,
                json.JSONDecodeError,
                KeyError,
                TypeError,
            ) as exc:
                raise ContainerRuntimeUnavailable(
                    "Container runtime diagnostics are unavailable because Operations "
                    "cannot reach the Docker engine."
                ) from exc
            if not isinstance(version, str) or not version.replace(".", "", 1).isdigit():
                raise ContainerRuntimeUnavailable(
                    "Container runtime diagnostics are unavailable because Operations "
                    "cannot reach the Docker engine."
                )
            _api_version = version
        return f"/v{_api_version}"


def _project_name() -> str:
    configured = settings.compose_project.strip()
    if configured:
        return configured
    status, body = _request(
        "GET", f"{_api_prefix()}/containers/{quote(socket.gethostname(), safe='')}/json"
    )
    if status == 200:
        try:
            info = json.loads(body)
            project = info["Config"]["Labels"]["com.docker.compose.project"]
        except (UnicodeDecodeError, json.JSONDecodeError, KeyError, TypeError):
            project = ""
        if isinstance(project, str) and project:
            return project
    return "dbmap"


def _resolve_container(service: str) -> tuple[str, str, str]:
    """Return display name, Compose service, and validated container ID."""
    target = _TARGETS.get(service)
    if target is None:
        raise UnknownService(service)
    service_name, compose_service = target
    filters = quote(
        json.dumps(
            {
                "label": [
                    f"com.docker.compose.service={compose_service}",
                    f"com.docker.compose.project={_project_name()}",
                ]
            },
            separators=(",", ":"),
        ),
        safe="",
    )
    status, body = _request(
        "GET", f"{_api_prefix()}/containers/json?all=1&filters={filters}"
    )
    if status != 200:
        log.warning("docker container list failed status=%s service=%s", status, service)
        raise ContainerRuntimeUnavailable(
            "Container runtime diagnostics are unavailable because Operations "
            "cannot reach the Docker engine."
        )
    try:
        containers = json.loads(body)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ContainerRuntimeUnavailable(
            "Container runtime diagnostics are unavailable because Operations "
            "cannot reach the Docker engine."
        ) from exc
    if not isinstance(containers, list) or not containers:
        raise ContainerNotFound(
            f"No {compose_service} container was found for this Compose project."
        )
    containers.sort(
        key=lambda item: item.get("Created", 0) if isinstance(item, dict) else 0,
        reverse=True,
    )
    running = [
        item
        for item in containers
        if isinstance(item, dict) and item.get("State") == "running"
    ]
    chosen = (running or containers)[0]
    container_id = chosen.get("Id") if isinstance(chosen, dict) else None
    if (
        not isinstance(container_id, str)
        or re.fullmatch(r"[0-9a-f]{12,64}", container_id) is None
    ):
        raise ContainerRuntimeUnavailable(
            "Container runtime diagnostics are unavailable because Operations "
            "cannot reach the Docker engine."
        )
    return service_name, compose_service, container_id


def decode_docker_logs(payload: bytes, line_limit: int = LOG_TAIL_LINES) -> list[str]:
    if not 1 <= line_limit <= LOG_TAIL_LINES:
        raise ValueError("line_limit must be between 1 and 30")
    if not payload:
        return []
    frames = _demux(payload)
    text = (frames if frames is not None else payload).decode("utf-8", errors="replace")
    return text.splitlines()[-line_limit:]


def _demux(payload: bytes) -> bytes | None:
    chunks: list[bytes] = []
    index = 0
    while index < len(payload):
        if index + 8 > len(payload) or payload[index] not in {0, 1, 2}:
            return None
        size = int.from_bytes(payload[index + 4 : index + 8], "big")
        index += 8
        if index + size > len(payload):
            return None
        chunks.append(payload[index : index + size])
        index += size
    return b"".join(chunks)


def read_tail(service: str) -> dict:
    service_name, compose_service, container_id = _resolve_container(service)
    log_status, log_body = _request(
        "GET",
        f"{_api_prefix()}/containers/{container_id}/logs"
        f"?stdout=1&stderr=1&tail={LOG_TAIL_LINES}",
    )
    if log_status != 200:
        log.warning(
            "docker logs failed status=%s service=%s", log_status, compose_service
        )
        raise ContainerRuntimeUnavailable(
            "Container logs are unavailable because Operations cannot reach the "
            "Docker engine."
        )
    return {
        "service": service_name,
        "compose_service": compose_service,
        "lines": decode_docker_logs(log_body),
        "line_limit": LOG_TAIL_LINES,
    }


def inspect_runtime(service: str) -> dict:
    """Expose selected runtime fields only; never return the full Docker inspect."""
    service_name, compose_service, container_id = _resolve_container(service)
    status, body = _request("GET", f"{_api_prefix()}/containers/{container_id}/json")
    if status != 200:
        raise ContainerRuntimeUnavailable(
            "Container inspection is unavailable because Operations cannot reach "
            "the Docker engine."
        )
    try:
        info = json.loads(body)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ContainerRuntimeUnavailable(
            "Container inspection is unavailable because Operations cannot reach "
            "the Docker engine."
        ) from exc
    if not isinstance(info, dict):
        raise ContainerRuntimeUnavailable(
            "Container inspection returned an invalid response."
        )
    state = info.get("State")
    if not isinstance(state, dict):
        state = {}
    health = state.get("Health")
    health_status = (
        health["Status"]
        if isinstance(health, dict) and isinstance(health.get("Status"), str)
        else None
    )
    restart_count = state.get("RestartCount")
    if not isinstance(restart_count, int):
        restart_count = info.get("RestartCount")
    if not isinstance(restart_count, int):
        restart_count = None
    started_at = state.get("StartedAt")
    if not isinstance(started_at, str):
        started_at = None
    return {
        "service": service_name,
        "compose_service": compose_service,
        "container_id": container_id[:12],
        "running": state.get("Status") == "running",
        "docker_state": state.get("Status"),
        "health_status": health_status,
        "started_at": started_at,
        "restart_count": restart_count,
    }


def inspect_monitored_runtimes() -> dict[str, dict]:
    results: dict[str, dict] = {}
    for service in RUNTIME_SERVICES:
        try:
            results[service] = inspect_runtime(service)
        except (UnknownService, ContainerNotFound, ContainerRuntimeUnavailable) as exc:
            results[service] = {
                "service": _TARGETS[service][0],
                "compose_service": _TARGETS[service][1],
                "error": str(exc),
            }
    return results
