from __future__ import annotations

import hmac
import math
import time
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request
from fastapi.responses import JSONResponse
from sqlalchemy import column, func, inspect, select, text
from sqlalchemy.exc import SQLAlchemyError
from sqlalchemy.orm import Session

from app import core_monitor, mqtt_connection_failures, probes, public_monitor
from app.config import settings
from app.db import get_db
from app.models import OperationsMonitorEvent
from app.pagination import (
    DEFAULT_PAGE_SIZE,
    MAX_PAGE_SIZE,
    resolve_snapshot_id,
    validate_page_params,
)
from app.policy import PolicyUnavailable, evaluate
from app.probes import (
    probe_api,
    probe_nginx,
    probe_opa,
    probe_public_api,
    probe_public_mqtt,
    probe_web,
)

router = APIRouter()
_started_at = time.monotonic()


@dataclass(frozen=True)
class Principal:
    subject_id: str
    role: str


def timing_safe_eq(left: str, right: str) -> bool:
    return hmac.compare_digest(left.encode(), right.encode())


def _authenticate(authorization: str | None) -> Principal:
    if authorization:
        scheme, separator, token = authorization.partition(" ")
        if separator and scheme.lower() == "bearer":
            if settings.admin_token and timing_safe_eq(token, settings.admin_token):
                return Principal(subject_id="local-admin", role="admin")
            if settings.viewer_token and timing_safe_eq(token, settings.viewer_token):
                return Principal(subject_id="local-viewer", role="viewer")
    raise HTTPException(status_code=401, detail="valid local access token required")


def require_policy(action: str, resource_type: str):
    def dependency(
        request: Request,
        authorization: str | None = Header(default=None),
    ) -> Principal:
        principal = _authenticate(authorization)
        decision_input = {
            "subject": {
                "id": principal.subject_id,
                "role": principal.role,
                "authenticated": True,
            },
            "action": action,
            "resource": {
                "type": resource_type,
                "id": request.path_params.get("service"),
            },
            "context": {},
        }
        try:
            allowed = evaluate(decision_input)
        except PolicyUnavailable as exc:
            raise HTTPException(status_code=503, detail=str(exc)) from exc
        if not allowed:
            raise HTTPException(status_code=403, detail="access denied by policy")
        return principal

    return dependency


def _database_healthy(db: Session) -> tuple[bool, str | None]:
    try:
        db.execute(text("SELECT 1"))
        return True, None
    except SQLAlchemyError as exc:
        db.rollback()
        return False, type(exc).__name__


def _self_worker_healthy() -> tuple[bool, list[str]]:
    reasons: list[str] = []
    if settings.core_monitor_enabled:
        core = core_monitor.worker_status()
        if not core["running"]:
            reasons.append("Core monitoring worker is not running")
        if core["stale"]:
            reasons.append("Core monitoring worker has no recent successful check")
    public = public_monitor.worker_status()
    for component, worker in public.items():
        if worker["enabled"] and (not worker["running"] or worker["stale"]):
            reasons.append(f"{component} monitoring worker is not healthy")
    return not reasons, reasons


@router.get("/health")
@router.get("/api/v1/operations/health")
def operations_health(db: Session = Depends(get_db)) -> dict:
    db_ok, _db_error = _database_healthy(db)
    workers_ok, worker_errors = _self_worker_healthy()
    healthy = db_ok and workers_ok
    result = {
        "status": "ok" if healthy else "degraded",
        "service": "operations",
    }
    if not healthy:
        result["database"] = "ok" if db_ok else "unavailable"
        result["workers"] = worker_errors
    status_code = 200 if healthy else 503
    return JSONResponse(status_code=status_code, content=result)


@router.get(
    "/api/v1/operations/status",
    dependencies=[Depends(require_policy("operations.status.read", "operations"))],
)
def operations_status(db: Session = Depends(get_db)) -> dict:
    db_ok, _db_error = _database_healthy(db)
    workers_ok, worker_errors = _self_worker_healthy()
    status = core_monitor.build_status(db)
    public = public_monitor.summary(db, datetime.now(UTC))
    return {
        "hub": {
            "hub_id": settings.hub_id,
        },
        "service": {
            "service_id": "operations",
            "application": "dBmap Operations",
            "version": "0.1.0",
            "uptime_seconds": int(time.monotonic() - _started_at),
            "health": "healthy" if db_ok and workers_ok else "degraded",
            "worker_errors": worker_errors,
        },
        "database": {
            "status": "ok" if db_ok else "unavailable",
            "engine": "sqlite",
            "path_owned": "operations.db",
        },
        "monitoring": {
            "core": core_monitor.worker_status(),
            "public": public_monitor.worker_status(),
            "configuration": {
                "core_interval_seconds": settings.core_monitor_interval_s,
                "public_interval_seconds": settings.public_monitor_interval_s,
                "gap_threshold_seconds": settings.gap_threshold_s,
                "retention_days": settings.retention_days,
            },
            "coverage": status,
            "public_components": public,
        },
        "own_health": {
            "status": "healthy" if db_ok and workers_ok else "degraded",
            "monitoring_another_service_does_not_change_operations_health": True,
        },
    }


def _service_inventory(db: Session) -> list[dict]:
    core = core_monitor.build_status(db)
    states = {component["id"]: component for component in core["health"]["components"]}
    states["hub-api"] = states.get("api", {})
    db_ok, _ = _database_healthy(db)
    hub_database_state = states.get("database", {}).get("state", "unknown")
    workers_ok, _ = _self_worker_healthy()
    operations_state = "healthy" if db_ok and workers_ok else "degraded"
    services = [
        {
            "id": "operations",
            "name": "Operations",
            "kind": "service",
            "database": {
                "name": "Operations database",
                "state": "ok" if db_ok else "unavailable",
            },
        },
        {
            "id": "hub-api",
            "name": "Hub API",
            "kind": "service",
            "database": {
                "name": "Hub database",
                "state": hub_database_state,
            },
            "api_via_nginx_state": states.get("hub_api_via_nginx", {}).get(
                "state", "unknown"
            ),
        },
        {"id": "nginx", "name": "Nginx", "kind": "service"},
        {"id": "web", "name": "Web", "kind": "service"},
        {"id": "mqtt", "name": "MQTT broker", "kind": "service"},
        {"id": "opa", "name": "Policy engine", "kind": "dependency"},
    ]
    return [
        {
            **service,
            "state": (
                operations_state
                if service["id"] == "operations"
                else states.get(
                    "nginx_local" if service["id"] == "nginx" else service["id"],
                    {},
                ).get("state", "unknown")
            ),
        }
        for service in services
    ]


@router.get(
    "/api/v1/operations/services",
    dependencies=[Depends(require_policy("operations.status.read", "operations"))],
)
def list_services(
    page: int = Query(default=1, ge=1),
    page_size: int = Query(default=DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    order: str = Query(default="oldest"),
    snapshot_id: int | None = Query(default=None, ge=1),
    db: Session = Depends(get_db),
) -> dict:
    try:
        order = validate_page_params(page, page_size, order)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    if snapshot_id is not None and snapshot_id < 1:
        raise HTTPException(status_code=422, detail="snapshot_id must be >= 1")
    items = _service_inventory(db)
    if order == "newest":
        items = list(reversed(items))
    total = len(items)
    total_pages = math.ceil(total / page_size) if total else 0
    start = (page - 1) * page_size
    page_items = items[start : start + page_size]
    return {
        "services": page_items,
        "pagination": {
            "total_count": total,
            "page": page,
            "page_size": page_size,
            "total_pages": total_pages,
            "order": order,
            "snapshot_id": snapshot_id or 1,
        },
    }


@router.get(
    "/api/v1/operations/services/{service}/details",
    dependencies=[Depends(require_policy("operations.details.read", "operations"))],
)
def service_details(service: str) -> dict:
    if service == "operations":
        workers_ok, worker_errors = _self_worker_healthy()
        try:
            with SessionLocalForDetails() as db:
                db_ok, db_error = _database_healthy(db)
                database = _operations_database_details(db)
        except SQLAlchemyError as exc:
            db_ok = False
            db_error = type(exc).__name__
            database = {"service": "Operations database", "status": "unavailable"}
        runtime, runtime_error = probes.probe_runtime("operations")
        result = {
            "service": "Operations",
            "instance": "operations",
            "status": "healthy" if db_ok and workers_ok else "degraded",
            "worker_errors": worker_errors,
            "workers": {
                "core": core_monitor.worker_status(),
                "public": public_monitor.worker_status(),
            },
            "database": database,
            "database_source": "Operations",
            "runtime": runtime,
            "runtime_source": "Operations Docker runtime",
        }
        if runtime_error:
            result["runtime_error"] = runtime_error
        if db_error:
            result["database_error"] = db_error
        return result

    if service == "database":
        diagnostics, error = probes.hub_component_details("database")
        status = "ok" if diagnostics and diagnostics.get("status") == "online" else "unknown"
        result = {
            "service": "Hub database",
            "instance": "database",
            "status": status,
            "source": "hub-bootstrap database diagnostic endpoint",
            "hub_diagnostics_source": "hub-bootstrap",
        }
        if diagnostics is not None:
            result["hub_diagnostics"] = diagnostics
        if error:
            result["hub_diagnostics_error"] = error
        return result

    probes_by_service = {
        "hub-api": probe_api,
        "nginx": probes.probe_nginx_local,
        "web": probe_web,
        "opa": probe_opa,
        "public-api": probe_public_api,
        "public-mqtt": probe_public_mqtt,
    }
    service_names = {
        "hub-api": "Hub API",
        "nginx": "Nginx",
        "web": "Web",
        "mqtt": "MQTT broker",
        "opa": "Policy engine",
        "public-api": "Public API endpoint",
        "public-mqtt": "Public MQTT endpoint",
    }
    probe = probes_by_service.get(service)
    if probe is None and service != "mqtt":
        raise HTTPException(status_code=404, detail="unknown monitored service")
    mqtt_diagnostics = None
    if service == "mqtt":
        state, reason, mqtt_diagnostics = probes.probe_mqtt_broker_diagnostics()
    else:
        state, reason = probe()
    result = {
        "service": service_names[service],
        "instance": service,
        "status": state,
        "reason": reason,
        "source": "Operations direct probe",
    }
    base_path = settings.public_base_path.rstrip("/")
    health_checks = {
        "hub-api": f"GET https://{settings.hub_api_host}:{settings.hub_api_port}/health",
        "nginx": (
            f"GET https://{settings.nginx_probe_name}:{settings.nginx_probe_port}"
            f"{base_path}/api/v1/nginx/health"
        ),
        "web": f"GET https://{settings.web_probe_host}/health",
        "opa": (
            f"GET https://{settings.opa_host}:{settings.opa_port}"
            "/health?plugins=true"
        ),
    }
    if service in health_checks:
        result["health_check"] = health_checks[service]
        result["health_check_source"] = "Operations direct probe"
    if mqtt_diagnostics is not None:
        result["connection_diagnostics"] = mqtt_diagnostics
    hub_components = {
        "hub-api": "hub-api",
        "database": "database",
        "nginx": "nginx",
        "web": "web",
        "mqtt": "mosquitto",
        "opa": "opa",
    }
    hub_component = hub_components.get(service)
    if hub_component:
        diagnostics, error = probes.hub_component_details(hub_component)
        result["hub_diagnostics_source"] = "hub-bootstrap"
        if diagnostics is not None:
            result["hub_diagnostics"] = diagnostics
        else:
            result["hub_diagnostics_error"] = error

    if service == "nginx":
        end_to_end_state, end_to_end_reason = probe_nginx()
        result["hub_api_via_nginx"] = {
            "status": end_to_end_state,
            "reason": end_to_end_reason,
            "health_check": (
                f"GET https://{settings.nginx_probe_name}:{settings.nginx_probe_port}"
                f"{base_path}/health"
            ),
            "source": "Operations end-to-end probe through Nginx",
        }
    if service == "hub-api":
        diagnostics, error = probes.hub_component_details("database")
        result["database_source"] = "hub-bootstrap"
        result["database"] = diagnostics or {
            "service": "Hub database",
            "status": "unavailable",
        }
        if error:
            result["database_error"] = error
    if service in {"hub-api", "database", "nginx", "web", "mqtt", "opa"}:
        runtime, runtime_error = probes.probe_runtime(service)
        result["runtime"] = runtime
        result["runtime_source"] = "Operations Docker runtime"
        if runtime_error:
            result["runtime_error"] = runtime_error
    return result


def _operations_database_details(db: Session) -> dict:
    bind = db.get_bind()
    details: dict = {
        "service": "Operations database",
        "status": "online",
        "engine": bind.dialect.name,
        "driver": bind.dialect.driver,
        "tables": [],
    }
    if bind.dialect.name == "sqlite":
        details["version"] = db.execute(text("SELECT sqlite_version()")).scalar_one()
        page_count = db.execute(text("PRAGMA page_count")).scalar_one()
        page_size = db.execute(text("PRAGMA page_size")).scalar_one()
        free_pages = db.execute(text("PRAGMA freelist_count")).scalar_one()
        details["allocated_size_bytes"] = page_count * page_size
        details["free_size_bytes"] = free_pages * page_size
        database_path = bind.url.database
        if database_path and database_path != ":memory:":
            try:
                details["file_size_bytes"] = Path(database_path).stat().st_size
            except OSError:
                details["file_size_bytes"] = None

    inspector = inspect(bind)
    for table_name in sorted(inspector.get_table_names()):
        quoted_name = bind.dialect.identifier_preparer.quote(table_name)
        row_count = db.execute(text(f"SELECT COUNT(*) FROM {quoted_name}")).scalar_one()
        details["tables"].append({"name": table_name, "rows": row_count})
    return details


class SessionLocalForDetails:
    def __enter__(self):
        from app.db import SessionLocal

        self.db = SessionLocal()
        return self.db

    def __exit__(self, exc_type, exc, tb):
        self.db.close()


@router.get(
    "/api/v1/operations/services/{service}/logs",
    dependencies=[Depends(require_policy("operations.logs.read", "operations"))],
)
def service_logs(service: str) -> dict:
    try:
        result, error = probes.fetch_service_logs(service)
    except ValueError:
        raise HTTPException(status_code=404, detail="service logs are not supported") from None
    if result is None:
        raise HTTPException(
            status_code=503,
            detail=error or "Service logs are unavailable",
        )
    return {**result, "source": "Operations Docker runtime"}


@router.get(
    "/api/v1/operations/services/mosquitto/connection-failures",
    dependencies=[Depends(require_policy("operations.logs.read", "operations"))],
)
def mqtt_connection_failures_summary() -> dict:
    """Recent MQTT connection failures from the mounted Mosquitto log."""
    return mqtt_connection_failures.read_recent_failures()


@router.get(
    "/api/v1/operations/events",
    dependencies=[Depends(require_policy("operations.events.read", "operations"))],
)
def list_events(
    page: int = Query(default=1, ge=1),
    page_size: int = Query(default=DEFAULT_PAGE_SIZE, ge=1, le=MAX_PAGE_SIZE),
    order: str = Query(default="newest"),
    snapshot_id: int | None = Query(default=None, ge=1),
    start: str | None = Query(default=None, description="Inclusive UTC ISO-8601 start"),
    end: str | None = Query(default=None, description="Inclusive UTC ISO-8601 end"),
    db: Session = Depends(get_db),
) -> dict:
    try:
        order = validate_page_params(page, page_size, order)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    if snapshot_id is not None and snapshot_id < 1:
        raise HTTPException(status_code=422, detail="snapshot_id must be >= 1")

    def parse_time(value: str | None, name: str) -> datetime | None:
        if value is None:
            return None
        try:
            parsed = datetime.fromisoformat(value)
        except ValueError as exc:
            raise HTTPException(
                status_code=422, detail=f"{name} must be ISO-8601"
            ) from exc
        if parsed.tzinfo is None or parsed.utcoffset() is None:
            raise HTTPException(status_code=422, detail=f"{name} must include timezone")
        return parsed.astimezone(UTC)

    start_at = parse_time(start, "start")
    end_at = parse_time(end, "end")
    if start_at is not None and end_at is not None and start_at > end_at:
        raise HTTPException(status_code=422, detail="start must not be after end")

    current_max = db.scalar(
        select(func.max(column("rowid"))).select_from(OperationsMonitorEvent)
    )
    current_max = int(current_max) if current_max is not None else None
    effective_snapshot = resolve_snapshot_id(snapshot_id, current_max)

    query = select(OperationsMonitorEvent)
    if effective_snapshot is not None:
        query = query.where(column("rowid") <= effective_snapshot)
    if start_at is not None:
        query = query.where(OperationsMonitorEvent.occurred_at >= start_at)
    if end_at is not None:
        query = query.where(OperationsMonitorEvent.occurred_at <= end_at)

    count_query = select(func.count()).select_from(query.subquery())
    total = int(db.scalar(count_query) or 0)
    total_pages = math.ceil(total / page_size) if total else 0
    if order == "newest":
        query = query.order_by(
            OperationsMonitorEvent.occurred_at.desc(),
            OperationsMonitorEvent.event_id.desc(),
        )
    else:
        query = query.order_by(
            OperationsMonitorEvent.occurred_at.asc(),
            OperationsMonitorEvent.event_id.asc(),
        )
    offset = (page - 1) * page_size
    events = list(db.scalars(query.offset(offset).limit(page_size)))

    return {
        "events": [
            {
                "event_id": event.event_id,
                "occurred_at": event.occurred_at.isoformat(),
                "observed_at": event.observed_at.isoformat(),
                "component": event.component,
                "event_type": event.event_type,
                "previous_state": event.previous_state,
                "new_state": event.new_state,
                "reason": event.reason,
                "source": event.source,
                "certainty": event.certainty,
            }
            for event in events
        ],
        "pagination": {
            "total_count": total,
            "page": page,
            "page_size": page_size,
            "total_pages": total_pages,
            "order": order,
            "snapshot_id": effective_snapshot,
        },
        "time_range": {
            "start": start_at.isoformat() if start_at else None,
            "end": end_at.isoformat() if end_at else None,
        },
    }
