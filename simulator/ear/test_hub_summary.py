from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

spec = spec_from_file_location(
    "dbmap_hub_summary",
    Path(__file__).with_name("hub_summary.py"),
)
assert spec is not None and spec.loader is not None
hub_summary = module_from_spec(spec)
spec.loader.exec_module(hub_summary)


def test_summary_counts_nodes_and_simulated_vehicle_observations() -> None:
    result = hub_summary.summarize(
        {
            "nodes": [
                {"node_id": "SIM-EAR-001", "node_type": "ear", "availability": "OPERATIONAL"},
                {"node_id": "OUT-001", "node_type": "output", "availability": "SERVICE"},
            ],
            "observations": [
                {
                    "node_id": "SIM-EAR-001",
                    "received_time_utc": "2026-10-05T15:00:00Z",
                    "classification_hints": {"simulated_source_id": "SIM-VEHICLE-001"},
                },
                {
                    "node_id": "SIM-EAR-001",
                    "received_time_utc": "2026-10-05T15:00:15Z",
                    "classification_hints": {"simulated_source_id": "SIM-VEHICLE-001"},
                },
            ],
        }
    )

    assert result["node_count"] == 2
    assert result["nodes_by_type"] == {"ear": 1, "output": 1}
    assert result["observation_count"] == 2
    assert result["observations_by_node"] == {"SIM-EAR-001": 2}
    assert result["observations_by_simulated_source"] == {"SIM-VEHICLE-001": 2}
    assert result["latest_received_time_utc"] == "2026-10-05T15:00:15Z"


def test_snapshot_diff_reports_node_state_and_new_observations() -> None:
    previous = {
        "nodes": [
            {
                "node_id": "SIM-EAR-001",
                "node_type": "ear",
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
                "availability": "OPERATIONAL",
                "reported": {},
            },
            {
                "node_id": "SIM-EAR-002",
                "node_type": "ear",
                "availability": "SERVICE",
                "reported": {},
            },
        ],
        "observations": [
            {"observation_id": "observation-1", "node_id": "SIM-EAR-001"},
            {
                "observation_id": "observation-2",
                "node_id": "SIM-EAR-001",
                "classification_hints": {"simulated_source_id": "SIM-VEHICLE-001"},
            },
        ],
    }

    changes = hub_summary.snapshot_diff(previous, current)

    assert "Node state changed: SIM-EAR-001" in changes
    assert "New node: SIM-EAR-002 (ear)" in changes
    assert "New observations: 1 (SIM-EAR-001: +1)" in changes
    assert any("observation-2" in change and "SIM-VEHICLE-001" in change for change in changes)


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
