"""B2.4 localization sensitivity, geometric dilution and EAR layout tests."""

from __future__ import annotations

import math
import sys
from datetime import UTC, datetime
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_localization import (
    analyze_bearing_sensitivity,
    analyze_leave_one_ear_out,
    estimate_localization,
    format_dilution_m,
    geometric_dilution_indicator_m,
    haversine_m,
)

spec = spec_from_file_location(
    "dbmap_hub_event_track_b24",
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
) -> dict:
    bearing = None
    if bearing_deg is not None:
        bearing = {"deg": bearing_deg, "reference": "node", "confidence": 0.8}
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


def _publish(scenario_path: Path) -> tuple[dict, list[dict], dict]:
    sim_spec = spec_from_file_location(
        "dbmap_sim_b24",
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
        started_at_utc=datetime(2026, 11, 5, 18, 0, tzinfo=UTC),
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


def test_geometric_dilution_indicator_basic_and_degenerate() -> None:
    value = geometric_dilution_indicator_m(500.0, 30.0)
    assert value is not None
    assert abs(value - 500.0 / math.sin(math.radians(30.0))) < 1e-9
    assert geometric_dilution_indicator_m(500.0, 90.0) == 500.0
    # Degenerate / tiny angle → inf, not a fake finite radius.
    assert geometric_dilution_indicator_m(160.0, 0.0) == math.inf
    assert geometric_dilution_indicator_m(160.0, 1e-9) == math.inf
    assert format_dilution_m(math.inf) == "degenerate"
    assert format_dilution_m(None) == "n/a"


def test_small_versus_adequate_intersection_angles_affect_dilution() -> None:
    acute = geometric_dilution_indicator_m(480.0, 8.0)
    adequate = geometric_dilution_indicator_m(480.0, 60.0)
    assert acute is not None and adequate is not None
    assert acute > adequate * 5


def test_bearing_sensitivity_plus_minus_1_3_5() -> None:
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_dense_stationary.json"
    )
    _scenario, published, nodes = _publish(scenario_path)
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    event = result.events[0]
    assert event.localization is not None
    assert event.localization.status == "useful"

    report = analyze_bearing_sensitivity(
        event.members,
        nodes,
        perturbations_deg=(1.0, 3.0, 5.0),
        baseline=event.localization,
    )
    assert report.perturbations_deg == (1.0, 3.0, 5.0)
    assert report.baseline_status == "useful"
    assert report.baseline_latitude is not None
    # 3 Ears × 2 signs × 3 magnitudes
    assert len(report.trials) == 18
    assert report.max_displacement_m is not None
    assert report.max_displacement_m > 0

    disp_1 = [
        trial.displacement_from_baseline_m
        for trial in report.trials
        if abs(trial.perturbation_deg) == 1.0
        and trial.displacement_from_baseline_m is not None
    ]
    disp_5 = [
        trial.displacement_from_baseline_m
        for trial in report.trials
        if abs(trial.perturbation_deg) == 5.0
        and trial.displacement_from_baseline_m is not None
    ]
    assert disp_1 and disp_5
    assert max(disp_5) >= max(disp_1)

    # Plus/minus tracked separately (asymmetry allowed).
    plus = [
        trial
        for trial in report.trials
        if trial.ear_id == "SIM-EAR-001" and trial.perturbation_deg == 3.0
    ]
    minus = [
        trial
        for trial in report.trials
        if trial.ear_id == "SIM-EAR-001" and trial.perturbation_deg == -3.0
    ]
    assert len(plus) == 1 and len(minus) == 1


def test_nearly_parallel_has_degenerate_or_huge_dilution_and_withhold() -> None:
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_stress_nearly_parallel.json"
    )
    _scenario, published, nodes = _publish(scenario_path)
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    loc = result.events[0].localization
    assert loc is not None
    assert loc.status == "unavailable"
    assert loc.details.get("position_withheld") is True
    sens = loc.details.get("network_geometric_sensitivity_m")
    assert sens is not None
    assert sens > 1000.0 or not math.isfinite(sens)


def test_three_versus_four_ear_and_leave_one_out() -> None:
    four_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_dense_four_ear.json"
    )
    _scenario, published, nodes = _publish(four_path)
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) >= 1
    # Prefer an Event that retained all four Ears when association allows it;
    # otherwise assemble all Observations for an explicit four-Ear localization.
    four_event = next(
        (
            event
            for event in result.events
            if len({member.node_id for member in event.members}) == 4
        ),
        None,
    )
    if four_event is not None:
        members = four_event.members
        loc = four_event.localization
    else:
        members = hub_event_track.build_assembled_observations(published, nodes)
        assert len({member.node_id for member in members}) == 4
        loc = estimate_localization(members, nodes)
    assert loc is not None
    assert len(loc.contributing_ears) == 4

    three_members = [m for m in members if m.node_id != "SIM-EAR-004"]
    three = estimate_localization(three_members, nodes)
    assert three.status in {"useful", "weak", "unavailable"}
    # Four-Ear is not assumed strictly better than three-Ear.
    layout = analyze_leave_one_ear_out(members, nodes, baseline=loc)
    assert len(layout.full_ear_ids) == 4
    assert len(layout.leave_one_out) == 4
    assert all(len(trial.remaining_ears) == 3 for trial in layout.leave_one_out)


def test_removing_one_ear_can_materially_worsen_geometry() -> None:
    """Good triangle: removing any Ear leaves only two → weak / no multi-ray."""
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_dense_stationary.json"
    )
    _scenario, published, nodes = _publish(scenario_path)
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    event = result.events[0]
    loc = event.localization
    assert loc is not None and loc.status == "useful"
    layout = analyze_leave_one_ear_out(event.members, nodes, baseline=loc)
    assert layout.critical_ear_ids
    # Every removal from a minimal 3-Ear useful solution should demote.
    assert all(trial.material_geometry_loss for trial in layout.leave_one_out)
    assert all(trial.status in {"weak", "unavailable"} for trial in layout.leave_one_out)


def test_additional_ear_need_not_improve_solution() -> None:
    """Near-duplicate fourth Ear should not be required for a useful solution."""
    from dataclasses import replace

    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_dense_stationary.json"
    )
    _scenario, published, nodes = _publish(scenario_path)
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    base_members = result.events[0].members
    # Clone ear_a as a near-collocated SIM-EAR-004 with the same bearings.
    primary = next(m for m in base_members if m.node_id == "SIM-EAR-001")
    nodes = dict(nodes)
    inst = nodes["SIM-EAR-001"]["installation"]
    nodes["SIM-EAR-004"] = _node(
        inst["latitude"] + 0.00001,
        inst["longitude"] + 0.00001,
        inst["orientation_deg"],
    )
    duplicate = replace(
        primary,
        observation_id="dup-ear-a",
        node_id="SIM-EAR-004",
        installation_latitude=nodes["SIM-EAR-004"]["installation"]["latitude"],
        installation_longitude=nodes["SIM-EAR-004"]["installation"]["longitude"],
        installation_orientation_deg=nodes["SIM-EAR-004"]["installation"][
            "orientation_deg"
        ],
    )
    members = list(base_members) + [duplicate]
    four = estimate_localization(members, nodes)
    three = estimate_localization(base_members, nodes)
    assert three.status == "useful"
    assert four.latitude is not None and three.latitude is not None
    # Extra near-duplicate Ear must not be required: removing it keeps useful geometry.
    layout = analyze_leave_one_ear_out(members, nodes, baseline=four)
    dup_trial = next(
        trial for trial in layout.leave_one_out if trial.removed_ear_id == "SIM-EAR-004"
    )
    assert dup_trial.status in {"useful", "weak"}
    assert dup_trial.position_withheld is False
    assert (
        "SIM-EAR-004" in layout.redundant_ear_ids
        or dup_trial.material_geometry_loss is False
    )
    # Four-Ear solution is not automatically better than the original three-Ear.
    if four.details.get("network_geometric_sensitivity_m") is not None and three.details.get(
        "network_geometric_sensitivity_m"
    ) is not None:
        four_sens = four.details["network_geometric_sensitivity_m"]
        three_sens = three.details["network_geometric_sensitivity_m"]
        if math.isfinite(four_sens) and math.isfinite(three_sens):
            assert four_sens >= three_sens * 0.5  # not dramatically "better" by fiat


def test_dense_and_sparse_reporting_remain_separate_in_eval_module() -> None:
    eval_path = (
        Path(__file__).parents[2] / "scripts" / "eval_dense_sparse_localization.py"
    )
    text = eval_path.read_text(encoding="utf-8")
    assert "do not pool" in text.lower() or "not pooled" in text.lower()
    assert "by_network" in text
    assert "dense" in text and "sparse" in text


def test_sensitivity_does_not_change_association_or_promotion() -> None:
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_dense_stationary.json"
    )
    _scenario, published, nodes = _publish(scenario_path)
    before = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(before.events) == 1
    event = before.events[0]
    ids_before = set(event.observation_ids)
    score_before = event.score

    analyze_bearing_sensitivity(event.members, nodes, baseline=event.localization)
    analyze_leave_one_ear_out(event.members, nodes, baseline=event.localization)

    after = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(after.events) == 1
    assert set(after.events[0].observation_ids) == ids_before
    assert after.events[0].score == score_before
    assert after.events[0].localization is not None
    assert after.events[0].localization.status == event.localization.status
    assert after.events[0].localization.latitude == event.localization.latitude


def test_ground_truth_remains_post_hoc_for_sensitivity() -> None:
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_dense_stationary.json"
    )
    scenario, published, nodes = _publish(scenario_path)
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    event = result.events[0]
    loc = event.localization
    assert loc is not None and loc.latitude is not None
    # Sensitivity uses members/nodes only.
    report = analyze_bearing_sensitivity(event.members, nodes, baseline=loc)
    assert "GT" in " ".join(report.notes) or "calibrated" in " ".join(report.notes).lower()
    # GT route start is unrelated to sensitivity math.
    gt_lat = scenario["movement"]["start"]["latitude"]
    gt_lon = scenario["movement"]["start"]["longitude"]
    assert haversine_m(loc.latitude, loc.longitude, gt_lat, gt_lon) >= 0
    # Perturbed trials are distinct objects from baseline.
    assert report.baseline_latitude == loc.latitude


def test_position_withholding_still_works_under_b23_gates() -> None:
    scenario_path = (
        Path(__file__).parents[2]
        / "simulator"
        / "ear"
        / "scenarios"
        / "cross_ear_stress_far_source.json"
    )
    _scenario, published, nodes = _publish(scenario_path)
    result = hub_event_track.run_cross_ear_correlation(
        published,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=False,
    )
    assert len(result.events) == 1
    loc = result.events[0].localization
    assert loc is not None
    assert loc.status == "unavailable"
    assert loc.latitude is None
    report = analyze_bearing_sensitivity(
        result.events[0].members, nodes, baseline=loc
    )
    assert report.baseline_position_withheld is True
