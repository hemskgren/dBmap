import sys
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

import pytest

spec = spec_from_file_location(
    "dbmap_hub_summary",
    Path(__file__).parents[2] / "scripts" / "hub_summary.py",
)
assert spec is not None and spec.loader is not None
hub_summary = module_from_spec(spec)
spec.loader.exec_module(hub_summary)


def test_print_monitoring_includes_windows_and_events(capsys) -> None:
    hub_summary.print_monitoring(
        {
            "health": {
                "state": "degraded",
                "score": 70,
                "calculated_at": "2026-10-27T12:00:00+00:00",
                "components": [
                    {
                        "id": "api",
                        "name": "Hub API",
                        "state": "ok",
                        "last_success_at": "2026-10-27T12:00:00+00:00",
                    },
                    {
                        "id": "mqtt",
                        "name": "MQTT broker",
                        "state": "offline",
                        "last_success_at": None,
                    }
                ],
            },
            "availability": {
                "current_state": "unavailable",
                "label": "Core Hub availability",
                "definition": (
                    "Whether core Hub functions for device communication were "
                    "confirmed available."
                ),
                "insufficient_data": False,
                "windows": {
                    "24h": {
                        "percent": 99.1,
                        "eligible_seconds": 86400,
                        "unavailable_seconds": 120,
                        "insufficient_data": False,
                        "low_coverage": False,
                        "score_ready": True,
                    },
                    "7d": {
                        "percent": 100.0,
                        "eligible_seconds": 10800,
                        "insufficient_data": False,
                        "low_coverage": True,
                        "score_ready": False,
                    },
                    "30d": {"percent": None, "insufficient_data": True, "reason": "insufficient_observation"},
                },
                "components": {
                    "mqtt": {
                        "label": "MQTT availability",
                        "windows": {
                            "24h": {
                                "percent": 99.1,
                                "eligible_seconds": 86400,
                                "unavailable_seconds": 120,
                                "insufficient_data": False,
                                "low_coverage": False,
                                "score_ready": True,
                            }
                        },
                    },
                    "api": {
                        "label": "API availability",
                        "windows": {
                            "24h": {
                                "percent": 100.0,
                                "eligible_seconds": 86400,
                                "unavailable_seconds": 0,
                                "insufficient_data": False,
                                "low_coverage": False,
                                "score_ready": True,
                            }
                        },
                    },
                    "web": {
                        "label": "Web availability",
                        "windows": {
                            "24h": {
                                "percent": 100.0,
                                "eligible_seconds": 86400,
                                "unavailable_seconds": 0,
                                "insufficient_data": False,
                                "low_coverage": False,
                                "score_ready": True,
                            }
                        },
                    },
                },
                "gaps": [
                    {
                        "start": "2026-10-27T10:00:00+00:00",
                        "end": "2026-10-27T11:00:00+00:00",
                        "certainty": "inferred",
                    }
                ],
            },
            "last_state_change": {
                "occurred_at": "2026-10-27T12:00:00+00:00",
                "component": "mqtt",
                "event_type": "state_changed",
                "previous_state": "ok",
                "new_state": "offline",
            },
            "recent_events": [
                {
                    "occurred_at": "2026-10-27T12:00:00+00:00",
                    "component": "mqtt",
                    "event_type": "state_changed",
                    "previous_state": "ok",
                    "new_state": "offline",
                    "certainty": "confirmed",
                }
            ],
        }
    )
    output = capsys.readouterr().out
    assert "Health (current)" in output
    assert "degraded" in output
    assert "Last check:" in output
    assert "2026-10-27" in output
    assert "• Hub API: ok" in output
    assert "• MQTT broker: offline (last success: never)" in output
    assert "Core Hub availability (historical)" in output
    assert "Current state: unavailable" in output
    assert "99.1%" in output
    assert "unavailable 2m" in output
    assert "Component availability (24h)" in output
    assert "MQTT availability:" in output
    assert "API availability:" in output
    assert "Incomplete (3h observed)" in output
    assert "Insufficient data" in output
    assert "Observation gaps reported: 1" in output
    assert "mqtt state_changed" in output


def test_summary_counts_nodes_and_simulated_vehicle_observations() -> None:
    result = hub_summary.summarize(
        {
            "nodes": [
                {
                    "node_id": "SIM-EAR-001",
                    "node_type": "ear",
                    "lifecycle_state": "active",
                    "availability": "OPERATIONAL",
                },
                {
                    "node_id": "OUT-001",
                    "node_type": "output",
                    "lifecycle_state": "deactivated",
                    "availability": "SERVICE",
                },
            ],
            "observations": [
                {
                    "node_id": "SIM-EAR-001",
                    "received_time_utc": "2026-10-05T15:00:00Z",
                    "classification": {
                        "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
                    },
                },
                {
                    "node_id": "SIM-EAR-001",
                    "received_time_utc": "2026-10-05T15:00:15Z",
                    "classification": {
                        "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
                    },
                },
                {
                    "node_id": "SIM-EAR-001",
                    "received_time_utc": "2026-10-05T15:00:30Z",
                    "classification": None,
                },
            ],
        }
    )

    assert result["node_count"] == 2
    assert result["nodes_by_type"] == {"ear": 1, "output": 1}
    assert result["nodes_by_lifecycle_state"] == {"active": 1, "deactivated": 1}
    assert result["observation_count"] == 3
    assert result["observations_by_node"] == {"SIM-EAR-001": 3}
    assert result["observations_by_simulated_source"] == {
        "SIM-VEHICLE-001": 2,
        "unlabelled": 1,
    }
    assert result["latest_received_time_utc"] == "2026-10-05T15:00:30Z"


def test_snapshot_diff_reports_node_state_and_new_observations() -> None:
    previous = {
        "nodes": [
            {
                "node_id": "SIM-EAR-001",
                "node_type": "ear",
                "lifecycle_state": "active",
                "availability": "SERVICE",
                "reported": {},
            }
        ],
        "observations": [{"observation_id": "observation-1", "node_id": "SIM-EAR-001"}],
    }
    current = {
        "nodes": [
            {
                "node_id": "SIM-EAR-001",
                "node_type": "ear",
                "lifecycle_state": "active",
                "availability": "OPERATIONAL",
                "reported": {},
            },
            {
                "node_id": "SIM-EAR-002",
                "node_type": "ear",
                "lifecycle_state": "active",
                "availability": "SERVICE",
                "reported": {},
            },
        ],
        "observations": [
            {"observation_id": "observation-1", "node_id": "SIM-EAR-001"},
            {
                "observation_id": "observation-2",
                "node_id": "SIM-EAR-001",
                "classification": {
                    "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
                },
            },
            {
                "observation_id": "observation-3",
                "node_id": "SIM-EAR-001",
                "classification": None,
            },
        ],
    }

    changes = hub_summary.snapshot_diff(previous, current)

    assert "Node state changed: SIM-EAR-001" in changes
    assert "New node: SIM-EAR-002 (ear)" in changes
    assert "New observations: 2 (SIM-EAR-001: +2)" in changes
    assert any("observation-2" in change and "SIM-VEHICLE-001" in change for change in changes)
    assert any("observation-3" in change and "source=" not in change for change in changes)


def test_unchanged_snapshot_has_no_diff() -> None:
    snapshot = {
        "nodes": [{"node_id": "SIM-EAR-001", "node_type": "ear", "reported": {}}],
        "observations": [{"observation_id": "observation-1", "node_id": "SIM-EAR-001"}],
    }

    assert hub_summary.snapshot_diff(snapshot, snapshot) == []


@pytest.mark.parametrize("interval", [0, -1])
def test_watch_interval_must_be_positive(interval: float) -> None:
    with pytest.raises(ValueError):
        hub_summary.parse_watch_interval(interval)
