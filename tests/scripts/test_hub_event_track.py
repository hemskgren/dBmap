import json
import sys
from datetime import UTC, datetime
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
    assert "Observation reports: 1 (" in output
    assert "page fetch" in output or "complete snapshot" in output
    assert "Unique observations: 1" in output
    assert "Report R1:" not in output
    assert "Candidate C" not in output
    assert "classification=vehicle" not in output
    assert (
        "Ground-truth simulator IDs are not used for grouping or correlation" in output
    )
    assert "SIM-VEHICLE-001" not in output
    assert "Ear activity episode A1 for SIM-EAR-001" in output
    assert "Track is not implemented" in output
    assert "experimental script-local analysis only" in output
    assert "Cross-Ear Bearing Geometry (diagnostic only" in output
    assert "Weather remains informational only" in output


def test_report_details_lists_each_report() -> None:
    output = "\n".join(
        hub_event_track.report(
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
            ],
            include_observation_details=True,
        )
    )

    assert "Observation reports (detail): each report listed separately." in output
    assert "Report R1: 1 independent report(s), 1 Ear(s) (SIM-EAR-001)" in output
    assert "classification=vehicle (confidence=0.72)" in output
    assert "node-relative bearing=180.0 deg (confidence=0.61)" in output
    assert "SIM-VEHICLE-001" not in output


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

    assert "Candidate C" not in output
    assert "Report R" not in output
    assert "Observation reports: 6 (" in output
    assert "page fetch" in output or "complete snapshot" in output
    assert "Unique observations: 6" in output
    assert "SIM-VEHICLE-001" not in output
    assert "Ear activity episode A1 for EAR-001: 1 report(s)" in output
    assert "Ear activity episode A2 for SIM-EAR-001: 3 report(s)" in output
    assert "Ear activity episode A3 for SIM-EAR-002: 1 report(s)" in output
    assert "Ear activity episode A4 for SIM-EAR-002: 1 report(s)" in output
    assert "gap 10.3s exceeds 5s window" in output

    detailed = "\n".join(
        hub_event_track.report(
            observations,
            event_gap_seconds=5,
            include_observation_details=True,
        )
    )
    assert detailed.count("Report R") == 6
    assert "Report R1: 1 independent report(s)" in detailed


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
    # Default report skips the per-report dump; site metadata still follows the header.
    assert "Report R" not in output
    assert output.index("Unique observations:") < output.index(
        "Node installation metadata"
    )
    detailed = "\n".join(
        hub_event_track.report(
            observations, nodes=nodes, include_observation_details=True
        )
    )
    assert detailed.index("Report R1:") < detailed.index("Node installation metadata")


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

    assert "Report R" not in output
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
    assert "Observation reports: 2 (" in output
    assert "page fetch" in output or "complete snapshot" in output
    assert "Report R" not in output
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
    episode_duration_ms: float | None = None,
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
        "episode_duration_ms": (
            interval_duration_ms if episode_duration_ms is None else episode_duration_ms
        ),
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


def test_format_observations_json_sorts_and_pretty_prints_reports() -> None:
    payload = hub_event_track.format_observations_json(
        [
            {
                "identity": {
                    "observation_id": "obs-late",
                    "node_id": "EAR-001",
                    "sequence_number": 2,
                },
                "context": {"event_time_utc": "2026-11-04T12:00:10Z"},
            },
            {
                "identity": {
                    "observation_id": "obs-early",
                    "node_id": "EAR-001",
                    "sequence_number": 1,
                },
                "context": {"event_time_utc": "2026-11-04T12:00:00Z"},
            },
        ]
    )
    parsed = json.loads(payload)
    assert [item["identity"]["observation_id"] for item in parsed] == [
        "obs-early",
        "obs-late",
    ]
    assert '"observation_id": "obs-early"' in payload
    assert payload.startswith("[\n")


def test_filter_observations_matches_web_node_and_observation_id_semantics() -> None:
    observations = [
        {"observation_id": "obs-a", "node_id": "EAR-001"},
        {"observation_id": "obs-b", "node_id": "EAR-001"},
        {"observation_id": "obs-a", "node_id": "EAR-002"},
        {"observation_id": "obs-c", "node_id": "EAR-002"},
    ]

    by_node = hub_event_track.filter_observations(
        observations, node_ids={"EAR-001"}
    )
    assert [item["observation_id"] for item in by_node] == ["obs-a", "obs-b"]

    by_obs = hub_event_track.filter_observations(
        observations, observation_ids={"obs-a"}
    )
    assert [(item["node_id"], item["observation_id"]) for item in by_obs] == [
        ("EAR-001", "obs-a"),
        ("EAR-002", "obs-a"),
    ]

    by_both = hub_event_track.filter_observations(
        observations,
        node_ids={"EAR-002"},
        observation_ids={"obs-a", "obs-c"},
    )
    assert [item["observation_id"] for item in by_both] == ["obs-a", "obs-c"]

    assert hub_event_track.unique_observation_count(observations) == 3
    status = hub_event_track.format_filter_status(
        node_ids={"EAR-001"},
        observation_ids={"obs-a"},
        fetched_count=4,
        filtered_count=1,
    )
    assert status[0].startswith("Filters applied (of 4 fetched reports → 1 match)")
    assert "Node ID: EAR-001" in status[1]
    assert "Observation ID: obs-a" in status[2]


def test_fetch_observation_count_parses_hub_totals(monkeypatch) -> None:
    requests = []

    class Response:
        def __enter__(self):
            return self

        def __exit__(self, *args):
            return None

        def read(self):
            return b'{"total": 192, "unique_observations": 1}'

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

    assert hub_event_track.fetch_observation_count(
        "https://hub.example",
        "ca.crt",
        "test-admin-token",
        node_id="EAR-001",
    ) == {"total": 192, "unique_observations": 1}
    assert "/api/v1/observations/count?node_id=EAR-001" in requests[0].full_url
    assert requests[0].get_header("Authorization") == "Bearer test-admin-token"


def test_parse_positive_int_and_pagination_summary() -> None:
    assert hub_event_track.parse_positive_int(3, label="page") == 3
    with pytest.raises(ValueError, match="page-size"):
        hub_event_track.parse_positive_int(0, label="page-size")
    with pytest.raises(ValueError, match="page"):
        hub_event_track.parse_positive_int("x", label="page")
    assert hub_event_track.format_pagination_summary(
        total_count=8200,
        page=1,
        page_size=200,
        total_pages=41,
        order="newest",
    ) == "8200 reports | 41 pages | page 1/41 | newest first"
    assert hub_event_track.DEFAULT_OBSERVATION_PAGE_SIZE == 200
    assert hub_event_track.MAX_OBSERVATION_PAGE_SIZE == 500


def test_parse_observation_list_envelope_requires_object() -> None:
    with pytest.raises(hub_event_track.HubApiError, match="envelope"):
        hub_event_track.parse_observation_list_envelope([])


def test_fetch_observations_reads_json_envelope(monkeypatch) -> None:
    requests = []
    envelope = {
        "pagination": {
            "total_count": 8200,
            "page": 2,
            "page_size": 200,
            "total_pages": 41,
            "order": "oldest",
            "snapshot_id": 999,
        },
        "reports": [
            {
                "identity": {
                    "observation_id": "o1",
                    "node_id": "EAR-001",
                    "sequence_number": 1,
                }
            }
        ],
    }

    class Response:
        def __init__(self):
            self._payload = json.dumps(envelope)
            self._pos = 0

        def __enter__(self):
            return self

        def __exit__(self, *args):
            return None

        def read(self, size: int = -1):
            if self._pos >= len(self._payload):
                return ""
            if size is None or size < 0:
                chunk = self._payload[self._pos :]
                self._pos = len(self._payload)
                return chunk
            chunk = self._payload[self._pos : self._pos + size]
            self._pos += len(chunk)
            return chunk

    def fake_urlopen(request, context, timeout):
        requests.append(request)
        return Response()

    monkeypatch.setattr(hub_event_track.urllib.request, "urlopen", fake_urlopen)
    monkeypatch.setattr(
        hub_event_track.ssl,
        "create_default_context",
        lambda **kwargs: object(),
    )

    items, meta, full = hub_event_track.fetch_observations(
        "https://hub.example",
        "ca.crt",
        "tok",
        observation_id="o1",
        page=2,
        page_size=200,
        order="oldest",
        snapshot_id=999,
    )
    assert len(items) == 1
    assert meta == envelope["pagination"]
    assert full == envelope
    assert "page=2" in requests[0].full_url
    assert "page_size=200" in requests[0].full_url
    assert "order=oldest" in requests[0].full_url
    assert "observation_id=o1" in requests[0].full_url
    assert "snapshot_id=999" in requests[0].full_url


def test_fetch_observations_rejects_invalid_page_size() -> None:
    with pytest.raises(hub_event_track.HubApiError, match="page_size"):
        hub_event_track.fetch_observations(
            "https://hub.example",
            "ca.crt",
            "tok",
            page_size=501,
        )


def _node(lat: float, lon: float, orientation: float = 0.0) -> dict:
    return {
        "installation": {
            "latitude": lat,
            "longitude": lon,
            "orientation_deg": orientation,
        }
    }


def _obs(
    observation_id: str,
    node_id: str,
    event_time_utc: str,
    *,
    duration_ms: float = 1000,
    bearing_deg: float | None = 90.0,
    bearing_confidence: float | None = 0.8,
    source_family: str | None = "vehicle",
    classification_confidence: float | None = 0.7,
    simulated_source_id: str | None = None,
) -> dict:
    classification = None
    if source_family is not None or simulated_source_id is not None:
        classification = {}
        if source_family is not None:
            classification["source_family"] = source_family
        if classification_confidence is not None:
            classification["confidence"] = classification_confidence
        if simulated_source_id is not None:
            classification["hints"] = {"simulated_source_id": simulated_source_id}
    bearing = None
    if bearing_deg is not None:
        bearing = {"deg": bearing_deg, "reference": "node"}
        if bearing_confidence is not None:
            bearing["confidence"] = bearing_confidence
    return {
        "observation_id": observation_id,
        "node_id": node_id,
        "event_time_utc": event_time_utc,
        "duration_ms": duration_ms,
        "classification": classification,
        "bearing": bearing,
    }


TRIANGLE_NODES = {
    # ~1 km east-west / north baselines — inside a raised association radius.
    "SIM-EAR-001": _node(59.3400, 18.0500, 0.0),
    "SIM-EAR-002": _node(59.3400, 18.0600, 0.0),
    "SIM-EAR-003": _node(59.3480, 18.0550, 180.0),
}


def test_one_ear_cannot_create_cross_ear_candidate() -> None:
    result = hub_event_track.run_cross_ear_correlation(
        [
            _obs("a1", "SIM-EAR-001", "2026-11-05T18:00:20Z"),
            _obs("a2", "SIM-EAR-001", "2026-11-05T18:00:25Z"),
        ],
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert result.candidates == []
    assert result.events == []
    assert any("same Ear" in item.reason for item in result.rejected)


def test_two_compatible_ears_create_one_candidate() -> None:
    result = hub_event_track.run_cross_ear_correlation(
        [
            _obs("a1", "SIM-EAR-001", "2026-11-05T18:00:20Z", bearing_deg=120.0),
            _obs("b1", "SIM-EAR-002", "2026-11-05T18:00:22Z", bearing_deg=240.0),
        ],
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert len(result.events) == 0
    open_candidates = [
        item for item in result.candidates if item.status == "EventCandidate"
    ]
    assert len(open_candidates) == 1
    assert set(open_candidates[0].ear_ids) == {"SIM-EAR-001", "SIM-EAR-002"}
    assert set(open_candidates[0].observation_ids) == {"a1", "b1"}


def test_third_ear_updates_same_candidate_and_can_promote() -> None:
    result = hub_event_track.run_cross_ear_correlation(
        [
            _obs(
                "a1",
                "SIM-EAR-001",
                "2026-11-05T18:00:20Z",
                duration_ms=10000,
                bearing_deg=150.0,
            ),
            _obs(
                "b1",
                "SIM-EAR-002",
                "2026-11-05T18:00:22Z",
                duration_ms=10000,
                bearing_deg=210.0,
            ),
            _obs(
                "c1",
                "SIM-EAR-003",
                "2026-11-05T18:00:24Z",
                duration_ms=10000,
                bearing_deg=0.0,
            ),
        ],
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
        min_ears_for_event=3,
        min_promote_score=4,
    )
    assert len(result.events) == 1
    assert set(result.events[0].ear_ids) == {
        "SIM-EAR-001",
        "SIM-EAR-002",
        "SIM-EAR-003",
    }
    promoted = [item for item in result.candidates if item.status == "Promoted"]
    assert len(promoted) == 1
    assert promoted[0].candidate_id == result.events[0].candidate_id


def test_three_ears_with_insufficient_evidence_do_not_promote() -> None:
    # Gap-only temporal compatibility (no overlap) and no bearings: 3 Ears stay Candidate.
    result = hub_event_track.run_cross_ear_correlation(
        [
            _obs(
                "a1",
                "SIM-EAR-001",
                "2026-11-05T18:00:00Z",
                duration_ms=500,
                bearing_deg=None,
                source_family=None,
            ),
            _obs(
                "b1",
                "SIM-EAR-002",
                "2026-11-05T18:00:10Z",
                duration_ms=500,
                bearing_deg=None,
                source_family=None,
            ),
            _obs(
                "c1",
                "SIM-EAR-003",
                "2026-11-05T18:00:20Z",
                duration_ms=500,
                bearing_deg=None,
                source_family=None,
            ),
        ],
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
        association_temporal_gap_seconds=30.0,
        min_ears_for_event=3,
        min_promote_score=4,
    )
    assert result.events == []
    open_candidates = [
        item for item in result.candidates if item.status == "EventCandidate"
    ]
    assert len(open_candidates) == 1
    assert len(open_candidates[0].ear_ids) == 3
    assert open_candidates[0].score_breakdown["temporal"] == 1
    assert any("promotion withheld" in note for note in open_candidates[0].evidence_notes)


def test_same_ear_observations_cannot_create_cross_ear_support() -> None:
    evidence = hub_event_track.run_cross_ear_correlation(
        [
            _obs("a1", "SIM-EAR-001", "2026-11-05T18:00:20Z"),
            _obs("a2", "SIM-EAR-001", "2026-11-05T18:00:21Z"),
        ],
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert evidence.candidates == []
    assert any("same Ear" in item.reason for item in evidence.rejected)


def test_temporally_incompatible_observation_is_rejected() -> None:
    from lib.cross_ear_event_correlation import (
        assemble_observations,
        evaluate_observation_pair,
    )

    reports = [
        _obs("a1", "SIM-EAR-001", "2026-11-05T18:00:00Z", duration_ms=500),
        _obs("b1", "SIM-EAR-002", "2026-11-05T18:05:00Z", duration_ms=500),
    ]
    assembled = assemble_observations(
        reports,
        TRIANGLE_NODES,
        observation_fields=hub_event_track.observation_fields,
        observation_time=hub_event_track.observation_time,
        sequence_gap_count=hub_event_track.sequence_gap_count,
        sort_observations=hub_event_track.sort_observations,
    )
    evidence = evaluate_observation_pair(
        assembled[0],
        assembled[1],
        TRIANGLE_NODES,
        association_temporal_gap_seconds=30.0,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
        installation_distance_m=hub_event_track.installation_distance_m,
        is_sim_ear=hub_event_track.is_sim_ear,
    )
    assert evidence.rejected
    assert evidence.temporal == "incompatible"


def test_geometrically_contradictory_observation_not_silently_joined() -> None:
    # Outward rays (300° / 60°) yield a behind_both intersection — reject, don't join.
    result = hub_event_track.run_cross_ear_correlation(
        [
            _obs("a1", "SIM-EAR-001", "2026-11-05T18:00:20Z", bearing_deg=300.0),
            _obs("b1", "SIM-EAR-002", "2026-11-05T18:00:22Z", bearing_deg=60.0),
        ],
        {
            "SIM-EAR-001": _node(0.0, 0.0, 0.0),
            "SIM-EAR-002": _node(0.0, 0.01, 0.0),
        },
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert result.candidates == []
    assert any(
        "bearing" in item.reason or "contradictory" in item.reason
        for item in result.rejected
    )


def test_missing_bearings_do_not_automatically_reject_compatible_pair() -> None:
    result = hub_event_track.run_cross_ear_correlation(
        [
            _obs(
                "a1",
                "SIM-EAR-001",
                "2026-11-05T18:00:20Z",
                bearing_deg=None,
                source_family="vehicle",
            ),
            _obs(
                "b1",
                "SIM-EAR-002",
                "2026-11-05T18:00:22Z",
                bearing_deg=None,
                source_family="vehicle",
            ),
        ],
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    open_candidates = [
        item for item in result.candidates if item.status == "EventCandidate"
    ]
    assert len(open_candidates) == 1
    assert open_candidates[0].provisional_position is None


def test_duplicate_input_does_not_create_duplicate_candidates() -> None:
    reports = [
        _obs("a1", "SIM-EAR-001", "2026-11-05T18:00:20Z", bearing_deg=120.0),
        _obs("b1", "SIM-EAR-002", "2026-11-05T18:00:22Z", bearing_deg=240.0),
        # Duplicate reports for the same Observation lifecycles:
        _obs("a1", "SIM-EAR-001", "2026-11-05T18:00:21Z", bearing_deg=121.0),
        _obs("b1", "SIM-EAR-002", "2026-11-05T18:00:23Z", bearing_deg=239.0),
    ]
    result = hub_event_track.run_cross_ear_correlation(
        reports,
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert len(result.observations) == 2
    open_candidates = [
        item for item in result.candidates if item.status == "EventCandidate"
    ]
    assert len(open_candidates) == 1


def test_simulator_ground_truth_hints_do_not_affect_associations() -> None:
    shared_hint = [
        _obs(
            "a1",
            "SIM-EAR-001",
            "2026-11-05T18:00:20Z",
            bearing_deg=120.0,
            simulated_source_id="SIM-XEAR-001",
        ),
        _obs(
            "b1",
            "SIM-EAR-002",
            "2026-11-05T18:00:22Z",
            bearing_deg=240.0,
            simulated_source_id="SIM-XEAR-001",
        ),
    ]
    mixed_hint = [
        _obs(
            "a1",
            "SIM-EAR-001",
            "2026-11-05T18:00:20Z",
            bearing_deg=120.0,
            simulated_source_id="SIM-XEAR-001",
        ),
        _obs(
            "b1",
            "SIM-EAR-002",
            "2026-11-05T18:00:22Z",
            bearing_deg=240.0,
            simulated_source_id="SIM-OTHER-999",
        ),
    ]
    first = hub_event_track.run_cross_ear_correlation(
        shared_hint,
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    second = hub_event_track.run_cross_ear_correlation(
        mixed_hint,
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert len(first.candidates) == len(second.candidates) == 1
    assert first.candidates[0].observation_ids == second.candidates[0].observation_ids
    assert first.candidates[0].score == second.candidates[0].score


def test_event_can_exist_without_position_estimate() -> None:
    result = hub_event_track.run_cross_ear_correlation(
        [
            _obs(
                "a1",
                "SIM-EAR-001",
                "2026-11-05T18:00:20Z",
                duration_ms=10000,
                bearing_deg=None,
                source_family="vehicle",
            ),
            _obs(
                "b1",
                "SIM-EAR-002",
                "2026-11-05T18:00:22Z",
                duration_ms=10000,
                bearing_deg=None,
                source_family="vehicle",
            ),
            _obs(
                "c1",
                "SIM-EAR-003",
                "2026-11-05T18:00:24Z",
                duration_ms=10000,
                bearing_deg=None,
                source_family="vehicle",
            ),
        ],
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
        min_ears_for_event=3,
        min_promote_score=4,
    )
    assert len(result.events) == 1
    assert result.events[0].provisional_position is None
    assert result.events[0].localization is not None
    assert result.events[0].localization.status == "unavailable"
    # Classification family must not be required for promotion.
    assert result.events[0].score_breakdown["acoustic"] == 0


def test_classification_null_matches_classification_present_association() -> None:
    with_class = [
        _obs(
            "a1",
            "SIM-EAR-001",
            "2026-11-05T18:00:20Z",
            duration_ms=10000,
            bearing_deg=120.0,
        ),
        _obs(
            "b1",
            "SIM-EAR-002",
            "2026-11-05T18:00:22Z",
            duration_ms=10000,
            bearing_deg=240.0,
        ),
        _obs(
            "c1",
            "SIM-EAR-003",
            "2026-11-05T18:00:24Z",
            duration_ms=10000,
            bearing_deg=0.0,
        ),
    ]
    null_class = [
        _obs(
            "a1",
            "SIM-EAR-001",
            "2026-11-05T18:00:20Z",
            duration_ms=10000,
            bearing_deg=120.0,
            source_family=None,
        ),
        _obs(
            "b1",
            "SIM-EAR-002",
            "2026-11-05T18:00:22Z",
            duration_ms=10000,
            bearing_deg=240.0,
            source_family=None,
        ),
        _obs(
            "c1",
            "SIM-EAR-003",
            "2026-11-05T18:00:24Z",
            duration_ms=10000,
            bearing_deg=0.0,
            source_family=None,
        ),
    ]
    first = hub_event_track.run_cross_ear_correlation(
        with_class,
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    second = hub_event_track.run_cross_ear_correlation(
        null_class,
        TRIANGLE_NODES,
        max_association_distance_m=5000,
        allow_long_range_sim_ears=True,
    )
    assert len(first.events) == len(second.events) == 1
    assert first.events[0].score == second.events[0].score
    assert first.events[0].score_breakdown == second.events[0].score_breakdown
    assert first.events[0].score_breakdown["acoustic"] == 0


def test_cross_ear_event_pass_scenario_end_to_end() -> None:
    from importlib.util import module_from_spec, spec_from_file_location

    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_event_pass.json"
    )
    sim_spec = spec_from_file_location(
        "dbmap_ear_scenario_simulator_for_correlation",
        Path(__file__).parents[2] / "simulator" / "ear" / "simulate_scenario.py",
    )
    assert sim_spec is not None and sim_spec.loader is not None
    simulate_scenario = module_from_spec(sim_spec)
    sim_spec.loader.exec_module(simulate_scenario)

    scenario = simulate_scenario.load_scenario(scenario_path)
    credentials = {
        "ear_a": {"node_id": "SIM-EAR-001", "next_sequence_number": 1},
        "ear_b": {"node_id": "SIM-EAR-002", "next_sequence_number": 1},
        "ear_c": {"node_id": "SIM-EAR-003", "next_sequence_number": 1},
    }
    published: list[dict] = []
    simulate_scenario.run_scenario(
        scenario,
        credentials,
        {
            "ear_a": Path("a.json"),
            "ear_b": Path("b.json"),
            "ear_c": Path("c.json"),
        },
        "ca.crt",
        1.0,
        publish=lambda _credentials, payload, _ca: (
            published.append(payload) or payload["identity"]["observation_id"]
        ),
        sleep=lambda _seconds: None,
        monotonic=lambda: 10**12,
        started_at_utc=datetime(2026, 11, 5, 18, 0, tzinfo=UTC),
        started_at_monotonic_us=0,
        save=lambda *_args, **_kwargs: None,
        publish_keepalive=lambda *_args, **_kwargs: None,
        make_keepalive=lambda credentials, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )
    nodes = {
        "SIM-EAR-001": {
            "installation": {
                "latitude": scenario["ears"]["ear_a"]["latitude"],
                "longitude": scenario["ears"]["ear_a"]["longitude"],
                "orientation_deg": scenario["ears"]["ear_a"]["orientation_deg"],
            }
        },
        "SIM-EAR-002": {
            "installation": {
                "latitude": scenario["ears"]["ear_b"]["latitude"],
                "longitude": scenario["ears"]["ear_b"]["longitude"],
                "orientation_deg": scenario["ears"]["ear_b"]["orientation_deg"],
            }
        },
        "SIM-EAR-003": {
            "installation": {
                "latitude": scenario["ears"]["ear_c"]["latitude"],
                "longitude": scenario["ears"]["ear_c"]["longitude"],
                "orientation_deg": scenario["ears"]["ear_c"]["orientation_deg"],
            }
        },
    }

    # Progressive slices: A alone → A+B → A+B+C → full incl. negative.
    by_time = sorted(
        published,
        key=lambda item: item["context"]["event_time_utc"],
    )
    main_pass = [
        item
        for item in by_time
        if item["context"]["event_time_utc"] < "2026-11-05T18:02:55"
    ]

    def up_to_ear_count(ear_count: int) -> list[dict]:
        if ear_count == 1:
            return [
                item
                for item in main_pass
                if item["identity"]["node_id"] == "SIM-EAR-001"
            ]
        if ear_count == 2:
            return [
                item
                for item in main_pass
                if item["identity"]["node_id"] in {"SIM-EAR-001", "SIM-EAR-002"}
            ]
        return main_pass

    alone = hub_event_track.run_cross_ear_correlation(
        up_to_ear_count(1),
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=True,
    )
    assert alone.candidates == []
    assert alone.events == []

    pair = hub_event_track.run_cross_ear_correlation(
        up_to_ear_count(2),
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=True,
    )
    assert len(pair.events) == 0
    assert len([c for c in pair.candidates if c.status == "EventCandidate"]) == 1

    triple = hub_event_track.run_cross_ear_correlation(
        up_to_ear_count(3),
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=True,
    )
    assert len(triple.events) == 1
    assert len(triple.events[0].ear_ids) == 3
    assert triple.events[0].score_breakdown["acoustic"] == 0
    assert triple.events[0].localization is not None

    # Null classification must not change association outcome on this scenario.
    import copy

    null_main = []
    for payload in up_to_ear_count(3):
        item = copy.deepcopy(payload)
        item["acoustic"]["classification"] = None
        null_main.append(item)
    null_result = hub_event_track.run_cross_ear_correlation(
        null_main,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=True,
    )
    assert len(null_result.events) == 1
    assert null_result.events[0].score == triple.events[0].score

    full = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=True,
    )
    assert len(full.events) == 1
    assert len(full.observations) == 4
    event_obs = set(full.events[0].observation_ids)
    negative = [
        item
        for item in full.observations
        if item.observation_id not in event_obs
    ]
    assert len(negative) == 1
    assert negative[0].node_id == "SIM-EAR-002"
    assert any(
        item.observation_id == negative[0].observation_id for item in full.rejected
    )

    output = "\n".join(
        hub_event_track.report(
            published,
            nodes=nodes,
            max_association_distance_m=500,
            allow_long_range_sim_ears=True,
            include_cross_ear_geometry=False,
        )
    )
    assert "Event Candidates (open): 0" in output or "Event Candidates (open):" in output
    assert "Events (promoted): 1" in output
    assert "Rejected associations:" in output
    assert "Localization (separate from association)" in output
    assert "source_family does not award acoustic" in output
    assert "SIM-XEAR-001" not in output


def _publish_cross_ear_event_pass():
    from importlib.util import module_from_spec, spec_from_file_location

    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_event_pass.json"
    )
    sim_spec = spec_from_file_location(
        "dbmap_ear_scenario_simulator_b21",
        Path(__file__).parents[2] / "simulator" / "ear" / "simulate_scenario.py",
    )
    assert sim_spec is not None and sim_spec.loader is not None
    simulate_scenario = module_from_spec(sim_spec)
    sim_spec.loader.exec_module(simulate_scenario)
    scenario = simulate_scenario.load_scenario(scenario_path)
    credentials = {
        "ear_a": {"node_id": "SIM-EAR-001", "next_sequence_number": 1},
        "ear_b": {"node_id": "SIM-EAR-002", "next_sequence_number": 1},
        "ear_c": {"node_id": "SIM-EAR-003", "next_sequence_number": 1},
    }
    published: list[dict] = []
    simulate_scenario.run_scenario(
        scenario,
        credentials,
        {
            "ear_a": Path("a.json"),
            "ear_b": Path("b.json"),
            "ear_c": Path("c.json"),
        },
        "ca.crt",
        1.0,
        publish=lambda _credentials, payload, _ca: (
            published.append(payload) or payload["identity"]["observation_id"]
        ),
        sleep=lambda _seconds: None,
        monotonic=lambda: 10**12,
        started_at_utc=datetime(2026, 11, 5, 18, 0, tzinfo=UTC),
        started_at_monotonic_us=0,
        save=lambda *_args, **_kwargs: None,
        publish_keepalive=lambda *_args, **_kwargs: None,
        make_keepalive=lambda credentials, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )
    nodes = {
        "SIM-EAR-001": {
            "installation": {
                "latitude": scenario["ears"]["ear_a"]["latitude"],
                "longitude": scenario["ears"]["ear_a"]["longitude"],
                "orientation_deg": scenario["ears"]["ear_a"]["orientation_deg"],
            }
        },
        "SIM-EAR-002": {
            "installation": {
                "latitude": scenario["ears"]["ear_b"]["latitude"],
                "longitude": scenario["ears"]["ear_b"]["longitude"],
                "orientation_deg": scenario["ears"]["ear_b"]["orientation_deg"],
            }
        },
        "SIM-EAR-003": {
            "installation": {
                "latitude": scenario["ears"]["ear_c"]["latitude"],
                "longitude": scenario["ears"]["ear_c"]["longitude"],
                "orientation_deg": scenario["ears"]["ear_c"]["orientation_deg"],
            }
        },
    }
    return scenario, published, nodes


def test_timing_offsets_preserve_event_and_degrade_localization() -> None:
    from lib.cross_ear_localization import (
        apply_sensor_imperfections,
    )

    _scenario, published, nodes = _publish_cross_ear_event_pass()
    main = [
        item
        for item in published
        if item["context"]["event_time_utc"] < "2026-11-05T18:02:55"
    ]
    baseline = hub_event_track.run_cross_ear_correlation(
        main,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=True,
    )
    assert len(baseline.events) == 1

    for offset in (0.1, 0.5, 1.0):
        imperfect = apply_sensor_imperfections(
            main,
            ear_time_offsets_s={
                "SIM-EAR-001": 0.0,
                "SIM-EAR-002": offset,
                "SIM-EAR-003": -offset / 2,
            },
            bearing_noise_deg=0.0,
            seed=42,
        )
        result = hub_event_track.run_cross_ear_correlation(
            imperfect,
            nodes,
            max_association_distance_m=500,
            allow_long_range_sim_ears=True,
        )
        assert len(result.events) == 1, f"offset={offset}"
        loc = result.events[0].localization
        assert loc is not None
        assert loc.max_bearing_dt_s is not None
        assert loc.max_bearing_dt_s + 1e-9 >= offset / 2
        # Localization may weaken at 1.0s but Event remains.
        if offset >= 1.0:
            assert loc.status in {"weak", "useful", "unavailable"}


def test_bearing_noise_and_gt_evaluation_do_not_affect_association() -> None:
    from lib.cross_ear_localization import (
        apply_sensor_imperfections,
        evaluate_against_ground_truth,
        source_position_on_route,
    )

    scenario, published, nodes = _publish_cross_ear_event_pass()
    main = [
        item
        for item in published
        if item["context"]["event_time_utc"] < "2026-11-05T18:02:55"
    ]
    baseline = hub_event_track.run_cross_ear_correlation(
        main,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=True,
    )
    assert len(baseline.events) == 1
    base_ids = set(baseline.events[0].observation_ids)

    for noise in (1.0, 3.0, 5.0):
        imperfect = apply_sensor_imperfections(
            main,
            bearing_noise_deg=noise,
            seed=7,
        )
        result = hub_event_track.run_cross_ear_correlation(
            imperfect,
            nodes,
            max_association_distance_m=500,
            allow_long_range_sim_ears=True,
        )
        assert len(result.events) == 1, f"noise={noise}"
        assert set(result.events[0].observation_ids) == base_ids
        assert result.events[0].score_breakdown["acoustic"] == 0

    # GT evaluation is post-hoc only.
    loc = baseline.events[0].localization
    assert loc is not None and loc.evaluation_time_utc is not None
    started = datetime(2026, 11, 5, 18, 0, tzinfo=UTC)
    at_seconds = (loc.evaluation_time_utc - started).total_seconds()
    gt_lat, gt_lon = source_position_on_route(
        scenario["movement"]["start"],
        scenario["movement"]["end"],
        speed_m_s=scenario["movement"]["speed_m_s"],
        at_seconds=at_seconds,
    )
    evaluation = evaluate_against_ground_truth(
        loc,
        ground_truth_latitude=gt_lat,
        ground_truth_longitude=gt_lon,
        ground_truth_time_utc=loc.evaluation_time_utc,
    )
    assert evaluation.error_m is not None
    assert "not used for association" in evaluation.notes


def test_network_geometry_classifies_dense_vs_sparse() -> None:
    from lib.cross_ear_localization import classify_network_geometry

    sparse_class, sparse_max, _ = classify_network_geometry(
        {
            "SIM-EAR-001": _node(59.348, 18.055),
            "SIM-EAR-002": _node(59.336, 18.085),
            "SIM-EAR-003": _node(59.334, 18.045),
        }
    )
    assert sparse_class == "sparse"
    assert sparse_max is not None and sparse_max > 500

    dense_class, dense_max, baselines = classify_network_geometry(
        {
            "SIM-EAR-001": _node(59.344534, 18.056),
            "SIM-EAR-002": _node(59.3408, 18.060228),
            "SIM-EAR-003": _node(59.3408, 18.051772),
        }
    )
    assert dense_class == "dense"
    assert dense_max is not None and dense_max <= 500
    assert len(baselines) == 3


def test_cross_ear_dense_pass_event_and_dense_localization_class() -> None:
    from importlib.util import module_from_spec, spec_from_file_location

    from lib.cross_ear_localization import (
        evaluate_against_ground_truth,
        source_position_on_route,
    )

    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_dense_pass.json"
    )
    sim_spec = spec_from_file_location(
        "dbmap_ear_scenario_simulator_dense",
        Path(__file__).parents[2] / "simulator" / "ear" / "simulate_scenario.py",
    )
    assert sim_spec is not None and sim_spec.loader is not None
    simulate_scenario = module_from_spec(sim_spec)
    sim_spec.loader.exec_module(simulate_scenario)
    scenario = simulate_scenario.load_scenario(scenario_path)
    credentials = {
        "ear_a": {"node_id": "SIM-EAR-001", "next_sequence_number": 1},
        "ear_b": {"node_id": "SIM-EAR-002", "next_sequence_number": 1},
        "ear_c": {"node_id": "SIM-EAR-003", "next_sequence_number": 1},
    }
    published: list[dict] = []
    simulate_scenario.run_scenario(
        scenario,
        credentials,
        {
            "ear_a": Path("a.json"),
            "ear_b": Path("b.json"),
            "ear_c": Path("c.json"),
        },
        "ca.crt",
        1.0,
        publish=lambda _credentials, payload, _ca: (
            published.append(payload) or payload["identity"]["observation_id"]
        ),
        sleep=lambda _seconds: None,
        monotonic=lambda: 10**12,
        started_at_utc=datetime(2026, 11, 5, 18, 0, tzinfo=UTC),
        started_at_monotonic_us=0,
        save=lambda *_args, **_kwargs: None,
        publish_keepalive=lambda *_args, **_kwargs: None,
        make_keepalive=lambda credentials, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )
    nodes = {
        "SIM-EAR-001": {
            "installation": {
                "latitude": scenario["ears"]["ear_a"]["latitude"],
                "longitude": scenario["ears"]["ear_a"]["longitude"],
                "orientation_deg": scenario["ears"]["ear_a"]["orientation_deg"],
            }
        },
        "SIM-EAR-002": {
            "installation": {
                "latitude": scenario["ears"]["ear_b"]["latitude"],
                "longitude": scenario["ears"]["ear_b"]["longitude"],
                "orientation_deg": scenario["ears"]["ear_b"]["orientation_deg"],
            }
        },
        "SIM-EAR-003": {
            "installation": {
                "latitude": scenario["ears"]["ear_c"]["latitude"],
                "longitude": scenario["ears"]["ear_c"]["longitude"],
                "orientation_deg": scenario["ears"]["ear_c"]["orientation_deg"],
            }
        },
    }
    main = [
        item
        for item in published
        if item["context"]["event_time_utc"] < "2026-11-05T18:01:15"
    ]
    result = hub_event_track.run_cross_ear_correlation(
        main,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    loc = result.events[0].localization
    assert loc is not None
    assert loc.details.get("network_class") == "dense"
    assert loc.details.get("evaluation_target_m") == 100.0
    assert loc.latitude is not None
    started = datetime(2026, 11, 5, 18, 0, tzinfo=UTC)
    assert loc.evaluation_time_utc is not None
    at_seconds = (loc.evaluation_time_utc - started).total_seconds()
    gt_lat, gt_lon = source_position_on_route(
        scenario["movement"]["start"],
        scenario["movement"]["end"],
        speed_m_s=scenario["movement"]["speed_m_s"],
        at_seconds=at_seconds,
    )
    evaluation = evaluate_against_ground_truth(
        loc,
        ground_truth_latitude=gt_lat,
        ground_truth_longitude=gt_lon,
        ground_truth_time_utc=loc.evaluation_time_utc,
    )
    assert evaluation.network_class == "dense"
    assert evaluation.target_m == 100.0
    assert evaluation.error_m is not None


def test_ground_truth_hints_do_not_change_localization_inputs() -> None:
    """Localization uses bearings/installations only — not simulated_source_id."""
    from lib.cross_ear_localization import estimate_localization

    reports = [
        _obs(
            "a1",
            "SIM-EAR-001",
            "2026-11-05T18:00:20Z",
            bearing_deg=120.0,
            simulated_source_id="SIM-XEAR-001",
        ),
        _obs(
            "b1",
            "SIM-EAR-002",
            "2026-11-05T18:00:20Z",
            bearing_deg=240.0,
            simulated_source_id="SIM-OTHER",
        ),
    ]
    assembled = hub_event_track.build_assembled_observations(reports, TRIANGLE_NODES)
    loc = estimate_localization(assembled, TRIANGLE_NODES)
    # Even with conflicting GT hints, localization still runs on bearings.
    assert loc.status in {"unavailable", "weak", "useful"}
    for member in assembled:
        assert member.classification is not None
        assert "association_policy" in member.classification


def test_observation_lifecycle_summary_multi_device_open_and_periodic() -> None:
    completed_a = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
    open_a = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
    completed_b = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
    periodic_only = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"
    observations = [
        _hierarchical_report(
            observation_id=completed_a,
            node_id="EAR-001",
            sequence_number=1,
            event_time_utc="2026-11-10T10:00:00Z",
            emission_kind="start",
            interval_duration_ms=2020.0,
            episode_duration_ms=2020.0,
        ),
        _hierarchical_report(
            observation_id=completed_a,
            node_id="EAR-001",
            sequence_number=2,
            event_time_utc="2026-11-10T10:00:01Z",
            emission_kind="periodic",
            interval_duration_ms=1000.0,
            episode_duration_ms=3020.0,
        ),
        _hierarchical_report(
            observation_id=completed_a,
            node_id="EAR-001",
            sequence_number=3,
            event_time_utc="2026-11-10T10:00:03Z",
            emission_kind="end",
            interval_duration_ms=3060.0,
            episode_duration_ms=5080.0,
        ),
        _hierarchical_report(
            observation_id=open_a,
            node_id="EAR-001",
            sequence_number=4,
            event_time_utc="2026-11-10T10:05:00Z",
            emission_kind="start",
            interval_duration_ms=1000.0,
            episode_duration_ms=1000.0,
        ),
        _hierarchical_report(
            observation_id=open_a,
            node_id="EAR-001",
            sequence_number=5,
            event_time_utc="2026-11-10T10:05:05Z",
            emission_kind="periodic",
            interval_duration_ms=1000.0,
            episode_duration_ms=6000.0,
        ),
        _hierarchical_report(
            observation_id=completed_b,
            node_id="EAR-002",
            sequence_number=10,
            event_time_utc="2026-11-10T10:00:00Z",
            emission_kind="start",
            interval_duration_ms=1000.0,
            episode_duration_ms=1000.0,
        ),
        _hierarchical_report(
            observation_id=completed_b,
            node_id="EAR-002",
            sequence_number=11,
            event_time_utc="2026-11-10T10:00:02Z",
            emission_kind="end",
            interval_duration_ms=1500.0,
            episode_duration_ms=2500.0,
        ),
        _hierarchical_report(
            observation_id=periodic_only,
            node_id="EAR-002",
            sequence_number=12,
            event_time_utc="2026-11-10T10:10:00Z",
            emission_kind="periodic",
            interval_duration_ms=1000.0,
            episode_duration_ms=1000.0,
        ),
    ]

    summary = hub_event_track.summarize_observation_lifecycles(observations)
    assert summary.combined.unique_observations == 3
    assert summary.combined.completed_observations == 2
    assert summary.combined.open_observations == 1
    assert summary.combined.shortest_completed_ms == 2500.0
    assert summary.combined.longest_completed_ms == 5080.0
    assert summary.combined.total_completed_ms == 7580.0

    by_device = {item.node_id: item for item in summary.devices}
    assert by_device["EAR-001"].unique_observations == 2
    assert by_device["EAR-001"].completed_observations == 1
    assert by_device["EAR-001"].open_observations == 1
    assert by_device["EAR-001"].shortest_completed_ms == 5080.0
    assert by_device["EAR-001"].total_completed_ms == 5080.0
    assert by_device["EAR-002"].unique_observations == 1
    assert by_device["EAR-002"].completed_observations == 1
    assert by_device["EAR-002"].open_observations == 0
    assert by_device["EAR-002"].total_completed_ms == 2500.0

    assert any("without START/COMPLETE" in note for note in summary.quality_notes)
    assert summary.oldest_report_event_time is not None
    assert summary.oldest_report_event_time.isoformat(timespec="seconds") == (
        "2026-11-10T10:00:00+00:00"
    )

    fetched_at = datetime(2026, 11, 10, 12, 24, 15, tzinfo=UTC)
    summary.fetched_at = fetched_at
    lines = hub_event_track.format_observation_lifecycle_summary(
        summary,
        fetch_meta={"snapshot_id": 42, "complete_snapshot": True},
    )
    text = "\n".join(lines)
    assert "Observation lifecycle summary" in text
    assert "retained history" in text
    assert "hub_snapshot_id: 42" in text
    assert "oldest report:" in text
    assert "fetched at:" in text
    assert "Device" in text and "Unique" in text and "Completed" in text
    assert "ALL" in text
    assert "EAR-001" in text
    assert "EAR-002" in text
    assert "5080 ms (5.1s)" in text
    assert "2500 ms (2.5s)" in text
    assert "Data-quality notes" in text

    payload = hub_event_track.observation_lifecycle_summary_to_dict(
        summary,
        fetch_meta={"snapshot_id": 42, "complete_snapshot": True},
    )
    assert payload["report_count"] == 8
    assert payload["oldest_report_event_time_utc"] == "2026-11-10T10:00:00+00:00"
    assert payload["fetched_at_utc"] == "2026-11-10T12:24:15+00:00"
    assert payload["combined"]["unique_observations"] == 3
    assert payload["combined"]["total_completed_ms"] == 7580.0
    assert payload["hub_snapshot_id"] == 42
    assert len(payload["devices"]) == 2


def test_observation_lifecycle_summary_ignores_arrival_gap_and_duplicate_start() -> None:
    oid = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"
    observations = [
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-003",
            sequence_number=1,
            event_time_utc="2026-11-10T11:00:00Z",
            emission_kind="start",
            interval_duration_ms=1000.0,
            episode_duration_ms=1000.0,
        ),
        # Duplicate START must not create a second unique observation.
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-003",
            sequence_number=2,
            event_time_utc="2026-11-10T11:00:01Z",
            emission_kind="start",
            interval_duration_ms=1000.0,
            episode_duration_ms=2000.0,
        ),
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-003",
            sequence_number=3,
            event_time_utc="2026-11-10T11:00:10Z",
            emission_kind="end",
            interval_duration_ms=9000.0,
            episode_duration_ms=12000.0,
        ),
    ]
    summary = hub_event_track.summarize_observation_lifecycles(observations)
    assert summary.combined.unique_observations == 1
    assert summary.combined.completed_observations == 1
    assert summary.combined.open_observations == 0
    assert summary.combined.total_completed_ms == 12000.0
    assert any("duplicate START" in note for note in summary.quality_notes)


def test_observation_lifecycle_summary_complete_kind_counts_as_completed() -> None:
    oid = "ffffffff-ffff-4fff-8fff-ffffffffffff"
    observations = [
        _hierarchical_report(
            observation_id=oid,
            node_id="EAR-004",
            sequence_number=1,
            event_time_utc="2026-11-10T12:00:00Z",
            emission_kind="complete",
            interval_duration_ms=4000.0,
            episode_duration_ms=4000.0,
        )
    ]
    summary = hub_event_track.summarize_observation_lifecycles(observations)
    assert summary.combined.unique_observations == 1
    assert summary.combined.completed_observations == 1
    assert summary.combined.open_observations == 0
    assert summary.combined.total_completed_ms == 4000.0
