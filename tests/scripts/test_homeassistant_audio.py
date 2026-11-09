"""Unit tests for Home Assistant audio CLI helpers and scenario schema."""

from __future__ import annotations

import io
import json
import subprocess
import sys
import urllib.error
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from typing import Any
from urllib.error import URLError

import pytest

REPO_ROOT = Path(__file__).parents[2]
SCRIPT_DIRECTORY = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPT_DIRECTORY))

from lib.audio_scenario import (
    AudioScenarioError,
    list_scenario_files,
    load_scenario_file,
    run_scenario,
    scenario_changes_volume,
    validate_scenario_document,
)
from lib.homeassistant_client import (
    PLAY_VERIFY_ATTEMPTS,
    PLAY_VERIFY_INTERVAL_S,
    HomeAssistantClient,
    HomeAssistantConfig,
    HomeAssistantError,
    clamp_volume,
    load_homeassistant_config,
    parse_volume,
    redact_secrets,
    validate_entity_id,
    validate_playable_url,
)

SECRET_TOKEN = "super-secret-ha-token-value-xyz"


def _load_cli():
    spec = spec_from_file_location(
        "dbmap_homeassistant_cli",
        SCRIPT_DIRECTORY / "homeassistant.py",
    )
    assert spec is not None and spec.loader is not None
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _config(**overrides: Any) -> HomeAssistantConfig:
    base = {
        "base_url": "http://ha.example:8123",
        "token": SECRET_TOKEN,
        "entity_id": "media_player.nest",
        "default_volume": 0.25,
    }
    base.update(overrides)
    return HomeAssistantConfig(**base)


def test_parse_volume_bounds() -> None:
    assert parse_volume(0.0) == 0.0
    assert parse_volume(1) == 1.0
    with pytest.raises(HomeAssistantError):
        parse_volume(-0.01)
    with pytest.raises(HomeAssistantError):
        parse_volume(1.01)
    with pytest.raises(HomeAssistantError):
        parse_volume(True)  # type: ignore[arg-type]


def test_validate_entity_and_url() -> None:
    assert validate_entity_id("media_player.nest_mini") == "media_player.nest_mini"
    with pytest.raises(HomeAssistantError):
        validate_entity_id("light.kitchen")
    assert (
        validate_playable_url("https://files.example/test.wav")
        == "https://files.example/test.wav"
    )
    with pytest.raises(HomeAssistantError, match="http"):
        validate_playable_url("/tmp/test.wav")
    with pytest.raises(HomeAssistantError, match="http"):
        validate_playable_url("file:///tmp/test.wav")


def test_load_config_prefers_explicit_values(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("HOMEASSISTANT_URL", "http://env.example:8123")
    monkeypatch.setenv("HOMEASSISTANT_TOKEN", "token-from-env")
    monkeypatch.setenv("HOMEASSISTANT_MEDIA_PLAYER", "media_player.from_env")
    monkeypatch.setenv("HOMEASSISTANT_VOLUME", "0.4")
    cfg = load_homeassistant_config(
        url="http://cli.example:8123",
        entity_id="media_player.from_cli",
    )
    assert cfg.base_url == "http://cli.example:8123"
    assert cfg.entity_id == "media_player.from_cli"
    assert cfg.default_volume == pytest.approx(0.4)


def test_example_scenario_validates() -> None:
    path = REPO_ROOT / "tests" / "audio" / "microphone-volume-comparison.json"
    scenario = load_scenario_file(path)
    assert scenario["schema_version"] == 1
    assert scenario["name"] == "microphone-volume-comparison"
    assert len(scenario["steps"]) == 5
    assert "test_signal" in scenario["media_sources"]


def test_scenario_rejects_unknown_action_and_bare_source() -> None:
    base: dict[str, Any] = {
        "schema_version": 1,
        "name": "bad",
        "media_sources": {
            "tone": {"url": "https://example.com/a.wav"},
        },
        "steps": [{"action": "shell", "command": "rm -rf /"}],
    }
    with pytest.raises(AudioScenarioError, match="action"):
        validate_scenario_document(base)

    base["steps"] = [{"action": "play", "source": "missing.wav", "volume": 0.2}]
    with pytest.raises(AudioScenarioError, match="media_sources"):
        validate_scenario_document(base)

    base["steps"] = [{"action": "play", "source": "tone", "volume": 2.0}]
    with pytest.raises(AudioScenarioError, match="volume"):
        validate_scenario_document(base)


def test_scenario_accepts_pause_and_resume() -> None:
    doc = {
        "schema_version": 1,
        "name": "pause-resume",
        "media_sources": {"tone": {"url": "https://example.com/a.wav"}},
        "steps": [
            {"action": "play", "source": "tone", "volume": 0.2},
            {"action": "pause"},
            {"action": "resume"},
            {"action": "stop"},
        ],
    }
    scenario = validate_scenario_document(doc)
    assert [step["action"] for step in scenario["steps"]] == [
        "play",
        "pause",
        "resume",
        "stop",
    ]


def test_scenario_list_finds_example() -> None:
    files = list_scenario_files()
    assert any(path.name == "microphone-volume-comparison.json" for path in files)


def test_cli_without_arguments_prints_help() -> None:
    result = subprocess.run(
        [sys.executable, str(SCRIPT_DIRECTORY / "homeassistant.py")],
        capture_output=True,
        check=False,
        text=True,
    )
    assert result.returncode == 0
    assert result.stdout.startswith("usage:")
    assert "audio" in result.stdout
    assert "ping" in result.stdout
    assert "status" in result.stdout


def test_audio_help_includes_list_and_status() -> None:
    result = subprocess.run(
        [sys.executable, str(SCRIPT_DIRECTORY / "homeassistant.py"), "audio", "-h"],
        capture_output=True,
        check=False,
        text=True,
    )
    assert result.returncode == 0
    assert "list" in result.stdout
    assert "status" in result.stdout
    assert "{list,status" in result.stdout or "list" in result.stdout


def test_cli_volume_set_and_play_use_client(monkeypatch: pytest.MonkeyPatch) -> None:
    cli = _load_cli()
    calls: list[tuple[str, float | None, str | None]] = []

    class FakeClient:
        def __init__(self, config: HomeAssistantConfig, **_kwargs: Any) -> None:
            self.config = config

        def get_volume_level(self, entity_id: str | None = None) -> float:
            return 0.20

        def set_volume_level(
            self, volume: float, entity_id: str | None = None
        ) -> float:
            calls.append(("set", volume, entity_id))
            return clamp_volume(volume)

        def play_media(self, url: str, **kwargs: Any) -> dict[str, Any]:
            calls.append(("play", kwargs.get("volume"), kwargs.get("entity_id")))
            return {
                "entity_id": kwargs.get("entity_id"),
                "media_content_id": url,
                "media_content_type": kwargs.get("media_content_type"),
                "volume_level": kwargs.get("volume"),
                "accepted": True,
                "playback_complete": False,
                "note": "test",
            }

    monkeypatch.setattr(cli, "HomeAssistantClient", FakeClient)
    monkeypatch.setattr(
        cli,
        "resolve_config",
        lambda _args, **_kwargs: _config(default_volume=0.3),
    )

    parser = cli.build_parser()
    vol_args = parser.parse_args(
        ["audio", "volume", "--set", "0.25", "--entity", "media_player.nest"]
    )
    assert cli.cmd_audio_volume(vol_args) == 0
    assert calls[-1] == ("set", 0.25, "media_player.nest")

    play_args = parser.parse_args(
        [
            "audio",
            "play",
            "--url",
            "https://example.com/a.wav",
            "--volume",
            "0.5",
        ]
    )
    assert cli.cmd_audio_play(play_args) == 0
    assert calls[-1][0] == "play"
    assert calls[-1][1] == pytest.approx(0.5)


def test_cli_play_uses_env_default_volume_when_flag_omitted(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    cli = _load_cli()
    seen: dict[str, Any] = {}

    class FakeClient:
        def __init__(self, config: HomeAssistantConfig, **_kwargs: Any) -> None:
            self.config = config

        def play_media(self, url: str, **kwargs: Any) -> dict[str, Any]:
            seen.update(kwargs)
            seen["url"] = url
            return {
                "entity_id": kwargs.get("entity_id"),
                "media_content_id": url,
                "media_content_type": "music",
                "volume_level": kwargs.get("volume"),
                "accepted": True,
                "playback_complete": False,
                "note": "test",
            }

    monkeypatch.setattr(cli, "HomeAssistantClient", FakeClient)
    monkeypatch.setattr(
        cli,
        "resolve_config",
        lambda _args, **_kwargs: _config(default_volume=0.33),
    )
    args = cli.build_parser().parse_args(
        ["audio", "play", "--url", "https://example.com/a.wav"]
    )
    assert cli.cmd_audio_play(args) == 0
    assert seen["volume"] == pytest.approx(0.33)


def test_cli_pause_resume_stop_and_entity_override(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    cli = _load_cli()
    seen: list[tuple[str, str | None]] = []

    class FakeClient:
        def __init__(self, config: HomeAssistantConfig, **_kwargs: Any) -> None:
            self.config = config

        def pause(self, entity_id: str | None = None) -> dict[str, Any]:
            seen.append(("pause", entity_id))
            return {
                "entity_id": entity_id,
                "service": "media_player.media_pause",
                "accepted": True,
                "playback_confirmed": False,
                "note": "test",
            }

        def resume(self, entity_id: str | None = None) -> dict[str, Any]:
            seen.append(("resume", entity_id))
            return {
                "entity_id": entity_id,
                "service": "media_player.media_play",
                "accepted": True,
                "playback_confirmed": False,
                "note": "test",
            }

        def stop(self, entity_id: str | None = None) -> dict[str, Any]:
            seen.append(("stop", entity_id))
            return {
                "entity_id": entity_id,
                "service": "media_player.media_stop",
                "accepted": True,
                "playback_confirmed": False,
                "note": "test",
            }

    monkeypatch.setattr(cli, "HomeAssistantClient", FakeClient)
    monkeypatch.setattr(
        cli,
        "resolve_config",
        lambda _args, **_kwargs: _config(entity_id="media_player.default"),
    )
    parser = cli.build_parser()

    assert (
        cli.cmd_audio_pause(
            parser.parse_args(["audio", "pause", "--entity", "media_player.override"])
        )
        == 0
    )
    assert (
        cli.cmd_audio_resume(parser.parse_args(["audio", "resume"])) == 0
    )
    assert cli.cmd_audio_stop(parser.parse_args(["audio", "stop"])) == 0
    assert seen == [
        ("pause", "media_player.override"),
        ("resume", "media_player.default"),
        ("stop", "media_player.default"),
    ]


def test_client_play_media_posts_expected_services(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    posts: list[tuple[str, dict[str, Any]]] = []

    def fake_request(
        self: HomeAssistantClient,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
    ) -> Any:
        assert method == "POST"
        assert payload is not None
        posts.append((path, payload))
        return []

    monkeypatch.setattr(HomeAssistantClient, "_request", fake_request)
    client = HomeAssistantClient(_config(default_volume=None))
    result = client.play_media(
        "https://example.com/a.wav",
        volume=0.25,
        media_content_type="music",
    )
    assert result["accepted"] is True
    assert result["playback_complete"] is False
    assert posts[0][0] == "/api/services/media_player/volume_set"
    assert posts[0][1]["volume_level"] == pytest.approx(0.25)
    assert posts[1][0] == "/api/services/media_player/play_media"
    assert posts[1][1]["media_content_id"] == "https://example.com/a.wav"


def test_client_pause_resume_use_expected_services(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    posts: list[tuple[str, dict[str, Any]]] = []

    def fake_request(
        self: HomeAssistantClient,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
    ) -> Any:
        posts.append((path, payload or {}))
        return []

    monkeypatch.setattr(HomeAssistantClient, "_request", fake_request)
    client = HomeAssistantClient(_config())
    assert client.pause()["service"] == "media_player.media_pause"
    assert client.resume(entity_id="media_player.other")["service"] == (
        "media_player.media_play"
    )
    assert posts[0][0] == "/api/services/media_player/media_pause"
    assert posts[0][1]["entity_id"] == "media_player.nest"
    assert posts[1][0] == "/api/services/media_player/media_play"
    assert posts[1][1]["entity_id"] == "media_player.other"


def test_ping_success(monkeypatch: pytest.MonkeyPatch) -> None:
    def fake_meta(
        self: HomeAssistantClient,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
    ) -> tuple[Any, int, float]:
        assert method == "GET"
        assert path == "/api/"
        return {"message": "API running."}, 200, 12.5

    monkeypatch.setattr(HomeAssistantClient, "_request_with_meta", fake_meta)
    result = HomeAssistantClient(_config()).ping()
    assert result.reachable is True
    assert result.authenticated is True
    assert result.http_status == 200
    assert result.elapsed_ms == pytest.approx(12.5)
    assert SECRET_TOKEN not in result.message


def test_ping_authentication_failure(monkeypatch: pytest.MonkeyPatch) -> None:
    def fake_meta(
        self: HomeAssistantClient,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
    ) -> tuple[Any, int, float]:
        error = HomeAssistantError("Home Assistant API /api/ returned HTTP 401")
        error.http_status = 401  # type: ignore[attr-defined]
        error.elapsed_ms = 8.0  # type: ignore[attr-defined]
        raise error

    monkeypatch.setattr(HomeAssistantClient, "_request_with_meta", fake_meta)
    result = HomeAssistantClient(_config()).ping()
    assert result.reachable is True
    assert result.authenticated is False
    assert result.http_status == 401
    assert "authentication failed" in result.message


def test_ping_timeout(monkeypatch: pytest.MonkeyPatch) -> None:
    def fake_meta(
        self: HomeAssistantClient,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
    ) -> tuple[Any, int, float]:
        error = HomeAssistantError("Home Assistant API /api/ timed out after 15s")
        error.http_status = None  # type: ignore[attr-defined]
        error.elapsed_ms = 15000.0  # type: ignore[attr-defined]
        raise error

    monkeypatch.setattr(HomeAssistantClient, "_request_with_meta", fake_meta)
    result = HomeAssistantClient(_config()).ping()
    assert result.reachable is False
    assert result.authenticated is None
    assert "timed out" in result.message


def test_ping_connection_failure(monkeypatch: pytest.MonkeyPatch) -> None:
    def fake_meta(
        self: HomeAssistantClient,
        method: str,
        path: str,
        payload: dict[str, Any] | None = None,
    ) -> tuple[Any, int, float]:
        error = HomeAssistantError(
            "could not reach Home Assistant at http://ha.example:8123: "
            "Connection refused"
        )
        error.http_status = None  # type: ignore[attr-defined]
        error.elapsed_ms = 3.0  # type: ignore[attr-defined]
        raise error

    monkeypatch.setattr(HomeAssistantClient, "_request_with_meta", fake_meta)
    result = HomeAssistantClient(_config()).ping()
    assert result.reachable is False
    assert result.http_status is None
    assert "could not reach" in result.message


def test_request_timeout_and_url_error_paths(monkeypatch: pytest.MonkeyPatch) -> None:
    client = HomeAssistantClient(_config(), timeout_s=1)

    class FakeTimeoutOpener:
        def __enter__(self) -> Any:
            raise TimeoutError("timed out")

        def __exit__(self, *args: object) -> None:
            return None

    monkeypatch.setattr(
        "urllib.request.urlopen", lambda *args, **kwargs: FakeTimeoutOpener()
    )
    with pytest.raises(HomeAssistantError, match="timed out"):
        client._request("GET", "/api/")

    class FakeConnOpener:
        def __enter__(self) -> Any:
            raise URLError("Connection refused")

        def __exit__(self, *args: object) -> None:
            return None

    monkeypatch.setattr(
        "urllib.request.urlopen", lambda *args, **kwargs: FakeConnOpener()
    )
    with pytest.raises(HomeAssistantError, match="could not reach"):
        client._request("GET", "/api/")


def test_status_reachable_with_player(monkeypatch: pytest.MonkeyPatch) -> None:
    client = HomeAssistantClient(_config())

    def fake_ping(self: HomeAssistantClient) -> Any:
        from lib.homeassistant_client import PingResult

        return PingResult(
            reachable=True,
            http_status=200,
            elapsed_ms=5.0,
            authenticated=True,
            message="reachable and authenticated",
        )

    def fake_state(self: HomeAssistantClient, entity_id: str | None = None) -> dict:
        return {
            "entity_id": entity_id or "media_player.nest",
            "state": "playing",
            "attributes": {"volume_level": 0.4},
        }

    monkeypatch.setattr(HomeAssistantClient, "ping", fake_ping)
    monkeypatch.setattr(HomeAssistantClient, "get_state", fake_state)
    report = client.status()
    assert report.api_reachable is True
    assert report.player_exists is True
    assert report.player_state == "playing"
    assert report.player_volume == pytest.approx(0.4)


def test_status_unreachable_api(monkeypatch: pytest.MonkeyPatch) -> None:
    client = HomeAssistantClient(_config())

    def fake_ping(self: HomeAssistantClient) -> Any:
        from lib.homeassistant_client import PingResult

        return PingResult(
            reachable=False,
            http_status=None,
            elapsed_ms=1.0,
            authenticated=None,
            message="could not reach Home Assistant",
        )

    monkeypatch.setattr(HomeAssistantClient, "ping", fake_ping)
    report = client.status()
    assert report.api_reachable is False
    assert report.player_exists is None
    assert "not checked" in report.player_message


def test_status_missing_media_player(monkeypatch: pytest.MonkeyPatch) -> None:
    client = HomeAssistantClient(_config())

    def fake_ping(self: HomeAssistantClient) -> Any:
        from lib.homeassistant_client import PingResult

        return PingResult(
            reachable=True,
            http_status=200,
            elapsed_ms=4.0,
            authenticated=True,
            message="reachable and authenticated",
        )

    def fake_state(self: HomeAssistantClient, entity_id: str | None = None) -> dict:
        error = HomeAssistantError("Home Assistant API /api/states/x returned HTTP 404")
        error.http_status = 404  # type: ignore[attr-defined]
        raise error

    monkeypatch.setattr(HomeAssistantClient, "ping", fake_ping)
    monkeypatch.setattr(HomeAssistantClient, "get_state", fake_state)
    report = client.status()
    assert report.api_reachable is True
    assert report.player_exists is False
    assert "not found" in report.player_message


class _RecordingClient:
    """Test double that records HA-facing calls and supports volume restore."""

    def __init__(self, *, initial_volume: float | None = 0.35) -> None:
        self.volume = initial_volume
        self.calls: list[tuple[str, Any]] = []

    def get_volume_level(self, entity_id: str | None = None) -> float:
        self.calls.append(("get_volume", entity_id))
        if self.volume is None:
            raise HomeAssistantError("volume unavailable")
        return float(self.volume)

    def set_volume_level(
        self, volume: float, entity_id: str | None = None
    ) -> float:
        self.calls.append(("set_volume", volume, entity_id))
        self.volume = float(volume)
        return float(volume)

    def play_media(self, url: str, **kwargs: Any) -> dict[str, Any]:
        self.calls.append(("play", url, kwargs.get("volume"), kwargs.get("entity_id")))
        if kwargs.get("volume") is not None:
            self.volume = float(kwargs["volume"])
        return {
            "entity_id": kwargs.get("entity_id"),
            "media_content_id": url,
            "media_content_type": kwargs.get("media_content_type"),
            "volume_level": kwargs.get("volume"),
            "accepted": True,
            "playback_complete": False,
            "note": "test",
        }

    def pause(self, entity_id: str | None = None) -> dict[str, Any]:
        self.calls.append(("pause", entity_id))
        return {
            "entity_id": entity_id,
            "service": "media_player.media_pause",
            "accepted": True,
            "playback_confirmed": False,
            "note": "test",
        }

    def resume(self, entity_id: str | None = None) -> dict[str, Any]:
        self.calls.append(("resume", entity_id))
        return {
            "entity_id": entity_id,
            "service": "media_player.media_play",
            "accepted": True,
            "playback_confirmed": False,
            "note": "test",
        }

    def stop(self, entity_id: str | None = None) -> dict[str, Any]:
        self.calls.append(("stop", entity_id))
        return {
            "entity_id": entity_id,
            "service": "media_player.media_stop",
            "accepted": True,
            "playback_confirmed": False,
            "note": "test",
        }


def _sample_scenario(**overrides: Any) -> dict[str, Any]:
    doc: dict[str, Any] = {
        "schema_version": 1,
        "name": "phase2-sample",
        "media_sources": {
            "tone": {"url": "https://example.com/a.wav", "media_content_type": "music"}
        },
        "steps": [
            {"action": "set_volume", "volume": 0.25},
            {"action": "play", "source": "tone", "volume": 0.40},
            {"action": "wait", "seconds": 2},
            {"action": "stop"},
        ],
    }
    doc.update(overrides)
    return validate_scenario_document(doc)


def test_scenario_run_executes_pause_resume_and_stops_on_failure() -> None:
    client = _RecordingClient()

    def resume(self: _RecordingClient, entity_id: str | None = None) -> dict[str, Any]:
        self.calls.append(("resume", entity_id))
        raise HomeAssistantError("resume failed")

    client.resume = resume.__get__(client, _RecordingClient)  # type: ignore[method-assign]

    scenario = validate_scenario_document(
        {
            "schema_version": 1,
            "name": "pause-fail",
            "media_sources": {"tone": {"url": "https://example.com/a.wav"}},
            "steps": [
                {"action": "play", "source": "tone"},
                {"action": "pause"},
                {"action": "resume"},
                {"action": "stop"},
            ],
        }
    )
    report = run_scenario(
        scenario,
        client,  # type: ignore[arg-type]
        entity_id="media_player.nest",
        sleep=lambda _s: None,
    )
    assert report.error is not None
    assert "resume" in report.error
    assert [item.action for item in report.steps] == ["play", "pause", "resume"]
    assert report.steps[-1].ok is False
    assert ("play", "https://example.com/a.wav", None, "media_player.nest") in client.calls
    assert ("pause", "media_player.nest") in client.calls
    assert ("resume", "media_player.nest") in client.calls
    # Scenario stop step must not run after failure (restore may still set_volume).
    assert not any(call[0] == "stop" for call in client.calls)


def test_scenario_dry_run_skips_ha_and_wait() -> None:
    class ExplodingClient:
        def play_media(self, *args: Any, **kwargs: Any) -> Any:
            raise AssertionError("should not call HA in dry-run")

        def get_volume_level(self, *args: Any, **kwargs: Any) -> Any:
            raise AssertionError("should not call HA in dry-run")

    scenario = validate_scenario_document(
        {
            "schema_version": 1,
            "name": "dry",
            "media_sources": {"tone": {"url": "https://example.com/a.wav"}},
            "steps": [
                {"action": "play", "source": "tone", "volume": 0.2},
                {"action": "wait", "seconds": 99},
                {"action": "pause"},
                {"action": "resume"},
                {"action": "stop"},
            ],
        }
    )
    sleeps: list[float] = []
    report = run_scenario(
        scenario,
        ExplodingClient(),  # type: ignore[arg-type]
        dry_run=True,
        sleep=sleeps.append,
    )
    assert report.error is None
    assert sleeps == []
    assert report.estimated_wait_s == pytest.approx(99.0)
    assert [item.action for item in report.steps] == [
        "play",
        "wait",
        "pause",
        "resume",
        "stop",
    ]
    assert all(item.dry_run and item.ok for item in report.steps)


def test_cli_scenario_run_dry_run(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    cli = _load_cli()
    monkeypatch.setenv("HOMEASSISTANT_MEDIA_PLAYER", "media_player.nest")
    path = REPO_ROOT / "tests" / "audio" / "microphone-volume-comparison.json"
    args = cli.build_parser().parse_args(
        ["audio", "scenario", "run", "--dry-run", str(path)]
    )
    assert cli.cmd_scenario_run(args) == 0
    out = capsys.readouterr().out
    assert "Dry-run plan" in out
    assert "media_player.nest" in out
    assert "action=play" in out
    assert "action=wait" in out
    assert "action=stop" in out
    assert "estimated_wait_s" in out


def test_phase2_successful_complete_scenario_and_ordering() -> None:
    client = _RecordingClient(initial_volume=0.35)
    scenario = _sample_scenario()
    sleeps: list[float] = []
    report = run_scenario(
        scenario,
        client,  # type: ignore[arg-type]
        entity_id="media_player.nest",
        sleep=sleeps.append,
    )
    assert report.error is None
    assert [step.action for step in report.steps] == [
        "set_volume",
        "play",
        "wait",
        "stop",
    ]
    assert sleeps == [2.0]
    assert report.play_requests_accepted == 1
    assert report.successful_steps == 4
    assert report.volume_restore.verified is True
    assert report.volume_restore.previous_volume == pytest.approx(0.35)
    assert client.volume == pytest.approx(0.35)
    # Initial read, set 0.25, play volume 0.40, restore 0.35
    assert client.calls[0][0] == "get_volume"
    assert ("set_volume", 0.25, "media_player.nest") in client.calls
    assert ("play", "https://example.com/a.wav", 0.40, "media_player.nest") in client.calls
    assert ("stop", "media_player.nest") in client.calls
    assert client.calls[-2][0] == "set_volume" and client.calls[-2][1] == pytest.approx(
        0.35
    )


def test_phase2_volume_before_playback_and_source_resolution() -> None:
    client = _RecordingClient(initial_volume=0.10)
    scenario = validate_scenario_document(
        {
            "schema_version": 1,
            "name": "source-resolution",
            "media_sources": {
                "alpha": {"url": "https://cdn.example/alpha.wav"},
                "beta": {"url": "https://cdn.example/beta.wav"},
            },
            "steps": [
                {"action": "play", "source": "beta", "volume": 0.55},
            ],
        }
    )
    report = run_scenario(
        scenario,
        client,  # type: ignore[arg-type]
        entity_id="media_player.nest",
        sleep=lambda _s: None,
    )
    assert report.error is None
    assert ("play", "https://cdn.example/beta.wav", 0.55, "media_player.nest") in (
        client.calls
    )
    play_step = report.steps[0]
    assert play_step.requested_volume == pytest.approx(0.55)
    assert play_step.ha_accepted is True


def test_phase2_dry_run_zero_ha_calls() -> None:
    client = _RecordingClient()
    report = run_scenario(
        _sample_scenario(),
        client,  # type: ignore[arg-type]
        entity_id="media_player.nest",
        dry_run=True,
        sleep=lambda _s: None,
    )
    assert report.error is None
    assert client.calls == []
    assert "https://example.com/a.wav" in report.steps[1].detail


def test_phase2_invalid_scenario_zero_ha_calls() -> None:
    client = _RecordingClient()
    with pytest.raises(AudioScenarioError):
        validate_scenario_document(
            {
                "schema_version": 1,
                "name": "bad",
                "media_sources": {},
                "steps": [{"action": "play", "source": "missing"}],
            }
        )
    assert client.calls == []


def test_phase2_failed_playback_stops_subsequent_steps() -> None:
    client = _RecordingClient(initial_volume=0.2)

    def play_media(self: _RecordingClient, url: str, **kwargs: Any) -> dict[str, Any]:
        self.calls.append(("play", url, kwargs.get("volume"), kwargs.get("entity_id")))
        raise HomeAssistantError("play_media rejected")

    client.play_media = play_media.__get__(client, _RecordingClient)  # type: ignore[method-assign]
    report = run_scenario(
        _sample_scenario(),
        client,  # type: ignore[arg-type]
        entity_id="media_player.nest",
        sleep=lambda _s: (_ for _ in ()).throw(AssertionError("must not wait")),
    )
    assert report.error is not None
    assert report.failed_step == 2
    assert [step.action for step in report.steps] == ["set_volume", "play"]
    assert report.volume_restore.verified is True
    assert client.volume == pytest.approx(0.2)


def test_phase2_failed_volume_change_stops_subsequent_steps() -> None:
    client = _RecordingClient(initial_volume=0.2)
    first_set = True

    def set_volume_level(
        self: _RecordingClient, volume: float, entity_id: str | None = None
    ) -> float:
        nonlocal first_set
        self.calls.append(("set_volume", volume, entity_id))
        if first_set:
            first_set = False
            raise HomeAssistantError("volume_set rejected")
        self.volume = float(volume)
        return float(volume)

    client.set_volume_level = set_volume_level.__get__(  # type: ignore[method-assign]
        client, _RecordingClient
    )
    report = run_scenario(
        _sample_scenario(),
        client,  # type: ignore[arg-type]
        entity_id="media_player.nest",
        sleep=lambda _s: None,
    )
    assert report.failed_step == 1
    assert report.steps[0].action == "set_volume"
    assert not any(call[0] == "play" for call in client.calls)
    # Restore still attempted after failure.
    assert any(
        call[0] == "set_volume" and call[1] == pytest.approx(0.2)
        for call in client.calls[1:]
    )


def test_phase2_ctrl_c_cleanup_stops_and_restores() -> None:
    client = _RecordingClient(initial_volume=0.44)

    def sleep_interrupt(_seconds: float) -> None:
        raise KeyboardInterrupt

    report = run_scenario(
        _sample_scenario(),
        client,  # type: ignore[arg-type]
        entity_id="media_player.nest",
        sleep=sleep_interrupt,
    )
    assert report.interrupted is True
    assert report.failed_step == 3
    assert ("stop", "media_player.nest") in client.calls
    assert report.volume_restore.verified is True
    assert client.volume == pytest.approx(0.44)


def test_phase2_missing_initial_volume_requires_no_restore() -> None:
    client = _RecordingClient(initial_volume=None)
    assert scenario_changes_volume(_sample_scenario()) is True
    with pytest.raises(AudioScenarioError, match="--no-restore"):
        run_scenario(
            _sample_scenario(),
            client,  # type: ignore[arg-type]
            entity_id="media_player.nest",
            sleep=lambda _s: None,
        )
    # Only the initial volume probe should have happened.
    assert client.calls == [("get_volume", "media_player.nest")]

    client2 = _RecordingClient(initial_volume=None)
    report = run_scenario(
        _sample_scenario(),
        client2,  # type: ignore[arg-type]
        entity_id="media_player.nest",
        restore_volume=False,
        sleep=lambda _s: None,
    )
    assert report.error is None
    assert "--no-restore" in (report.volume_restore.skipped_reason or "")


def test_phase2_cli_entity_override(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    cli = _load_cli()
    seen_entities: list[str | None] = []

    class Wrapped(_RecordingClient):
        def __init__(self, config: HomeAssistantConfig, **_kwargs: Any) -> None:
            super().__init__(initial_volume=0.3)
            self.config = config

        def play_media(self, url: str, **kwargs: Any) -> dict[str, Any]:
            seen_entities.append(kwargs.get("entity_id"))
            return super().play_media(url, **kwargs)

    monkeypatch.setattr(cli, "HomeAssistantClient", Wrapped)
    monkeypatch.setattr(
        cli,
        "resolve_config",
        lambda args, **_kwargs: _config(
            entity_id=getattr(args, "entity", None) or "media_player.default"
        ),
    )
    path = REPO_ROOT / "tests" / "audio" / "microphone-volume-comparison.json"
    monkeypatch.setattr(cli, "load_scenario_file", lambda _path: _sample_scenario())
    args = cli.build_parser().parse_args(
        [
            "audio",
            "scenario",
            "run",
            str(path),
            "--entity",
            "media_player.override",
        ]
    )
    assert cli.cmd_scenario_run(args) == 0
    out = capsys.readouterr().out
    assert "media_player.override" in out
    assert seen_entities == ["media_player.override"]


def test_list_media_players_multiple_missing_attrs_and_default_mark(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    states = [
        {
            "entity_id": "light.kitchen",
            "state": "on",
            "attributes": {"friendly_name": "Kitchen"},
        },
        {
            "entity_id": "media_player.zebra",
            "state": "playing",
            "attributes": {
                "friendly_name": "Zebra",
                "supported_features": 123,
                "volume_level": 0.4,
            },
        },
        {
            "entity_id": "media_player.alpha",
            "state": "unavailable",
            "attributes": {"friendly_name": "Alpha"},
        },
        {
            "entity_id": "media_player.beta",
            "state": "off",
            "attributes": {
                "friendly_name": "Beta",
                "supported_features": 4,
                # volume intentionally missing
            },
        },
    ]

    def fake_states(self: HomeAssistantClient) -> list[dict[str, Any]]:
        return states

    monkeypatch.setattr(HomeAssistantClient, "get_states", fake_states)
    client = HomeAssistantClient(_config(entity_id="media_player.beta"))
    players = client.list_media_players()
    assert [player.entity_id for player in players] == [
        "media_player.alpha",
        "media_player.beta",
        "media_player.zebra",
    ]
    assert players[0].volume_level is None
    assert players[0].state == "unavailable"
    assert players[1].is_default is True
    assert players[1].volume_level is None
    assert players[2].volume_level == pytest.approx(0.4)
    assert players[2].supported_features == 123


def test_list_media_players_empty(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(HomeAssistantClient, "get_states", lambda self: [])
    players = HomeAssistantClient(_config()).list_media_players()
    assert players == []
    cli = _load_cli()
    text = "\n".join(
        cli.format_media_player_list_lines([], default_entity_id="media_player.nest")
    )
    assert "No media_player entities found." in text
    assert "media_player.nest (not present)" in text


def test_inspect_media_player_and_cli_audio_status(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    def fake_state(
        self: HomeAssistantClient, entity_id: str | None = None
    ) -> dict[str, Any]:
        return {
            "entity_id": entity_id or "media_player.nest",
            "state": "playing",
            "attributes": {
                "friendly_name": "Nest",
                "volume_level": 0.22,
                "supported_features": 99,
                "media_position": 12.5,
                "media_duration": 60.0,
                "media_title": "tone",
            },
        }

    monkeypatch.setattr(HomeAssistantClient, "get_state", fake_state)
    info = HomeAssistantClient(_config()).inspect_media_player("media_player.nest")
    assert info.volume_level == pytest.approx(0.22)
    assert info.media_position == pytest.approx(12.5)
    assert info.media_title == "tone"
    assert info.is_default is True

    cli = _load_cli()

    class FakeClient:
        def __init__(self, config: HomeAssistantConfig, **_kwargs: Any) -> None:
            self.config = config

        def inspect_media_player(
            self, entity_id: str | None = None, **kwargs: Any
        ) -> Any:
            from lib.homeassistant_client import MediaPlayerInfo

            return MediaPlayerInfo(
                entity_id=entity_id or "media_player.nest",
                state="idle",
                friendly_name="Nest",
                supported_features=1,
                volume_level=None,
                media_position=None,
                media_duration=None,
                media_title=None,
                is_default=True,
                attributes={"friendly_name": "Nest"},
            )

    monkeypatch.setattr(cli, "HomeAssistantClient", FakeClient)
    monkeypatch.setattr(
        cli,
        "resolve_config",
        lambda _args, **_kwargs: _config(),
    )
    args = cli.build_parser().parse_args(["audio", "status"])
    assert cli.cmd_audio_status(args) == 0
    out = capsys.readouterr().out
    assert "entity_id: media_player.nest" in out
    assert "volume_level: n/a" in out


def test_cli_audio_list_marks_default(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    cli = _load_cli()

    class FakeClient:
        def __init__(self, config: HomeAssistantConfig, **_kwargs: Any) -> None:
            self.config = config

        def list_media_players(self, *, default_entity_id: str | None = None) -> list:
            from lib.homeassistant_client import MediaPlayerInfo

            return [
                MediaPlayerInfo(
                    entity_id="media_player.a",
                    state="off",
                    friendly_name="A",
                    supported_features=None,
                    volume_level=None,
                    media_position=None,
                    media_duration=None,
                    media_title=None,
                    is_default=default_entity_id == "media_player.a",
                    attributes={},
                ),
                MediaPlayerInfo(
                    entity_id="media_player.b",
                    state="playing",
                    friendly_name="B",
                    supported_features=8,
                    volume_level=0.5,
                    media_position=1.0,
                    media_duration=2.0,
                    media_title="x",
                    is_default=default_entity_id == "media_player.b",
                    attributes={},
                ),
            ]

    monkeypatch.setattr(cli, "HomeAssistantClient", FakeClient)
    monkeypatch.setattr(
        cli,
        "resolve_config",
        lambda _args, **_kwargs: _config(entity_id="media_player.b"),
    )
    args = cli.build_parser().parse_args(["audio", "list"])
    assert cli.cmd_audio_list(args) == 0
    out = capsys.readouterr().out
    assert "media_player.a  no" in out
    assert "media_player.b  yes" in out
    assert "Configured default: media_player.b" in out


def test_audio_status_requires_configured_or_entity(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    cli = _load_cli()
    monkeypatch.delenv("HOMEASSISTANT_MEDIA_PLAYER", raising=False)
    monkeypatch.setenv("HOMEASSISTANT_URL", "http://ha.example:8123")
    monkeypatch.setenv("HOMEASSISTANT_TOKEN", "token")
    args = cli.build_parser().parse_args(["audio", "status"])
    with pytest.raises(HomeAssistantError, match="media player not configured"):
        cli.cmd_audio_status(args)


def test_phase2_restore_after_success_and_failure() -> None:
    ok_client = _RecordingClient(initial_volume=0.12)
    ok_report = run_scenario(
        _sample_scenario(),
        ok_client,  # type: ignore[arg-type]
        entity_id="media_player.nest",
        sleep=lambda _s: None,
    )
    assert ok_report.volume_restore.verified is True
    assert ok_client.volume == pytest.approx(0.12)

    fail_client = _RecordingClient(initial_volume=0.12)

    def boom(self: _RecordingClient, url: str, **kwargs: Any) -> dict[str, Any]:
        raise HomeAssistantError("nope")

    fail_client.play_media = boom.__get__(fail_client, _RecordingClient)  # type: ignore[method-assign]
    fail_report = run_scenario(
        _sample_scenario(),
        fail_client,  # type: ignore[arg-type]
        entity_id="media_player.nest",
        sleep=lambda _s: None,
    )
    assert fail_report.error is not None
    assert fail_report.volume_restore.verified is True
    assert fail_client.volume == pytest.approx(0.12)


def _playing_state(
    *,
    media_content_id: str | None = "https://example.com/test.mp3",
    state: str = "playing",
) -> dict[str, Any]:
    attributes: dict[str, Any] = {
        "volume_level": 0.5,
        "media_position": 1.25,
        "media_duration": 30.0,
    }
    if media_content_id is not None:
        attributes["media_content_id"] = media_content_id
    return {
        "entity_id": "media_player.nest",
        "state": state,
        "attributes": attributes,
    }


def test_verify_playback_success_on_first_second_third_poll(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    url = "https://example.com/test.mp3"

    for success_at in (1, 2, 3):
        polls = {"n": 0}
        sleeps: list[float] = []

        def fake_state(
            self: HomeAssistantClient,
            entity_id: str | None = None,
            *,
            _success_at: int = success_at,
            _polls: dict[str, int] = polls,
        ) -> dict[str, Any]:
            _polls["n"] += 1
            if _polls["n"] < _success_at:
                return _playing_state(state="idle", media_content_id=None)
            return _playing_state(media_content_id=url)

        monkeypatch.setattr(HomeAssistantClient, "get_state", fake_state)
        result = HomeAssistantClient(_config()).verify_playback(
            url,
            entity_id="media_player.nest",
            sleep=sleeps.append,
        )
        assert result.result == "verified"
        assert result.attempts == success_at
        assert result.media_content_id == url
        assert result.volume_level == pytest.approx(0.5)
        assert result.media_position == pytest.approx(1.25)
        assert result.media_duration == pytest.approx(30.0)
        assert sleeps == [PLAY_VERIFY_INTERVAL_S] * (success_at - 1)


def test_verify_playback_never_playing(monkeypatch: pytest.MonkeyPatch) -> None:
    sleeps: list[float] = []

    def fake_state(
        self: HomeAssistantClient, entity_id: str | None = None
    ) -> dict[str, Any]:
        return _playing_state(state="idle", media_content_id=None)

    monkeypatch.setattr(HomeAssistantClient, "get_state", fake_state)
    result = HomeAssistantClient(_config()).verify_playback(
        "https://example.com/test.mp3",
        sleep=sleeps.append,
    )
    assert result.result == "accepted_unverified"
    assert result.attempts == PLAY_VERIFY_ATTEMPTS
    assert sleeps == [PLAY_VERIFY_INTERVAL_S] * (PLAY_VERIFY_ATTEMPTS - 1)


def test_verify_playback_wrong_media_id(monkeypatch: pytest.MonkeyPatch) -> None:
    def fake_state(
        self: HomeAssistantClient, entity_id: str | None = None
    ) -> dict[str, Any]:
        return _playing_state(media_content_id="https://example.com/other.mp3")

    monkeypatch.setattr(HomeAssistantClient, "get_state", fake_state)
    result = HomeAssistantClient(_config()).verify_playback(
        "https://example.com/test.mp3",
        sleep=lambda _s: None,
    )
    assert result.result == "verification_failed"
    assert "different media_content_id" in result.detail


def test_verify_playback_unavailable_entity(monkeypatch: pytest.MonkeyPatch) -> None:
    def fake_state(
        self: HomeAssistantClient, entity_id: str | None = None
    ) -> dict[str, Any]:
        return _playing_state(state="unavailable", media_content_id=None)

    monkeypatch.setattr(HomeAssistantClient, "get_state", fake_state)
    result = HomeAssistantClient(_config()).verify_playback(
        "https://example.com/test.mp3",
        sleep=lambda _s: None,
    )
    assert result.result == "verification_failed"
    assert result.state == "unavailable"


def test_verify_playback_missing_media_id_while_playing(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def fake_state(
        self: HomeAssistantClient, entity_id: str | None = None
    ) -> dict[str, Any]:
        return _playing_state(media_content_id=None)

    monkeypatch.setattr(HomeAssistantClient, "get_state", fake_state)
    result = HomeAssistantClient(_config()).verify_playback(
        "https://example.com/test.mp3",
        sleep=lambda _s: None,
    )
    assert result.result == "accepted_unverified"
    assert "media_content_id was not exposed" in result.detail


def test_verify_playback_status_api_errors(monkeypatch: pytest.MonkeyPatch) -> None:
    def always_error(
        self: HomeAssistantClient, entity_id: str | None = None
    ) -> dict[str, Any]:
        raise HomeAssistantError("Home Assistant API /api/states/x returned HTTP 500")

    monkeypatch.setattr(HomeAssistantClient, "get_state", always_error)
    result = HomeAssistantClient(_config()).verify_playback(
        "https://example.com/test.mp3",
        sleep=lambda _s: None,
    )
    assert result.result == "accepted_unverified"
    assert "status polling failed" in result.detail

    def missing_entity(
        self: HomeAssistantClient, entity_id: str | None = None
    ) -> dict[str, Any]:
        error = HomeAssistantError("Home Assistant API returned HTTP 404")
        error.http_status = 404  # type: ignore[attr-defined]
        raise error

    monkeypatch.setattr(HomeAssistantClient, "get_state", missing_entity)
    missing = HomeAssistantClient(_config()).verify_playback(
        "https://example.com/test.mp3",
        sleep=lambda _s: None,
    )
    assert missing.result == "verification_failed"
    assert "not found" in missing.detail


def test_cli_play_verify_optional_preserves_default(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    cli = _load_cli()
    verify_calls = {"n": 0}

    class FakeClient:
        def __init__(self, config: HomeAssistantConfig, **_kwargs: Any) -> None:
            self.config = config

        def play_media(self, url: str, **kwargs: Any) -> dict[str, Any]:
            return {
                "entity_id": "media_player.nest",
                "media_content_id": url,
                "media_content_type": "music",
                "volume_level": kwargs.get("volume"),
                "accepted": True,
                "playback_complete": False,
                "note": "test",
            }

        def verify_playback(self, *args: Any, **kwargs: Any) -> Any:
            verify_calls["n"] += 1
            raise AssertionError("verify must not run without --verify")

    monkeypatch.setattr(cli, "HomeAssistantClient", FakeClient)
    monkeypatch.setattr(
        cli,
        "resolve_config",
        lambda _args, **_kwargs: _config(),
    )
    args = cli.build_parser().parse_args(
        ["audio", "play", "--url", "https://example.com/test.mp3", "--volume", "0.5"]
    )
    assert cli.cmd_audio_play(args) == 0
    out = capsys.readouterr()
    payload = json.loads(out.out)
    assert payload["accepted"] is True
    assert "verification" not in payload
    assert verify_calls["n"] == 0
    assert "playback completion is not confirmed" in out.err


def test_cli_play_with_verify(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    cli = _load_cli()

    class FakeClient:
        def __init__(self, config: HomeAssistantConfig, **_kwargs: Any) -> None:
            self.config = config

        def play_media(self, url: str, **kwargs: Any) -> dict[str, Any]:
            return {
                "entity_id": "media_player.nest",
                "media_content_id": url,
                "media_content_type": "music",
                "volume_level": 0.5,
                "accepted": True,
                "playback_complete": False,
                "note": "test",
            }

        def verify_playback(self, requested: str, **kwargs: Any) -> Any:
            from lib.homeassistant_client import PlaybackVerification

            return PlaybackVerification(
                result="verified",
                attempts=1,
                entity_id="media_player.nest",
                requested_media_content_id=requested,
                state="playing",
                media_content_id=requested,
                volume_level=0.5,
                media_position=0.1,
                media_duration=10.0,
                detail="ok",
            )

    monkeypatch.setattr(cli, "HomeAssistantClient", FakeClient)
    monkeypatch.setattr(
        cli,
        "resolve_config",
        lambda _args, **_kwargs: _config(),
    )
    args = cli.build_parser().parse_args(
        [
            "audio",
            "play",
            "--url",
            "https://example.com/test.mp3",
            "--volume",
            "0.50",
            "--verify",
        ]
    )
    assert cli.cmd_audio_play(args) == 0
    out = capsys.readouterr()
    payload = json.loads(out.out)
    assert payload["verification"]["result"] == "verified"
    assert "verification=verified" in out.err
    assert "not acoustic" in out.err


def test_no_token_leakage_in_errors_or_ping_output(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    assert "***" in redact_secrets(f"Bearer {SECRET_TOKEN}", SECRET_TOKEN)
    assert SECRET_TOKEN not in redact_secrets(f"Bearer {SECRET_TOKEN}", SECRET_TOKEN)

    def fake_urlopen(*args: Any, **kwargs: Any) -> Any:
        raise urllib.error.HTTPError(
            url="http://ha.example:8123/api/",
            code=401,
            msg="Unauthorized",
            hdrs=None,  # type: ignore[arg-type]
            fp=io.BytesIO(f"bad token {SECRET_TOKEN}".encode()),
        )

    monkeypatch.setattr("urllib.request.urlopen", fake_urlopen)
    client = HomeAssistantClient(_config())
    with pytest.raises(HomeAssistantError) as excinfo:
        client._request("GET", "/api/")
    assert SECRET_TOKEN not in str(excinfo.value)

    cli = _load_cli()

    class FakeClient:
        def __init__(self, config: HomeAssistantConfig, **_kwargs: Any) -> None:
            self.config = config

        def ping(self) -> Any:
            from lib.homeassistant_client import PingResult

            return PingResult(
                reachable=True,
                http_status=401,
                elapsed_ms=1.0,
                authenticated=False,
                message=f"auth failed for {SECRET_TOKEN}",
            )

    monkeypatch.setattr(cli, "HomeAssistantClient", FakeClient)
    monkeypatch.setattr(
        cli,
        "resolve_config",
        lambda _args, **_kwargs: _config(),
    )
    # cmd_ping prints message before main() redaction; ensure client path redacts.
    # Here we verify main() redacts raised errors containing the token.
    monkeypatch.setenv("HOMEASSISTANT_TOKEN", SECRET_TOKEN)
    monkeypatch.setenv("HOMEASSISTANT_URL", "http://ha.example:8123")
    monkeypatch.setenv("HOMEASSISTANT_MEDIA_PLAYER", "media_player.nest")

    def boom(_args: Any) -> int:
        raise HomeAssistantError(f"failure involving {SECRET_TOKEN}")

    monkeypatch.setattr(cli, "cmd_ping", boom)
    monkeypatch.setattr(
        sys,
        "argv",
        ["homeassistant.py", "ping"],
    )
    code = cli.main()
    captured = capsys.readouterr()
    assert code == 1
    assert SECRET_TOKEN not in captured.out
    assert SECRET_TOKEN not in captured.err
    assert "***" in captured.err


def test_scenario_validate_cli_roundtrip() -> None:
    path = REPO_ROOT / "tests" / "audio" / "microphone-volume-comparison.json"
    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT_DIRECTORY / "homeassistant.py"),
            "audio",
            "scenario",
            "validate",
            str(path),
        ],
        capture_output=True,
        check=False,
        text=True,
        cwd=str(REPO_ROOT),
    )
    assert result.returncode == 0, result.stderr
    payload = json.loads(result.stdout)
    assert payload["name"] == "microphone-volume-comparison"
    assert SECRET_TOKEN not in result.stdout
    assert SECRET_TOKEN not in result.stderr
