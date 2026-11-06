#!/usr/bin/env python3
"""Post-hoc dense/sparse/stress Cross-EAR localization evaluation (simulation only).

Ground truth is never used for association or localization math.
Does not publish MQTT or persist Hub Events.
Dense and sparse results are reported separately; geometry classes are not pooled
into one accuracy number.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from lib.cross_ear_localization import (  # noqa: E402
    analyze_bearing_sensitivity,
    analyze_leave_one_ear_out,
    apply_sensor_imperfections,
    classify_network_geometry,
    evaluate_against_ground_truth,
    format_dilution_m,
    source_position_on_route,
)


def _load_hub():
    spec = spec_from_file_location(
        "dbmap_hub_event_track_eval", ROOT / "scripts" / "hub_event_track.py"
    )
    assert spec is not None and spec.loader is not None
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load_sim():
    spec = spec_from_file_location(
        "dbmap_sim_eval", ROOT / "simulator" / "ear" / "simulate_scenario.py"
    )
    assert spec is not None and spec.loader is not None
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ALIAS_TO_NODE = {
    "ear_a": "SIM-EAR-001",
    "ear_b": "SIM-EAR-002",
    "ear_c": "SIM-EAR-003",
    "ear_d": "SIM-EAR-004",
    "ear_e": "SIM-EAR-005",
}


@dataclass(frozen=True)
class ScenarioSpec:
    name: str
    path: Path
    network_label: str
    movement_label: str
    geometry_label: str
    allow_long_range_sim_ears: bool
    main_pass_before_utc: str | None = None
    geometry_class: str = "control"
    # Optional extra imperfection suites beyond the shared B2.2 set.
    extra_imperfections: tuple[tuple[Any, ...], ...] = ()


# B2.2 control scenarios (baseline; keep unchanged).
CONTROL_SCENARIOS = [
    ScenarioSpec(
        name="sparse_b1_moving",
        path=ROOT / "simulator/ear/scenarios/cross_ear_event_pass.json",
        network_label="sparse",
        movement_label="moving ~20 m/s",
        geometry_label="wide triangle ~1.7–2.3 km",
        allow_long_range_sim_ears=True,
        main_pass_before_utc="2026-11-05T18:02:55",
        geometry_class="control_sparse_triangle",
    ),
    ScenarioSpec(
        name="dense_good_moving",
        path=ROOT / "simulator/ear/scenarios/cross_ear_dense_pass.json",
        network_label="dense",
        movement_label="moving ~8 m/s",
        geometry_label="equilateral ~480 m",
        allow_long_range_sim_ears=False,
        main_pass_before_utc="2026-11-05T18:01:15",
        geometry_class="control_dense_triangle",
    ),
    ScenarioSpec(
        name="dense_stationary",
        path=ROOT / "simulator/ear/scenarios/cross_ear_dense_stationary.json",
        network_label="dense",
        movement_label="near-stationary ~0.1 m/s (5 m crawl)",
        geometry_label="equilateral ~480 m",
        allow_long_range_sim_ears=False,
        geometry_class="control_dense_stationary",
    ),
    ScenarioSpec(
        name="dense_poor_collinear",
        path=ROOT / "simulator/ear/scenarios/cross_ear_dense_poor_geometry.json",
        network_label="dense",
        movement_label="moving ~8 m/s",
        geometry_label="collinear EARs ~250 m spacing (poor bearing geometry)",
        allow_long_range_sim_ears=False,
        geometry_class="control_collinear",
    ),
    ScenarioSpec(
        name="dense_four_ear",
        path=ROOT / "simulator/ear/scenarios/cross_ear_dense_four_ear.json",
        network_label="dense",
        movement_label="moving ~10 m/s",
        geometry_label="~350 m square (4 Ears)",
        allow_long_range_sim_ears=False,
        geometry_class="control_four_ear",
    ),
]

# B2.3 geometry / failure-boundary stress scenarios.
STRESS_SCENARIOS = [
    ScenarioSpec(
        name="stress_nearly_parallel",
        path=ROOT / "simulator/ear/scenarios/cross_ear_stress_nearly_parallel.json",
        network_label="dense",
        movement_label="near-stationary far north of baseline",
        geometry_label="tight ~160 m baseline; LOBs nearly parallel",
        allow_long_range_sim_ears=False,
        geometry_class="nearly_parallel",
    ),
    ScenarioSpec(
        name="stress_near_baseline",
        path=ROOT / "simulator/ear/scenarios/cross_ear_stress_near_baseline.json",
        network_label="dense",
        movement_label="crawl ~15 m off baseline",
        geometry_label="collinear EARs; source close to baseline",
        allow_long_range_sim_ears=False,
        geometry_class="near_baseline",
    ),
    ScenarioSpec(
        name="stress_along_baseline",
        path=ROOT / "simulator/ear/scenarios/cross_ear_stress_along_baseline.json",
        network_label="dense",
        movement_label="moving ~8 m/s along baseline",
        geometry_label="collinear EARs; source travels along baseline",
        allow_long_range_sim_ears=False,
        geometry_class="along_baseline",
    ),
    ScenarioSpec(
        name="stress_far_source",
        path=ROOT / "simulator/ear/scenarios/cross_ear_stress_far_source.json",
        network_label="dense",
        movement_label="far source ~3 km from triangle",
        geometry_label="good ~480 m triangle; source ≫ EAR spacing",
        allow_long_range_sim_ears=False,
        geometry_class="far_source",
    ),
    ScenarioSpec(
        name="stress_poor_distribution",
        path=ROOT / "simulator/ear/scenarios/cross_ear_stress_poor_distribution.json",
        network_label="dense",
        movement_label="moving near clustered pair",
        geometry_label="two EARs clustered; third offset ~450 m",
        allow_long_range_sim_ears=False,
        geometry_class="poor_distribution",
    ),
    ScenarioSpec(
        name="stress_inconsistent_bearing",
        path=ROOT / "simulator/ear/scenarios/cross_ear_stress_inconsistent_bearing.json",
        network_label="dense",
        movement_label="near-stationary (good triangle control)",
        geometry_label="equilateral ~480 m; imperfection injects bad bearing",
        allow_long_range_sim_ears=False,
        geometry_class="bearing_outlier",
        extra_imperfections=(
            ("bias_ear_b_+25deg", {}, 0.0, 13, {"SIM-EAR-002": 25.0}, None, None),
            ("wrong_ear_b_+90deg", {}, 0.0, 13, {"SIM-EAR-002": 90.0}, None, None),
            ("drop_ear_b_bearing", {}, 0.0, 13, None, {"SIM-EAR-002"}, None),
            (
                "conflict_ear_b_+40deg",
                {},
                0.0,
                13,
                None,
                None,
                {"SIM-EAR-002": 40.0},
            ),
        ),
    ),
]

SCENARIOS = CONTROL_SCENARIOS + STRESS_SCENARIOS

# B2.4 deep-dive scenarios for bearing sensitivity + leave-one-out (baseline only).
B24_SENSITIVITY_SCENARIOS = {
    "dense_good_moving",
    "dense_stationary",
    "dense_four_ear",
    "sparse_b1_moving",
    "stress_nearly_parallel",
    "stress_far_source",
    "stress_poor_distribution",
}

# Shared B2.2 imperfection matrix (timing + noise). Tuple layout:
# (label, offsets, noise_deg, seed[, bias, drop_ids, conflicting])
IMPERFECTIONS = [
    ("baseline", {}, 0.0, 0),
    ("offset_0.1s", {"SIM-EAR-002": 0.1, "SIM-EAR-003": -0.05}, 0.0, 42),
    ("offset_0.5s", {"SIM-EAR-002": 0.5, "SIM-EAR-003": -0.25}, 0.0, 42),
    ("offset_1.0s", {"SIM-EAR-002": 1.0, "SIM-EAR-003": -0.5}, 0.0, 42),
    ("noise_1deg", {}, 1.0, 7),
    ("noise_3deg", {}, 3.0, 7),
    ("noise_5deg", {}, 5.0, 7),
    ("offset0.5_noise3", {"SIM-EAR-002": 0.5}, 3.0, 11),
]


def publish_scenario(sim, scenario: dict[str, Any]) -> tuple[list[dict], dict]:
    credentials = {}
    paths = {}
    for alias in scenario["ears"]:
        node_id = ALIAS_TO_NODE[alias]
        credentials[alias] = {"node_id": node_id, "next_sequence_number": 1}
        paths[alias] = Path(f"{alias}.json")
    published: list[dict] = []
    sim.run_scenario(
        scenario,
        credentials,
        paths,
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
        make_keepalive=lambda _c, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )
    nodes = {}
    for alias, ear in scenario["ears"].items():
        nodes[ALIAS_TO_NODE[alias]] = {
            "installation": {
                "latitude": ear["latitude"],
                "longitude": ear["longitude"],
                "orientation_deg": ear["orientation_deg"],
            }
        }
    return published, nodes


def main_pass(published: list[dict], cutoff: str | None) -> list[dict]:
    if cutoff is None:
        return published
    return [
        item
        for item in published
        if item["context"]["event_time_utc"] < cutoff
    ]


def _unpack_imperfection(item: tuple[Any, ...]) -> tuple:
    label = item[0]
    offsets = item[1]
    noise = item[2]
    seed = item[3]
    bias = item[4] if len(item) > 4 else None
    drop_ids = item[5] if len(item) > 5 else None
    conflicting = item[6] if len(item) > 6 else None
    return label, offsets, noise, seed, bias, drop_ids, conflicting


def evaluate_row(
    hub,
    *,
    scenario: dict[str, Any],
    payloads: list[dict],
    nodes: dict[str, dict],
    allow_long_range: bool,
) -> dict[str, Any]:
    result = hub.run_cross_ear_correlation(
        payloads,
        nodes,
        max_association_distance_m=500,
        allow_long_range_sim_ears=allow_long_range,
    )
    network_class, max_baseline, baselines = classify_network_geometry(nodes)
    row: dict[str, Any] = {
        "ears": len(scenario["ears"]),
        "baselines_m": [(a, b, round(d, 1)) for a, b, d in baselines],
        "max_baseline_m": None if max_baseline is None else round(max_baseline, 1),
        "network_class": network_class,
        "speed_m_s": scenario["movement"]["speed_m_s"],
        "event": len(result.events) > 0,
        "n_events": len(result.events),
        "loc_status": None,
        "est_lat": None,
        "est_lon": None,
        "gt_lat": None,
        "gt_lon": None,
        "eval_time": None,
        "error_m": None,
        "target_m": None,
        "within_target": None,
        "within_50m": None,
        "within_100m": None,
        "within_250m": None,
        "max_bearing_dt_s": None,
        "pairwise_spread_m": None,
        "best_intersection_angle_deg": None,
        "min_intersection_angle_deg": None,
        "outlier_pairs": None,
        "position_withheld": None,
        "network_geometric_sensitivity_m": None,
        "correlation_result": result,
    }
    if not result.events:
        return row
    event = result.events[0]
    loc = event.localization
    if loc is None:
        return row
    row["loc_status"] = loc.status
    row["max_bearing_dt_s"] = loc.max_bearing_dt_s
    row["pairwise_spread_m"] = loc.pairwise_spread_m
    row["est_lat"] = loc.latitude
    row["est_lon"] = loc.longitude
    row["best_intersection_angle_deg"] = loc.details.get("best_intersection_angle_deg")
    row["min_intersection_angle_deg"] = loc.details.get("min_intersection_angle_deg")
    row["outlier_pairs"] = loc.details.get("outlier_pairs")
    row["position_withheld"] = loc.details.get("position_withheld")
    row["network_geometric_sensitivity_m"] = loc.details.get(
        "network_geometric_sensitivity_m"
    )
    if loc.latitude is None or loc.evaluation_time_utc is None:
        return row
    started = datetime(2026, 11, 5, 18, 0, tzinfo=timezone.utc)
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
    row.update(
        {
            "gt_lat": gt_lat,
            "gt_lon": gt_lon,
            "eval_time": loc.evaluation_time_utc.isoformat(),
            "error_m": evaluation.error_m,
            "target_m": evaluation.target_m,
            "within_target": evaluation.within_target_m,
            "within_50m": evaluation.within_50m,
            "within_100m": evaluation.within_100m,
            "within_250m": evaluation.within_250m,
        }
    )
    return row


def pct(rows: list[dict[str, Any]], key: str) -> float | None:
    vals = [
        row[key]
        for row in rows
        if row.get("error_m") is not None and row.get(key) is not None
    ]
    if not vals:
        return None
    return 100.0 * sum(1 for value in vals if value) / len(vals)


def format_row(spec: ScenarioSpec, imperfection: str, row: dict[str, Any]) -> str:
    err = row["error_m"]
    err_s = "n/a" if err is None else f"{err:.1f}"
    est = (
        "none"
        if row["est_lat"] is None
        else f"{row['est_lat']:.6f},{row['est_lon']:.6f}"
    )
    gt = (
        "n/a"
        if row["gt_lat"] is None
        else f"{row['gt_lat']:.6f},{row['gt_lon']:.6f}"
    )
    angle = row.get("best_intersection_angle_deg")
    angle_s = "n/a" if angle is None else f"{angle:.1f}"
    withheld = row.get("position_withheld")
    sens = format_dilution_m(row.get("network_geometric_sensitivity_m"))
    return (
        f"{spec.name:28} {imperfection:22} ears={row['ears']} "
        f"max_base={row['max_baseline_m']} "
        f"move={spec.movement_label}; {spec.geometry_label} | "
        f"event={row['event']} loc={row['loc_status']} "
        f"dt={row['max_bearing_dt_s']} spread={row['pairwise_spread_m']} "
        f"best_ang={angle_s} sens={sens} withheld={withheld} "
        f"est={est} gt={gt} err_m={err_s} "
        f"tgt±{row['target_m']} within={row['within_target']} "
        f"bands50/100/250={row['within_50m']}/{row['within_100m']}/{row['within_250m']}"
    )


def _print_b24_diagnostics(
    *,
    spec: ScenarioSpec,
    nodes: dict[str, dict],
    baseline_row: dict[str, Any],
) -> None:
    """Bearing sensitivity + leave-one-out for the unperturbed baseline Event."""
    result = baseline_row.get("correlation_result")
    if result is None or not result.events:
        print("  B2.4 diagnostics: no Event on baseline — skipped")
        return
    event = result.events[0]
    loc = event.localization
    members = list(event.members)
    if loc is None:
        print("  B2.4 diagnostics: localization missing — skipped")
        return

    print("  -- B2.4 geometry sensitivity (baseline; not calibrated σ) --")
    print(
        f"     network sens indicator={format_dilution_m(loc.details.get('network_geometric_sensitivity_m'))}; "
        f"min_ang={loc.details.get('min_intersection_angle_deg')} "
        f"best_ang={loc.details.get('best_intersection_angle_deg')}"
    )
    for pair in loc.pairs:
        print(
            f"     pair {pair.ear_a_id}↔{pair.ear_b_id}: "
            f"status={pair.intersection_status} "
            f"baseline={pair.baseline_m if pair.baseline_m is not None else 'n/a'} "
            f"angle={pair.intersection_angle_deg} "
            f"dilution={format_dilution_m(pair.geometric_dilution_m)} "
            f"outlier={pair.outlier}"
        )

    sens = analyze_bearing_sensitivity(members, nodes, baseline=loc)
    print(
        f"  -- B2.4 bearing perturbations ±{list(sens.perturbations_deg)}° "
        f"(vs unperturbed baseline; association not re-run) --"
    )
    print(
        f"     baseline loc={sens.baseline_status} withheld={sens.baseline_position_withheld} "
        f"max_disp_m={sens.max_displacement_m}"
    )
    for trial in sens.trials:
        disp = (
            "n/a"
            if trial.displacement_from_baseline_m is None
            else f"{trial.displacement_from_baseline_m:.1f}"
        )
        print(
            f"     {trial.ear_id} δ={trial.perturbation_deg:+g}° → "
            f"loc={trial.status} withheld={trial.position_withheld} "
            f"disp_m={disp} best_ang={trial.best_intersection_angle_deg} "
            f"sens={format_dilution_m(trial.network_geometric_sensitivity_m)}"
        )
    for note in sens.notes:
        print(f"     note: {note}")

    layout = analyze_leave_one_ear_out(members, nodes, baseline=loc)
    print("  -- B2.4 leave-one-EAR-out (layout; GT unused) --")
    print(
        f"     full ears={layout.full_ear_ids} loc={layout.full_status} "
        f"withheld={layout.full_position_withheld} "
        f"sens={format_dilution_m(layout.full_network_geometric_sensitivity_m)}"
    )
    for trial in layout.leave_one_out:
        disp = (
            "n/a"
            if trial.displacement_from_full_m is None
            else f"{trial.displacement_from_full_m:.1f}"
        )
        print(
            f"     remove {trial.removed_ear_id}: remaining={trial.remaining_ears} "
            f"loc={trial.status} withheld={trial.position_withheld} "
            f"disp_m={disp} material={trial.material_geometry_loss} "
            f"({trial.notes})"
        )
    print(f"     critical={layout.critical_ear_ids} redundant={layout.redundant_ear_ids}")
    for note in layout.notes:
        print(f"     note: {note}")


def _summarize(label: str, rows: list[dict[str, Any]]) -> None:
    located = [row for row in rows if row.get("error_m") is not None]
    withheld = [
        row
        for row in rows
        if row.get("loc_status") == "unavailable"
        or row.get("position_withheld")
        or (row.get("event") and row.get("est_lat") is None)
    ]
    print(f"\n{label} n_cases={len(rows)} with_estimate={len(located)} "
          f"unavailable_or_withheld={len(withheld)}")
    if located:
        print(
            f"  <=50m={pct(located, 'within_50m'):.0f}%  "
            f"<=100m={pct(located, 'within_100m'):.0f}%  "
            f"<=250m={pct(located, 'within_250m'):.0f}%  "
            f"within_aspirational={pct(located, 'within_target'):.0f}%"
        )
        errors = [row["error_m"] for row in located]
        print(
            f"  error_m min/median/max = "
            f"{min(errors):.1f} / {sorted(errors)[len(errors)//2]:.1f} / {max(errors):.1f}"
        )
    status_counts: dict[str, int] = {}
    for row in rows:
        key = str(row.get("loc_status"))
        status_counts[key] = status_counts.get(key, 0) + 1
    print(f"  loc_status counts: {status_counts}")


def run() -> int:
    hub = _load_hub()
    sim = _load_sim()
    by_network: dict[str, list[dict[str, Any]]] = {"dense": [], "sparse": []}
    by_geometry: dict[str, list[dict[str, Any]]] = {}

    print("Cross-EAR localization evaluation (simulation only; GT post-hoc)")
    print("Bands ±50/100/250 m are aspirational, not guaranteed performance.")
    print("Geometry classes are summarized separately — do not pool accuracy.\n")

    for spec in SCENARIOS:
        scenario = sim.load_scenario(spec.path)
        published, nodes = publish_scenario(sim, scenario)
        payloads = main_pass(published, spec.main_pass_before_utc)
        print(f"=== {spec.name} ({spec.network_label} / {spec.geometry_class}) ===")
        print(f"  movement: {spec.movement_label}")
        print(f"  geometry: {spec.geometry_label}")
        print(f"  speed_m_s={scenario['movement']['speed_m_s']}")
        _cls, max_b, baselines = classify_network_geometry(nodes)
        print(f"  classified={_cls} max_baseline_m={max_b}")
        for first, second, distance in baselines:
            print(f"    {first} <-> {second}: {distance:.1f}m")

        suite = list(IMPERFECTIONS) + list(spec.extra_imperfections)
        baseline_row: dict[str, Any] | None = None
        for item in suite:
            label, offsets, noise, seed, bias, drop_ids, conflicting = _unpack_imperfection(
                item
            )
            imperfect = apply_sensor_imperfections(
                payloads,
                ear_time_offsets_s=offsets,
                bearing_noise_deg=noise,
                ear_bearing_bias_deg=bias,
                drop_bearing_node_ids=set(drop_ids) if drop_ids else None,
                conflicting_ear_bias_deg=conflicting,
                seed=seed,
            )
            row = evaluate_row(
                hub,
                scenario=scenario,
                payloads=imperfect,
                nodes=nodes,
                allow_long_range=spec.allow_long_range_sim_ears,
            )
            row["scenario"] = spec.name
            row["geometry_class"] = spec.geometry_class
            row["imperfection"] = label
            row["timing_offset"] = offsets
            row["bearing_noise_deg"] = noise
            row["ear_bearing_bias_deg"] = bias
            # Keep heavy objects off the summary aggregates.
            summary_row = {
                key: value
                for key, value in row.items()
                if key != "correlation_result"
            }
            by_network[spec.network_label].append(summary_row)
            by_geometry.setdefault(spec.geometry_class, []).append(summary_row)
            print(" ", format_row(spec, label, row))
            if label == "baseline":
                baseline_row = row
        if spec.name in B24_SENSITIVITY_SCENARIOS and baseline_row is not None:
            _print_b24_diagnostics(spec=spec, nodes=nodes, baseline_row=baseline_row)
        print()

    print("======== SUMMARY BY NETWORK (do not pool dense+sparse) ========")
    for network, rows in by_network.items():
        _summarize(network.upper(), rows)
        located = [row for row in rows if row.get("error_m") is not None]
        names = sorted({row["scenario"] for row in rows})
        for name in names:
            subset = [row for row in rows if row["scenario"] == name]
            located_s = [row for row in subset if row.get("error_m") is not None]
            med = (
                f"{sorted(r['error_m'] for r in located_s)[len(located_s)//2]:.1f}m"
                if located_s
                else "n/a"
            )
            print(
                f"  - {name}: n={len(subset)} located={len(located_s)} "
                f"<=50/100/250="
                f"{pct(located_s, 'within_50m') or 0:.0f}/"
                f"{pct(located_s, 'within_100m') or 0:.0f}/"
                f"{pct(located_s, 'within_250m') or 0:.0f}% "
                f"median_err={med}"
            )

    print("\n======== SUMMARY BY GEOMETRY CLASS (not pooled) ========")
    for geometry, rows in sorted(by_geometry.items()):
        _summarize(geometry, rows)

    return 0


def build_parser() -> argparse.ArgumentParser:
    return argparse.ArgumentParser(
        description=(
            "Evaluate dense vs sparse Cross-EAR localization scenarios "
            "(simulation GT is post-hoc only); includes B2.3 stress cases "
            "and B2.4 sensitivity / layout diagnostics"
        )
    )


if __name__ == "__main__":
    build_parser().parse_args()
    raise SystemExit(run())
