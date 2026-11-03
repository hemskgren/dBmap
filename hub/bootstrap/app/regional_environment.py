"""Regional environmental weather context (Open-Meteo), owned by the Local Hub."""

from __future__ import annotations

import json
import logging
import threading
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable
from datetime import datetime, timedelta, timezone
from queue import Empty, Queue
from typing import Any
from uuid import uuid4

from sqlalchemy import Select, select
from sqlalchemy.orm import Session

from app.config import settings
from app.db import SessionLocal
from app.models import RegionalEnvironmentalContext

log = logging.getLogger(__name__)

HOURLY_VARS = (
    "temperature_2m",
    "relative_humidity_2m",
    "wind_speed_10m",
    "wind_direction_10m",
    "wind_gusts_10m",
    "surface_pressure",
    "precipitation",
)

_worker_thread: threading.Thread | None = None
_stop = threading.Event()
_queue: Queue[datetime] = Queue()
_inflight: set[tuple[str, str, str]] = set()
_inflight_lock = threading.Lock()
_urlopen: Callable[..., Any] = urllib.request.urlopen


class WeatherProviderError(RuntimeError):
    pass


def _aware_utc(value: datetime) -> datetime:
    if value.tzinfo is None or value.utcoffset() is None:
        return value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc)


def hour_bucket_utc(event_time: datetime) -> datetime:
    aware = _aware_utc(event_time)
    return aware.replace(minute=0, second=0, microsecond=0)


def choose_source_mode(
    event_time: datetime,
    *,
    now: datetime | None = None,
    live_max_age_hours: float | None = None,
) -> str:
    now_utc = _aware_utc(now or datetime.now(timezone.utc))
    event_utc = _aware_utc(event_time)
    max_age = (
        settings.weather_live_max_age_hours
        if live_max_age_hours is None
        else live_max_age_hours
    )
    age = now_utc - event_utc
    if age <= timedelta(hours=max_age) and age >= timedelta(hours=-1):
        return "live"
    return "historical"


def _covering_query(
    region_id: str, event_time: datetime
) -> Select[tuple[RegionalEnvironmentalContext]]:
    event_utc = _aware_utc(event_time)
    return (
        select(RegionalEnvironmentalContext)
        .where(RegionalEnvironmentalContext.region_id == region_id)
        .where(RegionalEnvironmentalContext.valid_from <= event_utc)
        .where(RegionalEnvironmentalContext.valid_until > event_utc)
        .order_by(RegionalEnvironmentalContext.fetched_at.desc())
    )


def find_covering_context(
    db: Session, event_time: datetime, *, region_id: str | None = None
) -> RegionalEnvironmentalContext | None:
    region = region_id or settings.weather_region_id
    return db.scalars(_covering_query(region, event_time)).first()


def context_is_fresh(
    row: RegionalEnvironmentalContext,
    *,
    now: datetime | None = None,
    ttl_s: float | None = None,
) -> bool:
    if row.availability != "ok":
        return False
    now_utc = _aware_utc(now or datetime.now(timezone.utc))
    fetched = _aware_utc(row.fetched_at)
    ttl = settings.weather_cache_ttl_s if ttl_s is None else ttl_s
    return (now_utc - fetched) <= timedelta(seconds=ttl)


def context_to_dict(
    row: RegionalEnvironmentalContext | None,
    *,
    from_cache: bool,
    requested_at: datetime | None = None,
) -> dict[str, Any]:
    now = _aware_utc(requested_at or datetime.now(timezone.utc))
    if row is None:
        return {
            "region_id": settings.weather_region_id,
            "provider": settings.weather_provider,
            "latitude": settings.weather_region_latitude,
            "longitude": settings.weather_region_longitude,
            "availability": "unavailable",
            "from_cache": False,
            "age_s": None,
            "source_mode": None,
            "context_id": None,
            "fetched_at": None,
            "valid_from": None,
            "valid_until": None,
            "temperature_c": None,
            "relative_humidity_pct": None,
            "wind_speed_ms": None,
            "wind_direction_deg": None,
            "wind_gusts_ms": None,
            "pressure_hpa": None,
            "precipitation_mm": None,
        }
    fetched = _aware_utc(row.fetched_at)
    age_s = max(0.0, (now - fetched).total_seconds())
    availability = row.availability
    if availability == "ok" and not context_is_fresh(row, now=now):
        availability = "stale"
    return {
        "context_id": row.context_id,
        "region_id": row.region_id,
        "provider": row.provider,
        "latitude": row.latitude,
        "longitude": row.longitude,
        "fetched_at": fetched.isoformat(),
        "valid_from": _aware_utc(row.valid_from).isoformat(),
        "valid_until": _aware_utc(row.valid_until).isoformat(),
        "temperature_c": row.temperature_c,
        "relative_humidity_pct": row.relative_humidity_pct,
        "wind_speed_ms": row.wind_speed_ms,
        "wind_direction_deg": row.wind_direction_deg,
        "wind_gusts_ms": row.wind_gusts_ms,
        "pressure_hpa": row.pressure_hpa,
        "precipitation_mm": row.precipitation_mm,
        "availability": availability,
        "source_mode": row.source_mode,
        "from_cache": from_cache,
        "age_s": age_s,
    }


def _parse_hour_value(times: list[Any], values: list[Any] | None, hour: datetime) -> Any:
    if not values:
        return None
    target = hour.strftime("%Y-%m-%dT%H:00")
    for index, stamp in enumerate(times):
        if not isinstance(stamp, str):
            continue
        if (stamp.startswith(target) or stamp == hour.isoformat()) and index < len(
            values
        ):
            return values[index]
    return None


def _fetch_open_meteo_hour(
    *,
    latitude: float,
    longitude: float,
    hour: datetime,
    source_mode: str,
    timeout_s: float | None = None,
) -> dict[str, Any]:
    day = _aware_utc(hour).date().isoformat()
    base = (
        settings.weather_forecast_base_url
        if source_mode == "live"
        else settings.weather_archive_base_url
    )
    query = urllib.parse.urlencode(
        {
            "latitude": f"{latitude:.6f}",
            "longitude": f"{longitude:.6f}",
            "hourly": ",".join(HOURLY_VARS),
            "wind_speed_unit": "ms",
            "timezone": "UTC",
            "start_date": day,
            "end_date": day,
        }
    )
    url = f"{base}?{query}"
    request = urllib.request.Request(url, method="GET")
    timeout = settings.weather_timeout_s if timeout_s is None else timeout_s
    try:
        with _urlopen(request, timeout=timeout) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError) as exc:
        raise WeatherProviderError(f"Open-Meteo request failed: {exc}") from exc
    if not isinstance(payload, dict):
        raise WeatherProviderError("Open-Meteo returned a non-object payload")
    hourly = payload.get("hourly")
    if not isinstance(hourly, dict):
        raise WeatherProviderError("Open-Meteo response missing hourly block")
    times = hourly.get("time")
    if not isinstance(times, list) or not times:
        raise WeatherProviderError("Open-Meteo response missing hourly times")
    hour_utc = hour_bucket_utc(hour)
    temperature = _parse_hour_value(times, hourly.get("temperature_2m"), hour_utc)
    if temperature is None:
        raise WeatherProviderError(
            f"Open-Meteo has no hourly sample for {hour_utc.isoformat()}"
        )
    return {
        "temperature_c": temperature,
        "relative_humidity_pct": _parse_hour_value(
            times, hourly.get("relative_humidity_2m"), hour_utc
        ),
        "wind_speed_ms": _parse_hour_value(times, hourly.get("wind_speed_10m"), hour_utc),
        "wind_direction_deg": _parse_hour_value(
            times, hourly.get("wind_direction_10m"), hour_utc
        ),
        "wind_gusts_ms": _parse_hour_value(times, hourly.get("wind_gusts_10m"), hour_utc),
        "pressure_hpa": _parse_hour_value(
            times, hourly.get("surface_pressure"), hour_utc
        ),
        "precipitation_mm": _parse_hour_value(
            times, hourly.get("precipitation"), hour_utc
        ),
        "valid_from": hour_utc,
        "valid_until": hour_utc + timedelta(hours=1),
    }


def _persist_snapshot(
    db: Session,
    *,
    source_mode: str,
    values: dict[str, Any],
    availability: str = "ok",
) -> RegionalEnvironmentalContext:
    row = RegionalEnvironmentalContext(
        context_id=str(uuid4()),
        region_id=settings.weather_region_id,
        provider=settings.weather_provider,
        latitude=settings.weather_region_latitude,
        longitude=settings.weather_region_longitude,
        fetched_at=datetime.now(timezone.utc),
        valid_from=values["valid_from"],
        valid_until=values["valid_until"],
        temperature_c=values.get("temperature_c"),
        relative_humidity_pct=values.get("relative_humidity_pct"),
        wind_speed_ms=values.get("wind_speed_ms"),
        wind_direction_deg=values.get("wind_direction_deg"),
        wind_gusts_ms=values.get("wind_gusts_ms"),
        pressure_hpa=values.get("pressure_hpa"),
        precipitation_mm=values.get("precipitation_mm"),
        availability=availability,
        source_mode=source_mode,
    )
    db.add(row)
    db.commit()
    db.refresh(row)
    return row


def ensure_context(
    db: Session,
    event_time: datetime,
    *,
    force_refresh: bool = False,
    now: datetime | None = None,
) -> tuple[RegionalEnvironmentalContext | None, bool]:
    """Return covering context and whether it came from cache.

    Performs a provider fetch when needed. Safe to call from the worker thread.
    """
    if not settings.weather_enabled:
        return None, False
    now_utc = _aware_utc(now or datetime.now(timezone.utc))
    hour = hour_bucket_utc(event_time)
    source_mode = choose_source_mode(event_time, now=now_utc)
    existing = find_covering_context(db, event_time)
    if (
        existing is not None
        and not force_refresh
        and context_is_fresh(existing, now=now_utc)
    ):
        return existing, True

    key = (settings.weather_region_id, hour.isoformat(), source_mode)
    with _inflight_lock:
        if key in _inflight:
            return existing, existing is not None
        _inflight.add(key)
    try:
        # Re-check after claiming the inflight slot.
        existing = find_covering_context(db, event_time)
        if (
            existing is not None
            and not force_refresh
            and context_is_fresh(existing, now=now_utc)
        ):
            return existing, True
        try:
            values = _fetch_open_meteo_hour(
                latitude=settings.weather_region_latitude,
                longitude=settings.weather_region_longitude,
                hour=hour,
                source_mode=source_mode,
            )
        except WeatherProviderError as exc:
            log.warning(
                "weather fetch failed region=%s mode=%s hour=%s: %s",
                settings.weather_region_id,
                source_mode,
                hour.isoformat(),
                exc,
            )
            return existing, existing is not None
        row = _persist_snapshot(db, source_mode=source_mode, values=values)
        return row, False
    finally:
        with _inflight_lock:
            _inflight.discard(key)


def schedule_weather_ensure(event_time: datetime) -> None:
    """Non-blocking enqueue used after observation ingest."""
    if not settings.weather_enabled:
        return
    try:
        _queue.put_nowait(_aware_utc(event_time))
    except Exception:
        log.debug("weather schedule dropped", exc_info=True)


def get_context_for_analysis(
    db: Session,
    event_time: datetime | None = None,
    *,
    ensure: bool = False,
) -> dict[str, Any]:
    """Resolve regional weather for analysis consumers (informational only)."""
    target = _aware_utc(event_time or datetime.now(timezone.utc))
    if ensure:
        row, from_cache = ensure_context(db, target)
    else:
        row = find_covering_context(db, target)
        from_cache = row is not None
        if row is not None and not context_is_fresh(row):
            # Still return it as stale metadata; do not fabricate replacements.
            pass
    return context_to_dict(row, from_cache=from_cache)


def _worker_loop() -> None:
    while not _stop.is_set():
        try:
            event_time = _queue.get(timeout=0.5)
        except Empty:
            continue
        db = SessionLocal()
        try:
            ensure_context(db, event_time)
        except Exception:
            log.exception("weather worker failed")
            try:
                db.rollback()
            except Exception as rollback_exc:  # noqa: BLE001
                log.debug("weather rollback failed: %s", rollback_exc)
        finally:
            db.close()
            _queue.task_done()


def start_weather_worker() -> threading.Thread | None:
    global _worker_thread
    if not settings.weather_enabled:
        log.info("regional weather worker disabled")
        return None
    if _worker_thread is not None and _worker_thread.is_alive():
        return _worker_thread
    _stop.clear()
    _worker_thread = threading.Thread(
        target=_worker_loop, name="regional-weather", daemon=True
    )
    _worker_thread.start()
    log.info(
        "regional weather worker started provider=%s region=%s ttl=%ss",
        settings.weather_provider,
        settings.weather_region_id,
        settings.weather_cache_ttl_s,
    )
    return _worker_thread


def stop_weather_worker() -> None:
    _stop.set()
