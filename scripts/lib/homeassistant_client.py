"""Home Assistant REST helpers for controlled media-player audio tests."""

from __future__ import annotations

import json
import os
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass
from typing import Any
from urllib.parse import urlparse

ENTITY_ID_RE = re.compile(r"^media_player\.[A-Za-z0-9_]+$")
VOLUME_MIN = 0.0
VOLUME_MAX = 1.0
# Optional audio play --verify polling (not CLI-configurable).
PLAY_VERIFY_ATTEMPTS = 3
PLAY_VERIFY_INTERVAL_S = 1.0
PLAY_VERIFY_ERROR_STATES = frozenset({"unavailable", "unknown", "error"})


class HomeAssistantError(RuntimeError):
    """User-facing Home Assistant CLI / API failure."""


@dataclass(frozen=True)
class HomeAssistantConfig:
    base_url: str
    token: str
    entity_id: str | None
    default_volume: float | None


@dataclass(frozen=True)
class PingResult:
    reachable: bool
    http_status: int | None
    elapsed_ms: float
    authenticated: bool | None
    message: str


@dataclass(frozen=True)
class StatusReport:
    ha_url: str
    entity_id: str | None
    default_volume: float | None
    api_reachable: bool
    api_authenticated: bool | None
    api_http_status: int | None
    api_elapsed_ms: float | None
    api_message: str
    player_exists: bool | None
    player_state: str | None
    player_volume: float | None
    player_message: str


@dataclass(frozen=True)
class MediaPlayerInfo:
    """Read-only snapshot of one media_player entity."""

    entity_id: str
    state: str
    friendly_name: str | None
    supported_features: int | None
    volume_level: float | None
    media_position: float | None
    media_duration: float | None
    media_title: str | None
    is_default: bool
    attributes: dict[str, Any]


@dataclass(frozen=True)
class PlaybackVerification:
    """HA media-player state verification after play_media (not acoustic)."""

    result: str  # verified | accepted_unverified | verification_failed
    attempts: int
    entity_id: str
    requested_media_content_id: str
    state: str | None
    media_content_id: str | None
    volume_level: float | None
    media_position: float | None
    media_duration: float | None
    detail: str

    def as_dict(self) -> dict[str, Any]:
        return {
            "result": self.result,
            "attempts": self.attempts,
            "entity_id": self.entity_id,
            "requested_media_content_id": self.requested_media_content_id,
            "state": self.state,
            "media_content_id": self.media_content_id,
            "volume_level": self.volume_level,
            "media_position": self.media_position,
            "media_duration": self.media_duration,
            "detail": self.detail,
            "note": (
                "Verification checks Home Assistant player state only; "
                "it does not confirm acoustic playback."
            ),
        }


def clamp_volume(value: float) -> float:
    return max(VOLUME_MIN, min(VOLUME_MAX, value))


def parse_volume(raw: object, *, label: str = "volume") -> float:
    if isinstance(raw, bool) or not isinstance(raw, (int, float)):
        raise HomeAssistantError(f"{label} must be a number between 0.0 and 1.0")
    value = float(raw)
    if value < VOLUME_MIN or value > VOLUME_MAX:
        raise HomeAssistantError(f"{label} must be between 0.0 and 1.0 (got {value})")
    return value


def parse_optional_default_volume(raw: str | None) -> float | None:
    if raw is None or raw.strip() == "":
        return None
    try:
        value = float(raw)
    except ValueError as exc:
        raise HomeAssistantError(
            "HOMEASSISTANT_VOLUME must be a number between 0.0 and 1.0"
        ) from exc
    return parse_volume(value, label="HOMEASSISTANT_VOLUME")


def validate_entity_id(entity_id: str) -> str:
    value = entity_id.strip()
    if not ENTITY_ID_RE.fullmatch(value):
        raise HomeAssistantError(
            "media player entity must look like media_player.<name> "
            f"(got {entity_id!r})"
        )
    return value


def validate_playable_url(url: str) -> str:
    """Require an HA-reachable http(s) URL; reject local filesystem paths."""
    value = url.strip()
    if not value:
        raise HomeAssistantError("audio URL must not be empty")
    parsed = urlparse(value)
    if parsed.scheme in {"file", ""}:
        raise HomeAssistantError(
            "audio URL must be http(s); Nest Mini / Cast cannot use local "
            "filesystem paths. Host the file where Home Assistant and the "
            "player can fetch it (HA local media, media_source, or a LAN/HTTPS URL)."
        )
    if parsed.scheme not in {"http", "https"}:
        raise HomeAssistantError(
            f"audio URL scheme must be http or https (got {parsed.scheme!r})"
        )
    if not parsed.netloc:
        raise HomeAssistantError("audio URL must include a host")
    return value


def normalize_base_url(url: str) -> str:
    value = url.strip().rstrip("/")
    parsed = urlparse(value)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise HomeAssistantError(
            "HOMEASSISTANT_URL must be an http(s) base URL "
            "(example: http://homeassistant.local:8123)"
        )
    return value


def redact_secrets(text: str, *secrets: str | None) -> str:
    """Remove configured secrets from user-visible text."""
    redacted = text
    for secret in secrets:
        if secret and secret.strip():
            redacted = redacted.replace(secret.strip(), "***")
    return redacted


def load_homeassistant_config(
    *,
    url: str | None = None,
    token: str | None = None,
    entity_id: str | None = None,
    default_volume: float | None = None,
    require_media_player: bool = True,
    require_token: bool = True,
    require_url: bool = True,
) -> HomeAssistantConfig:
    resolved_url = url if url is not None else os.environ.get("HOMEASSISTANT_URL", "")
    resolved_token = (
        token if token is not None else os.environ.get("HOMEASSISTANT_TOKEN", "")
    )
    env_entity = os.environ.get("HOMEASSISTANT_MEDIA_PLAYER", "")
    if entity_id is not None:
        resolved_entity: str | None = entity_id
    elif env_entity.strip():
        resolved_entity = env_entity
    else:
        resolved_entity = None

    if default_volume is None:
        default_volume = parse_optional_default_volume(
            os.environ.get("HOMEASSISTANT_VOLUME")
        )

    if require_url and not resolved_url:
        raise HomeAssistantError(
            "Home Assistant URL not configured; set HOMEASSISTANT_URL or pass --ha-url"
        )
    if require_token and not resolved_token:
        raise HomeAssistantError(
            "Home Assistant token not configured; set HOMEASSISTANT_TOKEN"
        )
    if require_media_player and not resolved_entity:
        raise HomeAssistantError(
            "media player not configured; set HOMEASSISTANT_MEDIA_PLAYER "
            "or pass --entity"
        )

    return HomeAssistantConfig(
        base_url=normalize_base_url(resolved_url) if resolved_url else "",
        token=resolved_token.strip(),
        entity_id=validate_entity_id(resolved_entity) if resolved_entity else None,
        default_volume=default_volume,
    )


class HomeAssistantClient:
    def __init__(self, config: HomeAssistantConfig, *, timeout_s: float = 15.0) -> None:
        self.config = config
        self.timeout_s = timeout_s

    def _safe_error_text(self, text: str) -> str:
        return redact_secrets(text, self.config.token)

    def _request(
        self,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
    ) -> Any:
        body, _status, _elapsed = self._request_with_meta(method, path, payload)
        return body

    def _request_with_meta(
        self,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
    ) -> tuple[Any, int, float]:
        data = json.dumps(payload).encode() if payload is not None else None
        headers = {
            "Authorization": f"Bearer {self.config.token}",
            "Content-Type": "application/json",
        }
        request = urllib.request.Request(
            f"{self.config.base_url}{path}",
            data=data,
            headers=headers,
            method=method,
        )
        started = time.perf_counter()
        try:
            with urllib.request.urlopen(request, timeout=self.timeout_s) as response:
                raw = response.read()
                elapsed_ms = (time.perf_counter() - started) * 1000.0
                status = int(response.status)
                if not raw:
                    return None, status, elapsed_ms
                return json.loads(raw.decode("utf-8")), status, elapsed_ms
        except urllib.error.HTTPError as exc:
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            detail = exc.read(1024).decode("utf-8", errors="replace").strip()
            detail = " ".join(detail.split())
            detail = self._safe_error_text(detail)
            if len(detail) > 160:
                detail = detail[:159] + "…"
            suffix = f": {detail}" if detail else ""
            error = HomeAssistantError(
                f"Home Assistant API {path} returned HTTP {exc.code}{suffix}"
            )
            error.http_status = int(exc.code)  # type: ignore[attr-defined]
            error.elapsed_ms = elapsed_ms  # type: ignore[attr-defined]
            raise error from exc
        except TimeoutError as exc:
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            error = HomeAssistantError(
                f"Home Assistant API {path} timed out after {self.timeout_s:.0f}s"
            )
            error.http_status = None  # type: ignore[attr-defined]
            error.elapsed_ms = elapsed_ms  # type: ignore[attr-defined]
            raise error from exc
        except urllib.error.URLError as exc:
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            reason = self._safe_error_text(str(exc.reason))
            if "timed out" in reason.lower():
                message = (
                    f"Home Assistant API {path} timed out after {self.timeout_s:.0f}s"
                )
            else:
                message = (
                    f"could not reach Home Assistant at {self.config.base_url}: {reason}"
                )
            error = HomeAssistantError(message)
            error.http_status = None  # type: ignore[attr-defined]
            error.elapsed_ms = elapsed_ms  # type: ignore[attr-defined]
            raise error from exc
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            elapsed_ms = (time.perf_counter() - started) * 1000.0
            error = HomeAssistantError(
                f"Home Assistant returned invalid JSON for {path}"
            )
            error.http_status = None  # type: ignore[attr-defined]
            error.elapsed_ms = elapsed_ms  # type: ignore[attr-defined]
            raise error from exc

    def require_entity(self, entity_id: str | None = None) -> str:
        target = entity_id or self.config.entity_id
        if not target:
            raise HomeAssistantError(
                "media player not configured; set HOMEASSISTANT_MEDIA_PLAYER "
                "or pass --entity"
            )
        return validate_entity_id(target)

    def ping(self) -> PingResult:
        """Minimal authenticated GET /api/ — never prints the token."""
        try:
            _body, status, elapsed_ms = self._request_with_meta("GET", "/api/")
        except HomeAssistantError as exc:
            status = getattr(exc, "http_status", None)
            elapsed_ms = float(getattr(exc, "elapsed_ms", 0.0))
            if status == 401:
                return PingResult(
                    reachable=True,
                    http_status=401,
                    elapsed_ms=elapsed_ms,
                    authenticated=False,
                    message="reachable but authentication failed (HTTP 401)",
                )
            if status is not None:
                return PingResult(
                    reachable=True,
                    http_status=status,
                    elapsed_ms=elapsed_ms,
                    authenticated=False,
                    message=f"reachable but API error (HTTP {status})",
                )
            return PingResult(
                reachable=False,
                http_status=None,
                elapsed_ms=elapsed_ms,
                authenticated=None,
                message=str(exc),
            )
        return PingResult(
            reachable=True,
            http_status=status,
            elapsed_ms=elapsed_ms,
            authenticated=True,
            message="reachable and authenticated",
        )

    def status(self, entity_id: str | None = None) -> StatusReport:
        """Read-only connectivity + configured player probe (no playback changes)."""
        ping = self.ping()
        target = entity_id or self.config.entity_id
        player_exists: bool | None = None
        player_state: str | None = None
        player_volume: float | None = None
        player_message = "media player not checked (API unavailable)"

        if not ping.reachable or ping.authenticated is not True:
            if target:
                player_message = (
                    "media player not checked because Home Assistant API "
                    "is unavailable or unauthenticated"
                )
            else:
                player_message = "media player not configured"
            return StatusReport(
                ha_url=self.config.base_url,
                entity_id=target,
                default_volume=self.config.default_volume,
                api_reachable=ping.reachable,
                api_authenticated=ping.authenticated,
                api_http_status=ping.http_status,
                api_elapsed_ms=ping.elapsed_ms,
                api_message=ping.message,
                player_exists=player_exists,
                player_state=player_state,
                player_volume=player_volume,
                player_message=player_message,
            )

        if not target:
            return StatusReport(
                ha_url=self.config.base_url,
                entity_id=None,
                default_volume=self.config.default_volume,
                api_reachable=True,
                api_authenticated=True,
                api_http_status=ping.http_status,
                api_elapsed_ms=ping.elapsed_ms,
                api_message=ping.message,
                player_exists=None,
                player_state=None,
                player_volume=None,
                player_message=(
                    "media player not configured; set HOMEASSISTANT_MEDIA_PLAYER "
                    "or pass --entity"
                ),
            )

        try:
            state = self.get_state(target)
        except HomeAssistantError as exc:
            status_code = getattr(exc, "http_status", None)
            if status_code == 404:
                return StatusReport(
                    ha_url=self.config.base_url,
                    entity_id=target,
                    default_volume=self.config.default_volume,
                    api_reachable=True,
                    api_authenticated=True,
                    api_http_status=ping.http_status,
                    api_elapsed_ms=ping.elapsed_ms,
                    api_message=ping.message,
                    player_exists=False,
                    player_state=None,
                    player_volume=None,
                    player_message=f"media player entity {target} not found (HTTP 404)",
                )
            return StatusReport(
                ha_url=self.config.base_url,
                entity_id=target,
                default_volume=self.config.default_volume,
                api_reachable=True,
                api_authenticated=True,
                api_http_status=ping.http_status,
                api_elapsed_ms=ping.elapsed_ms,
                api_message=ping.message,
                player_exists=None,
                player_state=None,
                player_volume=None,
                player_message=f"media player probe failed: {exc}",
            )

        attributes = state.get("attributes")
        if isinstance(attributes, dict) and "volume_level" in attributes:
            try:
                player_volume = parse_volume(
                    attributes["volume_level"], label="volume_level"
                )
            except HomeAssistantError:
                player_volume = None
        return StatusReport(
            ha_url=self.config.base_url,
            entity_id=target,
            default_volume=self.config.default_volume,
            api_reachable=True,
            api_authenticated=True,
            api_http_status=ping.http_status,
            api_elapsed_ms=ping.elapsed_ms,
            api_message=ping.message,
            player_exists=True,
            player_state=str(state.get("state", "")),
            player_volume=player_volume,
            player_message=f"media player entity {target} present",
        )

    def get_state(self, entity_id: str | None = None) -> dict[str, Any]:
        target = self.require_entity(entity_id)
        encoded = urllib.parse.quote(target, safe="")
        result = self._request("GET", f"/api/states/{encoded}")
        if not isinstance(result, dict):
            raise HomeAssistantError(f"invalid state payload for {target}")
        return result

    def get_states(self) -> list[dict[str, Any]]:
        result = self._request("GET", "/api/states")
        if not isinstance(result, list):
            raise HomeAssistantError("Home Assistant /api/states returned invalid JSON")
        states: list[dict[str, Any]] = []
        for item in result:
            if isinstance(item, dict):
                states.append(item)
        return states

    @staticmethod
    def _optional_float(raw: object) -> float | None:
        if isinstance(raw, bool) or not isinstance(raw, (int, float)):
            return None
        return float(raw)

    @staticmethod
    def _optional_int(raw: object) -> int | None:
        if isinstance(raw, bool) or not isinstance(raw, int):
            return None
        return int(raw)

    def _media_player_info_from_state(
        self,
        state: dict[str, Any],
        *,
        default_entity_id: str | None,
    ) -> MediaPlayerInfo | None:
        entity_id = state.get("entity_id")
        if not isinstance(entity_id, str) or not entity_id.startswith("media_player."):
            return None
        attributes = state.get("attributes")
        attrs = attributes if isinstance(attributes, dict) else {}
        friendly = attrs.get("friendly_name")
        title = attrs.get("media_title")
        return MediaPlayerInfo(
            entity_id=entity_id,
            state=str(state.get("state", "")),
            friendly_name=friendly if isinstance(friendly, str) else None,
            supported_features=self._optional_int(attrs.get("supported_features")),
            volume_level=self._optional_float(attrs.get("volume_level")),
            media_position=self._optional_float(attrs.get("media_position")),
            media_duration=self._optional_float(attrs.get("media_duration")),
            media_title=title if isinstance(title, str) else None,
            is_default=bool(default_entity_id and entity_id == default_entity_id),
            attributes=dict(attrs),
        )

    def list_media_players(
        self, *, default_entity_id: str | None = None
    ) -> list[MediaPlayerInfo]:
        """Discover all media_player.* entities (sorted by entity_id)."""
        default = default_entity_id or self.config.entity_id
        players: list[MediaPlayerInfo] = []
        for state in self.get_states():
            info = self._media_player_info_from_state(state, default_entity_id=default)
            if info is not None:
                players.append(info)
        players.sort(key=lambda item: item.entity_id)
        return players

    def inspect_media_player(
        self,
        entity_id: str | None = None,
        *,
        default_entity_id: str | None = None,
    ) -> MediaPlayerInfo:
        """Inspect one media_player; requires explicit or configured entity."""
        target = self.require_entity(entity_id)
        state = self.get_state(target)
        info = self._media_player_info_from_state(
            state,
            default_entity_id=default_entity_id or self.config.entity_id,
        )
        if info is None:
            raise HomeAssistantError(
                f"{target} is not a media_player entity (got state without "
                "media_player domain)"
            )
        return info

    def call_service(
        self,
        domain: str,
        service: str,
        data: dict[str, Any],
    ) -> Any:
        return self._request(
            "POST",
            f"/api/services/{domain}/{service}",
            data,
        )

    def get_volume_level(self, entity_id: str | None = None) -> float:
        state = self.get_state(entity_id)
        attributes = state.get("attributes")
        if not isinstance(attributes, dict) or "volume_level" not in attributes:
            raise HomeAssistantError(
                "media player state has no volume_level attribute "
                "(entity may be unavailable or not support volume)"
            )
        return parse_volume(attributes["volume_level"], label="volume_level")

    def set_volume_level(
        self,
        volume: float,
        entity_id: str | None = None,
    ) -> float:
        level = parse_volume(volume)
        target = self.require_entity(entity_id)
        self.call_service(
            "media_player",
            "volume_set",
            {"entity_id": target, "volume_level": level},
        )
        return level

    def play_media(
        self,
        url: str,
        *,
        entity_id: str | None = None,
        media_content_type: str = "music",
        volume: float | None = None,
    ) -> dict[str, Any]:
        target = self.require_entity(entity_id)
        media_url = validate_playable_url(url)
        content_type = media_content_type.strip()
        if not content_type:
            raise HomeAssistantError("media_content_type must not be empty")

        applied_volume: float | None = None
        if volume is not None:
            applied_volume = self.set_volume_level(volume, entity_id=target)

        self.call_service(
            "media_player",
            "play_media",
            {
                "entity_id": target,
                "media_content_id": media_url,
                "media_content_type": content_type,
            },
        )
        return {
            "entity_id": target,
            "media_content_id": media_url,
            "media_content_type": content_type,
            "volume_level": applied_volume,
            "accepted": True,
            "playback_complete": False,
            "note": (
                "Home Assistant accepted the play_media request; "
                "this does not confirm that playback finished or that "
                "the player successfully fetched the URL."
            ),
        }

    def verify_playback(
        self,
        requested_media_content_id: str,
        *,
        entity_id: str | None = None,
        sleep: Callable[[float], None] | None = None,
    ) -> PlaybackVerification:
        """Poll media-player state after an accepted play_media (no retries of play).

        Distinguishes HA service acceptance from confirmed player state. Does not
        claim acoustic playback.
        """
        wait = sleep if sleep is not None else time.sleep
        target = self.require_entity(entity_id)
        requested = validate_playable_url(requested_media_content_id)

        last_state: str | None = None
        last_media_id: str | None = None
        last_volume: float | None = None
        last_position: float | None = None
        last_duration: float | None = None
        saw_playing_without_id = False

        for attempt in range(1, PLAY_VERIFY_ATTEMPTS + 1):
            try:
                state_payload = self.get_state(target)
            except HomeAssistantError as exc:
                status_code = getattr(exc, "http_status", None)
                if status_code == 404:
                    return PlaybackVerification(
                        result="verification_failed",
                        attempts=attempt,
                        entity_id=target,
                        requested_media_content_id=requested,
                        state=None,
                        media_content_id=None,
                        volume_level=None,
                        media_position=None,
                        media_duration=None,
                        detail=f"media player entity not found while verifying: {exc}",
                    )
                # Non-fatal status/API errors: keep polling; fall through to unverified.
                last_state = None
                if attempt < PLAY_VERIFY_ATTEMPTS:
                    wait(PLAY_VERIFY_INTERVAL_S)
                    continue
                return PlaybackVerification(
                    result="accepted_unverified",
                    attempts=attempt,
                    entity_id=target,
                    requested_media_content_id=requested,
                    state=None,
                    media_content_id=None,
                    volume_level=None,
                    media_position=None,
                    media_duration=None,
                    detail=(
                        "play_media was accepted, but status polling failed before "
                        f"playback could be confirmed ({exc})"
                    ),
                )

            last_state = str(state_payload.get("state", ""))
            attributes = state_payload.get("attributes")
            attrs = attributes if isinstance(attributes, dict) else {}
            raw_id = attrs.get("media_content_id")
            last_media_id = raw_id if isinstance(raw_id, str) and raw_id else None
            last_volume = self._optional_float(attrs.get("volume_level"))
            last_position = self._optional_float(attrs.get("media_position"))
            last_duration = self._optional_float(attrs.get("media_duration"))

            if last_state in PLAY_VERIFY_ERROR_STATES:
                return PlaybackVerification(
                    result="verification_failed",
                    attempts=attempt,
                    entity_id=target,
                    requested_media_content_id=requested,
                    state=last_state,
                    media_content_id=last_media_id,
                    volume_level=last_volume,
                    media_position=last_position,
                    media_duration=last_duration,
                    detail=(
                        f"media player reported explicit error/unavailable state "
                        f"{last_state!r}"
                    ),
                )

            if last_state == "playing":
                if last_media_id is None:
                    saw_playing_without_id = True
                elif last_media_id == requested:
                    return PlaybackVerification(
                        result="verified",
                        attempts=attempt,
                        entity_id=target,
                        requested_media_content_id=requested,
                        state=last_state,
                        media_content_id=last_media_id,
                        volume_level=last_volume,
                        media_position=last_position,
                        media_duration=last_duration,
                        detail=(
                            "player state is playing and media_content_id matches "
                            "the requested URL (HA state only; not acoustic)"
                        ),
                    )
                else:
                    return PlaybackVerification(
                        result="verification_failed",
                        attempts=attempt,
                        entity_id=target,
                        requested_media_content_id=requested,
                        state=last_state,
                        media_content_id=last_media_id,
                        volume_level=last_volume,
                        media_position=last_position,
                        media_duration=last_duration,
                        detail=(
                            "player is playing a different media_content_id than "
                            "requested"
                        ),
                    )

            if attempt < PLAY_VERIFY_ATTEMPTS:
                wait(PLAY_VERIFY_INTERVAL_S)

        if saw_playing_without_id:
            detail = (
                "player reported playing but media_content_id was not exposed; "
                "playback could not be confirmed against the requested URL"
            )
        else:
            detail = (
                "play_media was accepted, but the player did not report playing "
                f"with a matching media_content_id within {PLAY_VERIFY_ATTEMPTS} "
                f"polls ({PLAY_VERIFY_INTERVAL_S:g}s apart)"
            )
        return PlaybackVerification(
            result="accepted_unverified",
            attempts=PLAY_VERIFY_ATTEMPTS,
            entity_id=target,
            requested_media_content_id=requested,
            state=last_state,
            media_content_id=last_media_id,
            volume_level=last_volume,
            media_position=last_position,
            media_duration=last_duration,
            detail=detail,
        )

    def pause(self, entity_id: str | None = None) -> dict[str, Any]:
        target = self.require_entity(entity_id)
        self.call_service(
            "media_player",
            "media_pause",
            {"entity_id": target},
        )
        return {
            "entity_id": target,
            "service": "media_player.media_pause",
            "accepted": True,
            "playback_confirmed": False,
            "note": (
                "Home Assistant accepted media_pause; "
                "audible pause is not confirmed. "
                "Pause/resume is intended to preserve playback position."
            ),
        }

    def resume(self, entity_id: str | None = None) -> dict[str, Any]:
        target = self.require_entity(entity_id)
        self.call_service(
            "media_player",
            "media_play",
            {"entity_id": target},
        )
        return {
            "entity_id": target,
            "service": "media_player.media_play",
            "accepted": True,
            "playback_confirmed": False,
            "note": (
                "Home Assistant accepted media_play (resume); "
                "audible playback is not confirmed. "
                "Pause/resume is intended to preserve playback position."
            ),
        }

    def stop(self, entity_id: str | None = None) -> dict[str, Any]:
        target = self.require_entity(entity_id)
        self.call_service(
            "media_player",
            "media_stop",
            {"entity_id": target},
        )
        return {
            "entity_id": target,
            "service": "media_player.media_stop",
            "accepted": True,
            "playback_confirmed": False,
            "note": (
                "Home Assistant accepted media_stop; "
                "stop completion is not confirmed. "
                "Stop may discard playback position (unlike pause/resume)."
            ),
        }
