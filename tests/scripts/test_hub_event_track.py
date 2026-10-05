from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

spec = spec_from_file_location(
    "dbmap_hub_event_track",
    Path(__file__).parents[2] / "scripts" / "hub_event_track.py",
)
assert spec is not None and spec.loader is not None
hub_event_track = module_from_spec(spec)
spec.loader.exec_module(hub_event_track)


def test_groups_only_by_simulator_hint_and_keeps_unlabelled_observations_separate() -> None:
    observations = [
        {
            "observation_id": "obs-1",
            "node_id": "SIM-EAR-001",
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
        {
            "observation_id": "obs-2",
            "node_id": "SIM-EAR-002",
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
        {
            "observation_id": "obs-3",
            "node_id": "EAR-001",
            "classification": None,
        },
    ]

    groups, ungrouped = hub_event_track.group_by_simulated_source(observations)

    assert [item["observation_id"] for item in groups["SIM-VEHICLE-001"]] == [
        "obs-1",
        "obs-2",
    ]
    assert [item["observation_id"] for item in ungrouped] == ["obs-3"]


def test_report_disclaims_event_track_and_geographic_position_inference() -> None:
    lines = hub_event_track.report(
        [
            {
                "observation_id": "obs-1",
                "node_id": "SIM-EAR-001",
                "event_time_utc": "2026-10-05T18:00:00Z",
                "classification": {
                    "source_family": "vehicle",
                    "confidence": 0.72,
                    "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
                },
                "bearing": {"deg": 180.0, "reference": "node", "confidence": 0.61},
            }
        ]
    )

    output = "\n".join(lines)
    assert "SIM-VEHICLE-001: 1 observations across 1 Ear(s)" in output
    assert "classification=vehicle (confidence=0.72)" in output
    assert "node-relative bearing=180.0 deg (confidence=0.61)" in output
    assert "not events or tracks" in output
    assert "No event/track identity or geographic source position is inferred" in output


def test_observation_diff_reports_only_unseen_ids() -> None:
    new, known = hub_event_track.observation_diff(
        {"obs-1"},
        [
            {"observation_id": "obs-1"},
            {"observation_id": "obs-2"},
            {"node_id": "EAR-001"},
        ],
    )

    assert new == [{"observation_id": "obs-2"}]
    assert known == {"obs-1", "obs-2"}
