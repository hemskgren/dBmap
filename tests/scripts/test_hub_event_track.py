from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

spec = spec_from_file_location(
    "dbmap_hub_event_track",
    Path(__file__).parents[2] / "scripts" / "hub_event_track.py",
)
assert spec is not None and spec.loader is not None
hub_event_track = module_from_spec(spec)
spec.loader.exec_module(hub_event_track)


def test_fetch_nodes_uses_admin_bearer_token(monkeypatch) -> None:
    requests = []

    class Response:
        def __enter__(self):
            return self

        def __exit__(self, *args):
            return None

        def read(self):
            return b"[]"

    def fake_urlopen(request, context, timeout):
        requests.append(request)
        assert timeout == 15
        return Response()

    monkeypatch.setattr(hub_event_track.urllib.request, "urlopen", fake_urlopen)
    monkeypatch.setattr(
        hub_event_track.ssl,
        "create_default_context",
        lambda **kwargs: object(),
    )

    assert hub_event_track.fetch_nodes(
        "https://hub.example:8443",
        "ca.crt",
        "test-admin-token",
    ) == {}
    assert requests[0].get_header("Authorization") == "Bearer test-admin-token"


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
    assert "Candidate C1: simulator hint SIM-VEHICLE-001; 1 observation(s), 1 Ear(s)" in output
    assert "classification=vehicle (confidence=0.72)" in output
    assert "node-relative bearing=180.0 deg (confidence=0.61)" in output
    assert "Event-shaped inactivity episodes (not event correlation or persisted Events)" in output
    assert "not persistent Events or Tracks" in output


def test_report_groups_candidates_and_proposes_events_by_inactivity_gap() -> None:
    observations = [
        {
            "observation_id": "obs-5",
            "node_id": "SIM-EAR-002",
            "event_time_utc": "2026-10-05T18:00:15Z",
            "duration_ms": 650,
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
        {
            "observation_id": "obs-4",
            "node_id": "SIM-EAR-001",
            "event_time_utc": "2026-10-05T18:00:06Z",
            "duration_ms": 800,
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
        {
            "observation_id": "obs-3",
            "node_id": "SIM-EAR-002",
            "event_time_utc": "2026-10-05T18:00:04Z",
            "duration_ms": 700,
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
        {
            "observation_id": "obs-2",
            "node_id": "SIM-EAR-001",
            "event_time_utc": "2026-10-05T18:00:02Z",
            "duration_ms": 900,
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
        {
            "observation_id": "obs-1",
            "node_id": "SIM-EAR-001",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "duration_ms": 850,
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
        {
            "observation_id": "obs-unhinted",
            "node_id": "EAR-001",
            "event_time_utc": "2026-10-05T18:00:03Z",
            "classification": None,
        },
    ]

    output = "\n".join(hub_event_track.report(observations, event_gap_seconds=5))

    assert "Candidate C1: simulator hint SIM-VEHICLE-001; 5 observation(s), 2 Ear(s)" in output
    assert "Candidate C2: no simulator hint; 1 observation(s), 1 Ear(s)" in output
    assert "Event proposal E1 from Candidate C1: 4 observation(s)" in output
    assert "observations: obs-1, obs-2, obs-3, obs-4" in output
    assert "Event proposal E2 from Candidate C1: 1 observation(s)" in output
    assert "gap 8.2s exceeds 5s window" in output
    assert "Event proposal E3 from Candidate C2: 1 observation(s)" in output


def test_event_gap_window_is_configurable_and_requires_finite_positive_value() -> None:
    observations = [
        {
            "observation_id": "obs-1",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "duration_ms": 850,
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
        {
            "observation_id": "obs-2",
            "event_time_utc": "2026-10-05T18:00:15Z",
            "duration_ms": 650,
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
    ]

    output = "\n".join(hub_event_track.report(observations, event_gap_seconds=20))

    assert "Event proposal E1 from Candidate C1: 2 observation(s)" in output
    assert "gap 14.2s within 20s window" in output
    for invalid_interval in (0, -1, float("nan"), float("inf")):
        with pytest.raises(ValueError, match="finite and greater than zero"):
            hub_event_track.parse_positive_interval(invalid_interval)


def test_report_prints_installations_and_marks_distant_local_source_as_less_plausible() -> None:
    observations = [
        {
            "observation_id": "obs-1",
            "node_id": "EAR-001",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "classification": {
                "source_family": "vehicle",
                "hints": {"simulated_source_id": "SIM-CAR-001"},
            },
        },
        {
            "observation_id": "obs-2",
            "node_id": "EAR-002",
            "event_time_utc": "2026-10-05T18:00:03Z",
            "classification": {
                "source_family": "vehicle",
                "hints": {"simulated_source_id": "SIM-CAR-001"},
            },
        },
    ]
    nodes = {
        "EAR-001": {
            "installation": {
                "latitude": 0.0,
                "longitude": 0.0,
                "height_m": 2.5,
                "mount_type": "wall",
                "environment": "outdoor",
                "orientation_deg": 90.0,
            }
        },
        "EAR-002": {
            "installation": {
                "latitude": 0.005,
                "longitude": 0.0,
                "height_m": 3.0,
                "mount_type": "wall",
                "environment": "outdoor",
                "orientation_deg": 270.0,
            }
        },
    }

    output = "\n".join(hub_event_track.report(observations, nodes=nodes))

    assert "EAR-001: lat/lon=0.000000,0.000000" in output
    assert "mount height=2.5m" in output
    assert "EAR-002: lat/lon=0.005000,0.000000" in output
    assert "spatial assessment (local-ground; local radius 500m)" in output
    assert "556m — beyond local radius; one shared local ground source is less plausible" in output


def test_airborne_source_distance_is_printed_without_local_ground_rejection() -> None:
    observations = [
        {
            "observation_id": "obs-1",
            "node_id": "EAR-001",
            "classification": {
                "source_family": "drone",
                "hints": {"simulated_source_id": "SIM-DRONE-001"},
            },
        },
        {
            "observation_id": "obs-2",
            "node_id": "EAR-002",
            "classification": {
                "source_family": "drone",
                "hints": {"simulated_source_id": "SIM-DRONE-001"},
            },
        },
    ]
    nodes = {
        "EAR-001": {"installation": {"latitude": 0.0, "longitude": 0.0}},
        "EAR-002": {"installation": {"latitude": 0.005, "longitude": 0.0}},
    }

    output = "\n".join(hub_event_track.report(observations, nodes=nodes))

    assert "spatial assessment (airborne; local radius 500m)" in output
    assert "556m — beyond local radius; distance alone does not exclude an airborne source" in output
    assert "This is only a temporal boundary" in output


def test_default_inactivity_window_closes_episode_only_after_silence_exceeds_15_seconds() -> None:
    observations = [
        {
            "observation_id": "obs-1",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "duration_ms": 850,
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
        {
            "observation_id": "obs-2",
            "event_time_utc": "2026-10-05T18:00:15Z",
            "duration_ms": 650,
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
        {
            "observation_id": "obs-3",
            "event_time_utc": "2026-10-05T18:00:31Z",
            "duration_ms": 650,
            "classification": {
                "hints": {"simulated_source_id": "SIM-VEHICLE-001"}
            },
        },
    ]

    output = "\n".join(hub_event_track.report(observations))

    assert "15s of silence after interval end closes an episode" in output
    assert "Event proposal E1 from Candidate C1: 2 observation(s)" in output
    assert "Event proposal E2 from Candidate C1: 1 observation(s)" in output
    assert "gap 15.3s exceeds 15s window" in output
    assert "does not mean observations inside an episode share one source" in output


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
