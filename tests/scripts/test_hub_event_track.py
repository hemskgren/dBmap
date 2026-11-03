import sys
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

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

    assert (
        hub_event_track.fetch_nodes(
            "https://hub.example",
            "ca.crt",
            "test-admin-token",
        )
        == {}
    )
    assert requests[0].get_header("Authorization") == "Bearer test-admin-token"


def test_candidate_grouping_does_not_use_simulated_ground_truth() -> None:
    observations = [
        {
            "observation_id": "obs-1",
            "node_id": "SIM-EAR-001",
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
        {
            "observation_id": "obs-2",
            "node_id": "SIM-EAR-002",
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
        {
            "observation_id": "obs-3",
            "node_id": "EAR-001",
            "classification": None,
        },
    ]

    groups = hub_event_track.candidate_groups(observations)

    assert [[item["observation_id"] for item in items] for _, items in groups] == [
        ["obs-1"],
        ["obs-2"],
        ["obs-3"],
    ]


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
                    "hints": {"simulated_source_id": "SIM-VEHICLE-001"},
                },
                "bearing": {"deg": 180.0, "reference": "node", "confidence": 0.61},
            }
        ]
    )

    output = "\n".join(lines)
    assert "Candidate C1: 1 independent observation(s), 1 Ear(s)" in output
    assert "classification=vehicle (confidence=0.72)" in output
    assert "node-relative bearing=180.0 deg (confidence=0.61)" in output
    assert (
        "Ground-truth simulator IDs are not used for grouping or correlation" in output
    )
    assert "SIM-VEHICLE-001" not in output
    assert "Ear activity episode A1 for SIM-EAR-001" in output
    assert "No Event or Track association is attempted" in output
    assert "Cross-Ear Bearing Geometry (diagnostic only" in output
    assert "Weather remains informational only" in output


def test_report_cross_ear_geometry_detail_and_weather_unchanged() -> None:
    observations = [
        {
            "observation_id": "obs-1",
            "node_id": "EAR-001",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "bearing": {"deg": 90.0, "reference": "node", "confidence": 0.9},
            "classification": {"source_family": "helicopter", "confidence": 0.8},
        },
        {
            "observation_id": "obs-2",
            "node_id": "EAR-002",
            "event_time_utc": "2026-10-05T18:00:02Z",
            "bearing": {"deg": 0.0, "reference": "node", "confidence": 0.85},
            "classification": {"source_family": "helicopter", "confidence": 0.7},
        },
    ]
    nodes = {
        "EAR-001": {
            "installation": {
                "latitude": 0.0,
                "longitude": 0.0,
                "orientation_deg": 0.0,
            }
        },
        "EAR-002": {
            "installation": {
                "latitude": 0.0,
                "longitude": 0.01,
                "orientation_deg": 0.0,
            }
        },
    }
    weather = {
        "region_id": "local-01",
        "provider": "open-meteo",
        "availability": "ok",
        "from_cache": True,
        "age_s": 1,
        "temperature_c": 8.0,
        "relative_humidity_pct": 70,
        "wind_speed_ms": 2.0,
        "wind_direction_deg": 200,
    }
    output = "\n".join(
        hub_event_track.report(
            observations,
            nodes=nodes,
            environmental_context=weather,
            cross_ear_geometry_detail=True,
        )
    )
    assert "Regional environmental context (informational; not association input)" in output
    assert "temp=8.0°C" in output
    assert "geographic=90.00°" in output
    assert "geographic=0.00°" in output
    assert "forward_intersection" in output
    assert "not an Event/Track" in output
    assert "angular σ=unavailable" in output
    omitted = "\n".join(
        hub_event_track.report(
            observations,
            nodes=nodes,
            include_cross_ear_geometry=False,
        )
    )
    assert "Cross-Ear Bearing Geometry" not in omitted


def test_report_keeps_same_hint_observations_separate_and_groups_only_ear_activity() -> (
    None
):
    observations = [
        {
            "observation_id": "obs-5",
            "node_id": "SIM-EAR-002",
            "event_time_utc": "2026-10-05T18:00:15Z",
            "duration_ms": 650,
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
        {
            "observation_id": "obs-4",
            "node_id": "SIM-EAR-001",
            "event_time_utc": "2026-10-05T18:00:06Z",
            "duration_ms": 800,
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
        {
            "observation_id": "obs-3",
            "node_id": "SIM-EAR-002",
            "event_time_utc": "2026-10-05T18:00:04Z",
            "duration_ms": 700,
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
        {
            "observation_id": "obs-2",
            "node_id": "SIM-EAR-001",
            "event_time_utc": "2026-10-05T18:00:02Z",
            "duration_ms": 900,
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
        {
            "observation_id": "obs-1",
            "node_id": "SIM-EAR-001",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "duration_ms": 850,
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
        {
            "observation_id": "obs-unhinted",
            "node_id": "EAR-001",
            "event_time_utc": "2026-10-05T18:00:03Z",
            "classification": None,
        },
    ]

    output = "\n".join(hub_event_track.report(observations, event_gap_seconds=5))

    assert output.count("Candidate C") == 6
    assert "Candidate C1: 1 independent observation(s)" in output
    assert "SIM-VEHICLE-001" not in output
    assert "Ear activity episode A1 for EAR-001: 1 report(s)" in output
    assert "Ear activity episode A2 for SIM-EAR-001: 3 report(s)" in output
    assert "Ear activity episode A3 for SIM-EAR-002: 1 report(s)" in output
    assert "Ear activity episode A4 for SIM-EAR-002: 1 report(s)" in output
    assert "gap 10.3s exceeds 5s window" in output


def test_event_gap_window_is_configurable_and_requires_finite_positive_value() -> None:
    observations = [
        {
            "observation_id": "obs-1",
            "node_id": "EAR-001",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "duration_ms": 850,
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
        {
            "observation_id": "obs-2",
            "node_id": "EAR-001",
            "event_time_utc": "2026-10-05T18:00:15Z",
            "duration_ms": 650,
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
    ]

    output = "\n".join(hub_event_track.report(observations, event_gap_seconds=20))

    assert "Ear activity episode A1 for EAR-001: 2 report(s)" in output
    assert "gap 14.2s within 20s window" in output
    for invalid_interval in (0, -1, float("nan"), float("inf")):
        with pytest.raises(ValueError, match="finite and greater than zero"):
            hub_event_track.parse_positive_interval(invalid_interval)


def test_report_prints_installations_and_site_distance_without_associating_sources() -> (
    None
):
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
    assert (
        "Ear installation distances (site context only; not source associations)"
        in output
    )
    assert "EAR-001 ↔ EAR-002: 556m (>500m)" in output
    # Candidates appear before installation metadata.
    assert output.index("Candidate C1:") < output.index("Node installation metadata")


def test_report_never_uses_airborne_ground_truth_hint_for_grouping() -> None:
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

    assert output.count("Candidate C") == 2
    assert "SIM-DRONE-001" not in output
    assert "EAR-001 ↔ EAR-002: 556m (>500m)" in output
    assert "Ground-truth simulator IDs are not used" in output


def test_report_can_omit_site_context_for_watch_updates() -> None:
    observations = [
        {
            "observation_id": "obs-1",
            "node_id": "SIM-EAR-001",
            "event_time_utc": "2026-10-05T18:00:00Z",
        },
        {
            "observation_id": "obs-2",
            "node_id": "SIM-EAR-002",
            "event_time_utc": "2026-10-05T18:00:03Z",
        },
    ]
    nodes = {
        "SIM-EAR-001": {
            "installation": {
                "latitude": 59.35100847144696,
                "longitude": 18.04842390753845,
                "height_m": 74,
            }
        },
        "SIM-EAR-002": {
            "installation": {
                "latitude": 59.335118371024905,
                "longitude": 18.12680373585177,
                "height_m": 155,
            }
        },
    }
    weather = {
        "region_id": "local-01",
        "provider": "open-meteo",
        "availability": "ok",
        "from_cache": True,
        "age_s": 1,
        "temperature_c": 8.0,
        "relative_humidity_pct": 70,
        "wind_speed_ms": 2.0,
        "wind_direction_deg": 200,
    }
    output = "\n".join(
        hub_event_track.report(
            observations,
            nodes=nodes,
            environmental_context=weather,
            include_site_context=False,
        )
    )
    assert "Candidate C1:" in output
    assert "Ear activity episode" in output
    assert "Node installation metadata" not in output
    assert "Ear installation distances" not in output
    assert "Regional environmental context" not in output


def test_report_prints_single_regional_weather_block() -> None:
    observations = [
        {
            "observation_id": "obs-1",
            "node_id": "SIM-EAR-001",
            "event_time_utc": "2026-10-05T18:00:00Z",
        }
    ]
    weather = {
        "region_id": "local-01",
        "provider": "open-meteo",
        "source_mode": "live",
        "availability": "ok",
        "from_cache": True,
        "age_s": 12,
        "fetched_at": "2026-10-05T18:00:00+00:00",
        "temperature_c": 8.5,
        "relative_humidity_pct": 70,
        "wind_speed_ms": 2.1,
        "wind_direction_deg": 200,
    }
    output = "\n".join(
        hub_event_track.report(
            observations,
            environmental_context=weather,
        )
    )
    assert "Regional environmental context (informational; not association input)" in output
    assert "region=local-01  provider=open-meteo  mode=live" in output
    assert "temp=8.5°C" in output
    assert output.count("Regional environmental context") == 1


def test_sim_ear_long_range_option_keeps_pair_eligible_without_hiding_distance() -> None:
    observations = [
        {
            "observation_id": "obs-1",
            "node_id": "SIM-EAR-001",
            "event_time_utc": "2026-10-05T18:00:00Z",
        },
        {
            "observation_id": "obs-2",
            "node_id": "SIM-EAR-002",
            "event_time_utc": "2026-10-05T18:00:03Z",
        },
    ]
    nodes = {
        "SIM-EAR-001": {
            "installation": {
                "latitude": 59.35100847144696,
                "longitude": 18.04842390753845,
                "height_m": 74,
            }
        },
        "SIM-EAR-002": {
            "installation": {
                "latitude": 59.335118371024905,
                "longitude": 18.12680373585177,
                "height_m": 155,
            }
        },
    }

    default_output = "\n".join(
        hub_event_track.report(observations, nodes=nodes)
    )
    allowed_output = "\n".join(
        hub_event_track.report(
            observations,
            nodes=nodes,
            allow_long_range_sim_ears=True,
        )
    )

    assert "SIM-EAR-001 ↔ SIM-EAR-002: 4782m (>500m)" in default_output
    assert "SIM eligible" not in default_output
    assert "SIM-EAR-001 ↔ SIM-EAR-002: 4782m (>500m, SIM eligible)" in allowed_output
    assert "SIM eligible = long-range SIM-EAR pair kept for analysis" in allowed_output


def test_default_inactivity_window_closes_episode_only_after_silence_exceeds_30_seconds() -> (
    None
):
    observations = [
        {
            "observation_id": "obs-1",
            "node_id": "EAR-001",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "duration_ms": 850,
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
        {
            "observation_id": "obs-2",
            "node_id": "EAR-001",
            "event_time_utc": "2026-10-05T18:00:15Z",
            "duration_ms": 650,
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
        {
            "observation_id": "obs-3",
            "node_id": "EAR-001",
            "event_time_utc": "2026-10-05T18:00:50Z",
            "duration_ms": 650,
            "classification": {"hints": {"simulated_source_id": "SIM-VEHICLE-001"}},
        },
    ]

    output = "\n".join(hub_event_track.report(observations))

    assert "30s of silence after interval end closes an episode" in output
    assert "Ear activity episode A1 for EAR-001: 2 report(s)" in output
    assert "Ear activity episode A2 for EAR-001: 1 report(s)" in output
    assert "gap 34.4s exceeds 30s window" in output
    assert "does not mean observations inside an episode share one source" in output


def _hierarchical_report(
    *,
    observation_id: str,
    node_id: str,
    sequence_number: int | None,
    event_time_utc: str,
    emission_kind: str | None,
    interval_duration_ms: float = 1000.0,
) -> dict:
    identity = {
        "observation_id": observation_id,
        "node_id": node_id,
        "hub_id": "local-01",
    }
    if sequence_number is not None:
        identity["sequence_number"] = sequence_number
    context = {
        "event_time_utc": event_time_utc,
        "capture_timestamp_monotonic_us": 0,
        "timing_quality": "estimated",
        "clock_offset_ms": None,
        "timestamp_uncertainty_ms": 10.0,
        "interval_duration_ms": interval_duration_ms,
        "episode_duration_ms": interval_duration_ms,
    }
    if emission_kind is not None:
        context["emission_kind"] = emission_kind
    return {
        "protocol_version": 1,
        "message_type": "observation",
        "identity": identity,
        "context": context,
        "acoustic": {
            "signal_level_dbfs": -40.0,
            "measurements": None,
            "signature": None,
            "classification": None,
        },
        "spatial": {"bearing": None, "position": None},
    }


def test_episode_summary_one_observation_many_periodics() -> None:
    oid = "11111111-1111-4111-8111-111111111111"
    observations = [
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-001",
            sequence_number=10 + index,
            event_time_utc=f"2026-11-03T18:00:{index:02d}Z",
            emission_kind="periodic",
        )
        for index in range(5)
    ]

    output = "\n".join(hub_event_track.report(observations, event_gap_seconds=30))

    assert "Ear activity episode A1 for EAR-001: 5 report(s)" in output
    assert "observations: 1 unique observation" in output
    assert f"Observation {oid}" in output
    assert "reports: 5" in output
    assert "emissions: START=0 PERIODIC=5 END=0 COMPLETE=0" in output
    assert "sequence: 10–14" in output
    assert "sequence gaps: 0" in output
    episode_section = output.split("Per-Ear activity episodes", 1)[1]
    assert episode_section.count(oid) == 1


def test_episode_summary_start_periodic_end() -> None:
    oid = "22222222-2222-4222-8222-222222222222"
    observations = [
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-001",
            sequence_number=100,
            event_time_utc="2026-11-03T18:00:00Z",
            emission_kind="start",
        ),
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-001",
            sequence_number=101,
            event_time_utc="2026-11-03T18:00:05Z",
            emission_kind="periodic",
        ),
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-001",
            sequence_number=102,
            event_time_utc="2026-11-03T18:00:10Z",
            emission_kind="end",
        ),
    ]

    output = "\n".join(hub_event_track.report(observations, event_gap_seconds=30))

    assert "Ear activity episode A1 for EAR-001: 3 report(s)" in output
    assert "emissions: START=1 PERIODIC=1 END=1 COMPLETE=0" in output
    assert "sequence: 100–102" in output
    assert "sequence gaps: 0" in output


def test_episode_summary_multiple_observation_ids() -> None:
    first = "33333333-3333-4333-8333-333333333333"
    second = "44444444-4444-4444-8444-444444444444"
    observations = [
        _hierarchical_report(
            observation_id=first,
            node_id="EAR-001",
            sequence_number=1,
            event_time_utc="2026-11-03T18:00:00Z",
            emission_kind="start",
        ),
        _hierarchical_report(
            observation_id=first,
            node_id="EAR-001",
            sequence_number=2,
            event_time_utc="2026-11-03T18:00:05Z",
            emission_kind="end",
        ),
        _hierarchical_report(
            observation_id=second,
            node_id="EAR-001",
            sequence_number=3,
            event_time_utc="2026-11-03T18:00:08Z",
            emission_kind="complete",
        ),
    ]

    output = "\n".join(hub_event_track.report(observations, event_gap_seconds=30))

    assert "observations: 2 unique observations" in output
    assert f"Observation {first}" in output
    assert f"Observation {second}" in output
    assert "emissions: START=1 PERIODIC=0 END=1 COMPLETE=0" in output
    assert "emissions: START=0 PERIODIC=0 END=0 COMPLETE=1" in output


def test_episode_summary_sequence_gaps() -> None:
    oid = "55555555-5555-4555-8555-555555555555"
    observations = [
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-001",
            sequence_number=20,
            event_time_utc="2026-11-03T18:00:00Z",
            emission_kind="start",
        ),
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-001",
            sequence_number=22,
            event_time_utc="2026-11-03T18:00:05Z",
            emission_kind="periodic",
        ),
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-001",
            sequence_number=25,
            event_time_utc="2026-11-03T18:00:10Z",
            emission_kind="end",
        ),
    ]

    assert hub_event_track.sequence_gap_count([20, 22, 25]) == 3
    output = "\n".join(hub_event_track.report(observations, event_gap_seconds=30))
    assert "sequence: 20–25" in output
    assert "sequence gaps: 3" in output


def test_episode_summary_missing_emission_kind_and_sequence() -> None:
    oid = "66666666-6666-4666-8666-666666666666"
    observations = [
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-001",
            sequence_number=None,
            event_time_utc="2026-11-03T18:00:00Z",
            emission_kind=None,
        ),
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-001",
            sequence_number=None,
            event_time_utc="2026-11-03T18:00:05Z",
            emission_kind="periodic",
        ),
    ]

    output = "\n".join(hub_event_track.report(observations, event_gap_seconds=30))

    assert "emissions: START=0 PERIODIC=1 END=0 COMPLETE=0 unknown=1" in output
    assert "sequence: n/a" in output
    assert "sequence gaps: n/a" in output


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
