"""Grouped keepalive cycle console output."""

from __future__ import annotations

import importlib.util
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[3] / "simulator" / "ear" / "simulate_keepalive.py"
SPEC = importlib.util.spec_from_file_location("dbmap_simulate_keepalive_output", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
keepalive = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(keepalive)


def test_format_cycle_one_timestamp_and_one_entry_per_device() -> None:
    text = keepalive.format_keepalive_cycle_report(
        successes=[
            ("SIM-EAR-001", "shared/simulator/sim-ear-a.json"),
            ("SIM-EAR-002", "shared/simulator/sim-ear-b.json"),
            ("SIM-EAR-003", "shared/simulator/sim-ear-c.json"),
            ("SIM-EAR-004", "shared/simulator/sim-ear-d.json"),
            ("SIM-EAR-005", "shared/simulator/sim-ear-e.json"),
            ("SIM-EAR-006", "shared/simulator/sim-ear-f.json"),
            ("SIM-EAR-007", "shared/simulator/sim-ear-g.json"),
        ],
        failures=[],
        timestamp="2026-11-08 10:15:00",
    )
    lines = text.splitlines()
    assert lines[0] == (
        "[2026-11-08 10:15:00] Keepalive published for 7 simulator Ears:"
    )
    assert sum(1 for line in lines if line.startswith("[")) == 1
    assert len([line for line in lines if line.startswith("  - ")]) == 7
    assert "SIM-EAR-001 (shared/simulator/sim-ear-a.json)" in lines[1]
    assert "SIM-EAR-007 (shared/simulator/sim-ear-g.json)" in lines[7]
    assert "Failed" not in text


def test_format_cycle_singular_ear_word() -> None:
    text = keepalive.format_keepalive_cycle_report(
        successes=[("SIM-EAR-001", "shared/simulator/sim-ear-a.json")],
        failures=[],
        timestamp="2026-11-08 10:15:00",
    )
    assert "1 simulator Ear:" in text
    assert "Ears:" not in text


def test_format_cycle_failures_visible_under_same_timestamp() -> None:
    text = keepalive.format_keepalive_cycle_report(
        successes=[
            ("SIM-EAR-001", "shared/simulator/sim-ear-a.json"),
            ("SIM-EAR-002", "shared/simulator/sim-ear-b.json"),
        ],
        failures=[
            (
                "SIM-EAR-006",
                "shared/simulator/sim-ear-f.json",
                "timed out publishing the keepalive",
            ),
            (
                "SIM-EAR-007",
                "shared/simulator/sim-ear-g.json",
                "timed out connecting to the TLS MQTT broker",
            ),
        ],
        timestamp="2026-11-08 10:15:00",
    )
    lines = text.splitlines()
    assert lines[0].startswith("[2026-11-08 10:15:00] Keepalive published for 2")
    assert "Failed (2):" in lines
    assert (
        "  - SIM-EAR-006 (shared/simulator/sim-ear-f.json): "
        "timed out publishing the keepalive"
    ) in lines
    assert sum(1 for line in lines if line.startswith("[")) == 1


def test_format_cycle_timestamp_local_format() -> None:
    stamp = keepalive.format_cycle_timestamp(
        datetime(2026, 11, 8, 10, 15, 0, tzinfo=timezone(timedelta(hours=1)))
    )
    assert stamp == "2026-11-08 10:15:00"


def _credentials_for_path(path: Path) -> dict:
    slug = "sim-ear-001" if path.name.endswith("a.json") else "sim-ear-002"
    return {
        "node_id": slug.upper(),
        "mqtt": {
            "host": "mqtt.ear-test.invalid", "port": 8883, "use_tls": True,
            "username": slug, "password": "isolated-test-password",
        },
        "topics": {"keepalive": f"esp-ear/local/{slug}/keepalive"},
    }


def test_one_cycle_prints_single_grouped_report(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    published: list[str] = []

    def fake_publish(credentials: dict, ca_file: str, payload: dict, *, live: bool) -> None:
        assert live
        published.append(credentials["node_id"])

    monkeypatch.setattr(keepalive, "publish_keepalive", fake_publish)
    monkeypatch.setattr(
        keepalive,
        "resolve_credentials_files",
        lambda _args, **_kwargs: [
            "shared/simulator/sim-ear-a.json",
            "shared/simulator/sim-ear-b.json",
        ],
    )
    monkeypatch.setattr(
        keepalive,
        "load_credentials",
        _credentials_for_path,
    )
    monkeypatch.setattr(
        keepalive.Path,
        "is_file",
        lambda self: True if str(self).endswith("ca.crt") else Path.is_file(self),
    )
    monkeypatch.setattr(
        keepalive.sys,
        "argv",
        ["simulate_keepalive.py", "once", "--ear", "all", "--live-mqtt"],
    )
    # The isolated CA is represented by a stubbed filesystem check.
    real_is_file = Path.is_file

    def ca_ok(self: Path) -> bool:
        if self.name == "test-ca.crt":
            return True
        return real_is_file(self)

    monkeypatch.setattr(keepalive.Path, "is_file", ca_ok)

    assert keepalive.main() == 0
    out = capsys.readouterr().out
    assert published == ["SIM-EAR-001", "SIM-EAR-002"]
    assert out.count("Keepalive published for 2 simulator Ears:") == 1
    assert out.count("[") == 1 or out.count("] Keepalive") == 1
    assert "SIM-EAR-001 (shared/simulator/sim-ear-a.json)" in out
    assert "SIM-EAR-002 (shared/simulator/sim-ear-b.json)" in out
    assert "Published keepalive for" not in out


def test_cycle_with_failure_reports_and_exits_nonzero(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    def fake_publish(credentials: dict, ca_file: str, payload: dict, *, live: bool) -> None:
        assert live
        if credentials["node_id"] == "SIM-EAR-002":
            raise RuntimeError("timed out publishing the keepalive")

    monkeypatch.setattr(keepalive, "publish_keepalive", fake_publish)
    monkeypatch.setattr(
        keepalive,
        "resolve_credentials_files",
        lambda _args, **_kwargs: [
            "shared/simulator/sim-ear-a.json",
            "shared/simulator/sim-ear-b.json",
        ],
    )
    monkeypatch.setattr(
        keepalive,
        "load_credentials",
        _credentials_for_path,
    )
    real_is_file = Path.is_file

    def ca_ok(self: Path) -> bool:
        if self.name == "test-ca.crt":
            return True
        return real_is_file(self)

    monkeypatch.setattr(keepalive.Path, "is_file", ca_ok)
    monkeypatch.setattr(
        keepalive.sys,
        "argv",
        ["simulate_keepalive.py", "once", "--ear", "all", "--live-mqtt"],
    )

    assert keepalive.main() == 1
    out = capsys.readouterr().out
    assert "Keepalive published for 1 simulator Ear:" in out
    assert "Failed (1):" in out
    assert "SIM-EAR-002" in out
    assert "timed out publishing the keepalive" in out
    assert out.count("] Keepalive published") == 1


def test_minimal_watch_uses_configured_local_tls_destination(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    published: list[str] = []

    def fake_publish(
        credentials: dict, ca_file: str, payload: dict, *, live: bool
    ) -> None:
        assert live is False
        published.append(credentials["node_id"])

    def local_credentials(path: Path) -> dict:
        credentials = _credentials_for_path(path)
        credentials["mqtt"].update(
            host="local-hub.example",
            use_tls=True,
        )
        return credentials

    monkeypatch.setenv("DBMAP_LOCAL_MQTT_HOST", "local-hub.example")
    monkeypatch.setenv("DBMAP_LOCAL_MQTT_PORT", "8883")
    monkeypatch.setattr(keepalive, "publish_keepalive", fake_publish)
    monkeypatch.setattr(
        keepalive,
        "resolve_credentials_files",
        lambda _args, **_kwargs: [
            "shared/simulator/sim-ear-a.json",
            "shared/simulator/sim-ear-b.json",
        ],
    )
    monkeypatch.setattr(keepalive, "load_credentials", local_credentials)
    real_is_file = Path.is_file
    monkeypatch.setattr(
        keepalive.Path,
        "is_file",
        lambda path: True
        if str(path).endswith("shared/trust/ca.crt")
        else real_is_file(path),
    )
    monkeypatch.setattr(
        keepalive.sys,
        "argv",
        ["simulate_keepalive.py", "watch", "--ear", "all"],
    )

    def stop_watch(_seconds: float) -> None:
        raise KeyboardInterrupt

    monkeypatch.setattr(keepalive.time, "sleep", stop_watch)

    assert keepalive.main() == 0
    out = capsys.readouterr().out
    assert published == ["SIM-EAR-001", "SIM-EAR-002"]
    assert "MQTT destination: TLS local-hub.example:8883" in out
    assert "Stopped simulated keepalive." in out
