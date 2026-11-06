"""B2.3 localization robustness and failure-boundary tests.

Ground truth never feeds association or position calculation.
Event promotion rules are not relaxed for localization outcomes.
"""

from __future__ import annotations

import copy
import sys
from datetime import datetime, timezone
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_localization import (
    apply_sensor_imperfections,
    estimate_localization,
    evaluate_against_ground_truth,
    haversine_m,
    intersection_angle_deg,
)

spec = spec_from_file_location(
    "dbmap_hub_event_track_b23",
    Path(__file__).parents[2] / "scripts" / "hub_event_track.py",
)
assert spec is not None and spec.loader is not None
hub_event_track = module_from_spec(spec)
spec.loader.exec_module(hub_event_track)


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
    duration_ms: float = 2000,
    bearing_deg: float | None = 90.0,
    bearing_confidence: float | None = 0.8,
) -> dict:
    bearing = None
    if bearing_deg is not None:
        bearing = {
            "deg": bearing_deg,
            "reference": "node",
            "confidence": bearing_confidence,
        }
    return {
        "observation_id": observation_id,
        "node_id": node_id,
        "event_time_utc": event_time_utc,
        "duration_ms": duration_ms,
        "classification": {
            "source_family": "vehicle",
            "confidence": 0.7,
            "association_policy": (
                "simulator_or_unverified_classification; "
                "ignored for association score"
            ),
        },
        "bearing": bearing,
    }


def _publish_scenario(scenario_path: Path) -> tuple[dict, list[dict], dict]:
    sim_spec = spec_from_file_location(
        "dbmap_sim_b23",
        Path(__file__).parents[2] / "simulator" / "ear" / "simulate_scenario.py",
    )
    assert sim_spec is not None and sim_spec.loader is not None
    simulate_scenario = module_from_spec(sim_spec)
    sim_spec.loader.exec_module(simulate_scenario)
    scenario = simulate_scenario.load_scenario(scenario_path)
    alias_to_node = {
        "ear_a": "SIM-EAR-001",
        "ear_b": "SIM-EAR-002",
        "ear_c": "SIM-EAR-003",
        "ear_d": "SIM-EAR-004",
    }
    credentials = {
        alias: {"node_id": alias_to_node[alias], "next_sequence_number": 1}
        for alias in scenario["ears"]
    }
    published: list[dict] = []
    simulate_scenario.run_scenario(
        scenario,
        credentials,
        {alias: Path(f"{alias}.json") for alias in scenario["ears"]},
        "ca.crt",
        1.0,
        publish=lambda _c, payload, _ca: (
            published.append(payload) or payload["identity"]["observation_id"]
        ),
        sleep=lambda _s: None,
        monotonic=lambda: 10**12,
        started_at_utc=datetime(2026, 11, 5, 18, 0, tzinfo=timezone.utc),
        started_at_monotonic_us=0,
        save=lambda *_a, **_k: None,
        publish_keepalive=lambda *_a, **_k: None,
        make_keepalive=lambda credentials, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )
    nodes = {
        alias_to_node[alias]: {
            "installation": {
                "latitude": ear["latitude"],
                "longitude": ear["longitude"],
                "orientation_deg": ear["orientation_deg"],
            }
        }
        for alias, ear in scenario["ears"].items()
    }
    return scenario, published, nodes


def test_intersection_angle_helper_is_acute() -> None:
    assert intersection_angle_deg(0.0, 10.0) == 10.0
    assert intersection_angle_deg(0.0, 170.0) == 10.0
    assert abs(intersection_angle_deg(10.0, 100.0) - 90.0) < 1e-9


def test_nearly_parallel_geometry_is_weak_or_unavailable() -> None:
    """Tight baseline + far source ⇒ acute LOBs; must not claim useful."""
    # Ears ~160 m E-W; bearings nearly north (node-relative with orientation 0).
    nodes = {
        "SIM-EAR-001": _node(59.342045, 18.054589),
        "SIM-EAR-002": _node(59.342045, 18.056),
        "SIM-EAR-003": _node(59.342045, 18.057411),
    }
    reports = [
        _obs("a1", "SIM-EAR-001", "2026-11-05T18:00:10Z", bearing_deg=2.0),
        _obs("b1", "SIM-EAR-002", "2026-11-05T18:00:10Z", bearing_deg=0.0),
        _obs("c1", "SIM-EAR-003", "2026-11-05T18:00:10Z", bearing_deg=358.0),
    ]
    assembled = hub_event_track.build_assembled_observations(reports, nodes)
    loc = estimate_localization(assembled, nodes)
    assert loc.status in {"weak", "unavailable"}
    assert loc.status != "useful"
    best = loc.details.get("best_intersection_angle_deg")
    if best is not None:
        assert best < 20.0
    if loc.details.get("position_withheld"):
        assert loc.latitude is None
        assert loc.status == "unavailable"


def test_misleading_bearing_does_not_silently_claim_useful() -> None:
    """One large-but-associable bearing bias must not stay 'useful'.

    Extreme contradictions (e.g. ~90°) can make association mark bearings
    incompatible — that is association evidence, not a localization claim.
    A 25° bias still associates and is the localization robustness case.
    """
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_stress_inconsistent_bearing.json"
    )
    _scenario, published, nodes = _publish_scenario(scenario_path)
    baseline = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(baseline.events) == 1
    assert baseline.events[0].localization is not None
    assert baseline.events[0].localization.status in {"useful", "weak"}

    biased = apply_sensor_imperfections(
        published,
        ear_bearing_bias_deg={"SIM-EAR-002": 25.0},
        seed=13,
    )
    result = hub_event_track.run_cross_ear_correlation(
        biased,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1  # Event independent of localization quality
    loc = result.events[0].localization
    assert loc is not None
    assert loc.status in {"weak", "unavailable"}
    assert loc.status != "useful"
    # Either withheld or clearly degraded vs clean baseline.
    if loc.latitude is not None and baseline.events[0].localization.latitude is not None:
        assert loc.details.get("outlier_pairs") or (
            loc.pairwise_spread_m is not None and loc.pairwise_spread_m > 100.0
        )


def test_missing_bearings_do_not_block_event_when_association_holds() -> None:
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_dense_stationary.json"
    )
    _scenario, published, nodes = _publish_scenario(scenario_path)
    dropped = apply_sensor_imperfections(
        published,
        drop_bearing_node_ids={"SIM-EAR-003"},
        seed=3,
    )
    # Confirm bearings removed for ear_c.
    assert any(
        item["identity"]["node_id"] == "SIM-EAR-003"
        and item.get("spatial", {}).get("bearing") is None
        for item in dropped
    )
    result = hub_event_track.run_cross_ear_correlation(
        dropped,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    loc = result.events[0].localization
    assert loc is not None
    # Localization may be weak/unavailable; Event still exists.
    assert loc.status in {"weak", "useful", "unavailable"}


def test_valid_event_can_have_unavailable_localization() -> None:
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_stress_nearly_parallel.json"
    )
    _scenario, published, nodes = _publish_scenario(scenario_path)
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    loc = result.events[0].localization
    assert loc is not None
    assert loc.status in {"weak", "unavailable"}
    if loc.status == "unavailable":
        assert loc.latitude is None


def test_ground_truth_never_affects_association_or_position() -> None:
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_dense_stationary.json"
    )
    scenario, published, nodes = _publish_scenario(scenario_path)
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    loc = result.events[0].localization
    assert loc is not None and loc.latitude is not None

    # Mutate GT-only fields on a deep copy; localization inputs unchanged.
    mutated = copy.deepcopy(published)
    for item in mutated:
        hints = (
            item.setdefault("classification", {})
            .setdefault("hints", {})
        )
        hints["simulated_source_id"] = "SIM-FAKE-GT"
        hints["ground_truth_latitude"] = 0.0
        hints["ground_truth_longitude"] = 0.0
    result2 = hub_event_track.run_cross_ear_correlation(
        mutated,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result2.events) == 1
    loc2 = result2.events[0].localization
    assert loc2 is not None
    assert loc2.latitude == loc.latitude
    assert loc2.longitude == loc.longitude
    assert set(result2.events[0].observation_ids) == set(
        result.events[0].observation_ids
    )

    # Post-hoc GT evaluation uses route math only after the fact.
    evaluation = evaluate_against_ground_truth(
        loc,
        ground_truth_latitude=scenario["movement"]["start"]["latitude"],
        ground_truth_longitude=scenario["movement"]["start"]["longitude"],
        ground_truth_time_utc=loc.evaluation_time_utc,
    )
    assert "not used for association" in evaluation.notes
    # Re-running estimate is independent of that evaluation object.
    assert loc.latitude is not None


def test_b22_dense_stationary_baseline_still_useful() -> None:
    """Preserve B2.2 good-geometry baseline: dense stationary remains useful."""
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_dense_stationary.json"
    )
    _scenario, published, nodes = _publish_scenario(scenario_path)
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    loc = result.events[0].localization
    assert loc is not None
    assert loc.status == "useful"
    assert loc.latitude is not None
    assert loc.details.get("network_class") == "dense"
    best = loc.details.get("best_intersection_angle_deg")
    assert best is not None and best >= 20.0


def test_conflicting_bearing_samples_degrade_quality() -> None:
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_stress_inconsistent_bearing.json"
    )
    _scenario, published, nodes = _publish_scenario(scenario_path)
    conflicted = apply_sensor_imperfections(
        published,
        conflicting_ear_bias_deg={"SIM-EAR-002": 40.0},
        seed=13,
    )
    result = hub_event_track.run_cross_ear_correlation(
        conflicted,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    loc = result.events[0].localization
    assert loc is not None
    assert loc.status in {"weak", "unavailable"}


def test_far_source_stress_scenario_not_silently_useful_when_acute() -> None:
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_stress_far_source.json"
    )
    _scenario, published, nodes = _publish_scenario(scenario_path)
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    loc = result.events[0].localization
    assert loc is not None
    best = loc.details.get("best_intersection_angle_deg")
    # Far source over a ~480 m triangle ⇒ acute LOBs (~9° scale).
    if best is not None and best < 20.0:
        assert loc.status in {"weak", "unavailable"}
        if best < 10.0:
            assert loc.status == "unavailable"
            assert loc.latitude is None


def test_single_incorrect_bearing_can_shift_locus_substantially() -> None:
    """Document that two-ray residuals stay small while error can be large.

    With three Ears, quality gates must refuse 'useful' when loci disagree.
    """
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_stress_inconsistent_bearing.json"
    )
    scenario, published, nodes = _publish_scenario(scenario_path)
    clean = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    biased = apply_sensor_imperfections(
        published,
        ear_bearing_bias_deg={"SIM-EAR-002": 25.0},
        seed=13,
    )
    dirty = hub_event_track.run_cross_ear_correlation(
        biased,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(clean.events) == 1 and len(dirty.events) == 1
    clean_loc = clean.events[0].localization
    dirty_loc = dirty.events[0].localization
    assert clean_loc is not None and dirty_loc is not None
    assert dirty_loc.status != "useful"
    if clean_loc.latitude is not None and dirty_loc.latitude is not None:
        shift = haversine_m(
            clean_loc.latitude,
            clean_loc.longitude,
            dirty_loc.latitude,
            dirty_loc.longitude,
        )
        # 25° on one EAR is enough to move the consensus substantially, or
        # force withhold. Either outcome is acceptable; silent useful is not.
        assert shift > 50.0 or dirty_loc.details.get("position_withheld")
    # Pair residuals on forward intersections remain near zero — not accuracy.
    for pair in dirty_loc.pairs:
        if pair.intersection_status == "forward_intersection":
            if pair.residual_a_deg is not None:
                assert pair.residual_a_deg < 1.0
            if pair.residual_b_deg is not None:
                assert pair.residual_b_deg < 1.0
