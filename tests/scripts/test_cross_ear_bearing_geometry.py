import math
import sys
from datetime import UTC, datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

import pytest
from lib import cross_ear_bearing_geometry as geometry


def test_bearing_conversion_zero_orientation() -> None:
    assert geometry.geographic_bearing_deg(0.0, 45.0) == pytest.approx(45.0)


def test_bearing_conversion_nonzero_orientation() -> None:
    assert geometry.geographic_bearing_deg(270.0, 90.0) == pytest.approx(0.0)
    assert geometry.geographic_bearing_deg(90.0, 90.0) == pytest.approx(180.0)


def test_angle_normalization_around_0_360() -> None:
    assert geometry.normalize_deg(360.0) == pytest.approx(0.0)
    assert geometry.normalize_deg(-10.0) == pytest.approx(350.0)
    assert geometry.geographic_bearing_deg(350.0, 20.0) == pytest.approx(10.0)
    assert geometry.smallest_angle_diff_deg(350.0, 10.0) == pytest.approx(20.0)


def test_forward_intersection_of_two_rays() -> None:
    # Ear A at origin looking east (90°); Ear B 1000 m east / 200 m south looking north.
    origin_lat, origin_lon = 59.0, 18.0
    result = geometry.intersect_rays_en((0.0, 0.0), 90.0, (1000.0, -200.0), 0.0)
    assert result.status == "forward_intersection"
    assert result.east_m == pytest.approx(1000.0, abs=1e-6)
    assert result.north_m == pytest.approx(0.0, abs=1e-6)
    assert result.t1_m == pytest.approx(1000.0, abs=1e-6)
    assert result.t2_m == pytest.approx(200.0, abs=1e-6)
    lat, lon = geometry.local_en_to_latlon(
        result.east_m,
        result.north_m,
        origin_lat=origin_lat,
        origin_lon=origin_lon,
    )
    assert math.isfinite(lat) and math.isfinite(lon)


def test_nearly_parallel_rays() -> None:
    result = geometry.intersect_rays_en((0.0, 0.0), 45.0, (10.0, 0.0), 45.0)
    assert result.status == "nearly_parallel"
    assert result.east_m is None


def test_intersection_behind_one_ear() -> None:
    # A looks east to (500,0); B sits north of that point looking further north.
    result = geometry.intersect_rays_en((0.0, 0.0), 90.0, (500.0, 100.0), 0.0)
    assert result.status == "behind_one"
    assert result.t1_m == pytest.approx(500.0, abs=1e-6)
    assert result.t2_m is not None and result.t2_m < 0


def test_intersection_behind_both_ears() -> None:
    # Forward rays diverge north-west / north-east; crossing is south, behind both.
    result = geometry.intersect_rays_en((0.0, 0.0), 315.0, (100.0, 0.0), 45.0)
    assert result.status == "behind_both"
    assert result.t1_m is not None and result.t1_m < 0
    assert result.t2_m is not None and result.t2_m < 0


def test_missing_orientation_ineligible() -> None:
    prepared = geometry.prepare_observation_geometry(
        {
            "observation_id": "obs-1",
            "node_id": "EAR-001",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "bearing": {"deg": 10.0, "reference": "node", "confidence": 0.8},
        },
        {
            "EAR-001": {
                "installation": {"latitude": 59.0, "longitude": 18.0},
            }
        },
    )
    assert not prepared.eligible
    assert prepared.exclusion_reason is not None
    assert "orientation_deg missing" in prepared.exclusion_reason
    assert prepared.geographic_bearing_deg is None


def test_invalid_orientation_ineligible() -> None:
    prepared = geometry.prepare_observation_geometry(
        {
            "observation_id": "obs-1",
            "node_id": "EAR-001",
            "bearing": {"deg": 10.0, "reference": "node"},
        },
        {
            "EAR-001": {
                "installation": {
                    "latitude": 59.0,
                    "longitude": 18.0,
                    "orientation_deg": 360.0,
                }
            }
        },
    )
    assert not prepared.eligible
    assert "out of [0, 360)" in (prepared.exclusion_reason or "")


def test_missing_or_invalid_bearing_ineligible() -> None:
    missing = geometry.prepare_observation_geometry(
        {"observation_id": "obs-1", "node_id": "EAR-001", "bearing": None},
        {
            "EAR-001": {
                "installation": {
                    "latitude": 59.0,
                    "longitude": 18.0,
                    "orientation_deg": 0.0,
                }
            }
        },
    )
    assert not missing.eligible
    assert "bearing" in (missing.exclusion_reason or "")

    invalid = geometry.prepare_observation_geometry(
        {
            "observation_id": "obs-2",
            "node_id": "EAR-001",
            "bearing": {"deg": 360.0, "reference": "node"},
        },
        {
            "EAR-001": {
                "installation": {
                    "latitude": 59.0,
                    "longitude": 18.0,
                    "orientation_deg": 0.0,
                }
            }
        },
    )
    assert not invalid.eligible


def test_angular_residual_calculation() -> None:
    nodes = {
        "EAR-A": {
            "installation": {
                "latitude": 0.0,
                "longitude": 0.0,
                "orientation_deg": 0.0,
            }
        },
        "EAR-B": {
            "installation": {
                "latitude": 0.0,
                "longitude": 0.01,  # ~1112 m east
                "orientation_deg": 0.0,
            }
        },
    }
    # Geographic east from A (90°) and north from B (0°) → forward intersection.
    obs_a = geometry.prepare_observation_geometry(
        {
            "observation_id": "a",
            "node_id": "EAR-A",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "bearing": {"deg": 90.0, "reference": "node", "confidence": 0.9},
            "classification": {"source_family": "helicopter", "confidence": 0.8},
        },
        nodes,
    )
    obs_b = geometry.prepare_observation_geometry(
        {
            "observation_id": "b",
            "node_id": "EAR-B",
            "event_time_utc": "2026-10-05T18:00:02Z",
            "bearing": {"deg": 0.0, "reference": "node", "confidence": 0.85},
            "classification": {"source_family": "helicopter", "confidence": 0.7},
        },
        nodes,
    )
    assert obs_a.eligible and obs_b.eligible
    pair = geometry.analyze_pair(obs_a, obs_b)
    assert pair.intersection_status == "forward_intersection"
    assert pair.residual_a_deg == pytest.approx(0.0, abs=0.05)
    assert pair.residual_b_deg == pytest.approx(0.0, abs=0.05)
    assert pair.geometric_compatible


def test_low_confidence_observations_flagged() -> None:
    prepared = geometry.prepare_observation_geometry(
        {
            "observation_id": "obs-1",
            "node_id": "EAR-001",
            "bearing": {"deg": 10.0, "reference": "node", "confidence": 0.2},
        },
        {
            "EAR-001": {
                "installation": {
                    "latitude": 59.0,
                    "longitude": 18.0,
                    "orientation_deg": 0.0,
                }
            }
        },
    )
    assert prepared.eligible
    assert prepared.low_bearing_confidence


def test_temporal_compatibility_with_different_time_gaps() -> None:
    base = {
        "latitude": 0.0,
        "longitude": 0.0,
        "node_relative_bearing_deg": 90.0,
        "orientation_deg": 0.0,
        "geographic_bearing_deg": 90.0,
        "bearing_confidence": 0.9,
        "eligible": True,
        "exclusion_reason": None,
        "source_family": "helicopter",
        "classification_confidence": 0.8,
        "low_bearing_confidence": False,
    }
    close_a = geometry.ObservationGeometry(
        observation_id="a",
        node_id="EAR-A",
        event_time_utc=datetime(2026, 10, 5, 18, 0, 0, tzinfo=UTC),
        **base,
    )
    close_b = geometry.ObservationGeometry(
        observation_id="b",
        node_id="EAR-B",
        event_time_utc=datetime(2026, 10, 5, 18, 0, 10, tzinfo=UTC),
        **{**base, "longitude": 0.01, "geographic_bearing_deg": 0.0},
    )
    far_b = geometry.ObservationGeometry(
        observation_id="c",
        node_id="EAR-B",
        event_time_utc=datetime(2026, 10, 5, 18, 5, 0, tzinfo=UTC),
        **{**base, "longitude": 0.01, "geographic_bearing_deg": 0.0},
    )
    ok, delta, notes = geometry.assess_temporal_compatibility(
        close_a, close_b, distance_a_m=1000.0, distance_b_m=200.0
    )
    assert ok
    assert delta == pytest.approx(10.0)
    assert "quasi-static" in notes
    assert "ear→locus ranges 1000m / 200m" in notes
    assert "not source speed" in notes

    weak, far_delta, far_notes = geometry.assess_temporal_compatibility(
        close_a, far_b, distance_a_m=1000.0, distance_b_m=200.0
    )
    assert not weak
    assert far_delta == pytest.approx(300.0)
    assert "outside provisional" in far_notes


def test_same_ear_observations_are_not_cross_ear_pairs() -> None:
    nodes = {
        "EAR-A": {
            "installation": {
                "latitude": 0.0,
                "longitude": 0.0,
                "orientation_deg": 0.0,
            }
        }
    }
    observations = [
        {
            "observation_id": "a1",
            "node_id": "EAR-A",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "bearing": {"deg": 10.0, "reference": "node", "confidence": 0.9},
        },
        {
            "observation_id": "a2",
            "node_id": "EAR-A",
            "event_time_utc": "2026-10-05T18:00:05Z",
            "bearing": {"deg": 20.0, "reference": "node", "confidence": 0.9},
        },
    ]
    report = geometry.analyze_cross_ear_geometry(observations, nodes)
    assert report.eligible_count == 2
    assert report.pair_count == 0


def test_simulator_ground_truth_hints_do_not_enter_algorithm() -> None:
    nodes = {
        "EAR-A": {
            "installation": {
                "latitude": 0.0,
                "longitude": 0.0,
                "orientation_deg": 0.0,
            }
        },
        "EAR-B": {
            "installation": {
                "latitude": 0.0,
                "longitude": 0.01,
                "orientation_deg": 0.0,
            }
        },
    }
    observations = [
        {
            "observation_id": "a",
            "node_id": "EAR-A",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "bearing": {"deg": 90.0, "reference": "node", "confidence": 0.9},
            "classification": {
                "source_family": "helicopter",
                "confidence": 0.8,
                "hints": {"simulated_source_id": "SIM-HELI-001"},
            },
        },
        {
            "observation_id": "b",
            "node_id": "EAR-B",
            "event_time_utc": "2026-10-05T18:00:02Z",
            "bearing": {"deg": 0.0, "reference": "node", "confidence": 0.85},
            "classification": {
                "source_family": "helicopter",
                "confidence": 0.7,
                "hints": {"simulated_source_id": "SIM-HELI-001"},
            },
        },
    ]
    report = geometry.analyze_cross_ear_geometry(observations, nodes)
    text = "\n".join(geometry.format_cross_ear_geometry_lines(report, detail=True))
    assert "SIM-HELI-001" not in text
    assert report.pair_count == 1
    # Hints must not be read: monkey-proof by ensuring analyze uses only public fields.
    assert all(
        "simulated_source_id" not in (pair.geometric_notes + pair.temporal_notes)
        for pair in report.pairs
    )


def test_weather_context_does_not_affect_results() -> None:
    nodes = {
        "EAR-A": {
            "installation": {
                "latitude": 0.0,
                "longitude": 0.0,
                "orientation_deg": 0.0,
            }
        },
        "EAR-B": {
            "installation": {
                "latitude": 0.0,
                "longitude": 0.01,
                "orientation_deg": 0.0,
            }
        },
    }
    observations = [
        {
            "observation_id": "a",
            "node_id": "EAR-A",
            "event_time_utc": "2026-10-05T18:00:00Z",
            "bearing": {"deg": 90.0, "reference": "node", "confidence": 0.9},
            "classification": {"source_family": "helicopter", "confidence": 0.8},
        },
        {
            "observation_id": "b",
            "node_id": "EAR-B",
            "event_time_utc": "2026-10-05T18:00:02Z",
            "bearing": {"deg": 0.0, "reference": "node", "confidence": 0.85},
            "classification": {"source_family": "helicopter", "confidence": 0.7},
        },
    ]
    plain = geometry.analyze_cross_ear_geometry(observations, nodes)
    with_weather = geometry.analyze_cross_ear_geometry(
        observations,
        nodes,
        environmental_context={
            "wind_speed_ms": 40.0,
            "wind_direction_deg": 90.0,
            "temperature_c": -20.0,
        },
    )
    assert plain.pair_count == with_weather.pair_count == 1
    assert (
        plain.pairs[0].intersection_latitude
        == with_weather.pairs[0].intersection_latitude
    )
    assert (
        plain.pairs[0].geometric_compatible
        == with_weather.pairs[0].geometric_compatible
    )
    assert (
        plain.pairs[0].temporal_compatible == with_weather.pairs[0].temporal_compatible
    )
