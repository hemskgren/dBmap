"""Phase 5C Tier 2: segmented motion scenarios + offline temporal evaluation.

Scenario authoring/evaluation only. Does not modify movement implementation,
association, localization, or thresholds.

Artifacts (gitignored): ``shared/results/ear-tests/phase5c-tier2/``.
Run ``python tests/scripts/test_phase5c_tier2_motion_scenarios.py`` to refresh
``REPORT.md`` / ``results.json``.
"""

from __future__ import annotations

import contextlib
import inspect
import io
import json
import math
import random
import sys
from datetime import UTC, datetime
from importlib.util import module_from_spec, spec_from_file_location
from itertools import pairwise
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_localization import evaluate_against_ground_truth
from lib.localization_epoch_diagnostic import evaluate_event_localization_epochs
from lib.scenario_ground_truth import ground_truth_from_loaded_scenario
from lib.scenario_movement import CONTINUITY_TOLERANCE_M, distance_m, position_at

_ROOT = Path(__file__).parents[2]
_SCENARIOS = _ROOT / "simulator" / "ear" / "scenarios"
_RESULTS = _ROOT / "shared" / "results" / "ear-tests" / "phase5c-tier2"
_ALIAS_TO_NODE = {
    "ear_a": "SIM-EAR-001",
    "ear_b": "SIM-EAR-002",
    "ear_d": "SIM-EAR-004",
    "ear_e": "SIM-EAR-005",
}
_ALL = set(_ALIAS_TO_NODE.values())
_START = datetime(2026, 11, 7, 17, 0, tzinfo=UTC)
_ABDE = _SCENARIOS / "helicopter_abde_overlap.json"

_SCENARIO_FILES = {
    "ACC": _SCENARIOS / "phase5c_acceleration.json",
    "DEC": _SCENARIOS / "phase5c_deceleration.json",
    "HOVER": _SCENARIOS / "phase5c_hover.json",
    "STOP": _SCENARIOS / "phase5c_stop_and_go.json",
    "TURN-G": _SCENARIOS / "phase5c_turn_gentle.json",
    "TURN-P": _SCENARIOS / "phase5c_turn_sharp.json",
}


def _load(name: str, path: Path):
    spec = spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _bearing_deg(a: dict[str, float], b: dict[str, float]) -> float:
    lat1 = math.radians(a["latitude"])
    lat2 = math.radians(b["latitude"])
    lon_delta = math.radians(b["longitude"] - a["longitude"])
    y = math.sin(lon_delta) * math.cos(lat2)
    x = math.cos(lat1) * math.sin(lat2) - math.sin(lat1) * math.cos(lat2) * math.cos(
        lon_delta
    )
    return math.degrees(math.atan2(y, x)) % 360


def _heading_delta(a: float, b: float) -> float:
    return (b - a + 180.0) % 360.0 - 180.0


def _assert_no_gt_in_reports(reports: list[dict]) -> None:
    for payload in reports:
        spatial = payload.get("spatial") or {}
        assert spatial.get("position") is None
        assert "ground_truth" not in payload
        assert "ground_truth_id" not in (payload.get("identity") or {})
        hints = (
            ((payload.get("acoustic") or {}).get("classification") or {}).get("hints")
            or {}
        )
        assert "ground_truth" not in hints
        assert hints.get("route") is None


def _run(
    scenario_path: Path,
    *,
    reports: list[dict] | None = None,
    reverse: bool = False,
    shuffle_seed: int | None = None,
):
    """Fresh isolated offline publish buffer per call (unless reports injected)."""
    sim = _load(f"sim_{scenario_path.stem}", _ROOT / "simulator/ear/simulate_scenario.py")
    hub = _load(f"hub_{scenario_path.stem}", _ROOT / "scripts/hub_event_track.py")
    scenario = sim.load_scenario(scenario_path)
    credentials = {
        alias: {"node_id": node_id, "next_sequence_number": 1}
        for alias, node_id in _ALIAS_TO_NODE.items()
        if alias in scenario["ears"]
    }
    published: list[dict] = []
    if reports is None:
        with contextlib.redirect_stdout(io.StringIO()):
            sim.run_scenario(
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
                started_at_utc=_START,
                started_at_monotonic_us=0,
                save=lambda *_a, **_k: None,
                publish_keepalive=lambda *_a, **_k: None,
                make_keepalive=lambda _c, uptime_s: {"uptime_s": uptime_s},
                announce=False,
            )
        reports = published
    else:
        reports = list(reports)
    if reverse:
        reports = list(reversed(reports))
    if shuffle_seed is not None:
        rng = random.Random(shuffle_seed)
        reports = list(reports)
        rng.shuffle(reports)

    nodes = {
        _ALIAS_TO_NODE[alias]: {
            "installation": {
                "latitude": ear["latitude"],
                "longitude": ear["longitude"],
                "orientation_deg": ear["orientation_deg"],
            }
        }
        for alias, ear in scenario["ears"].items()
    }
    members = hub.build_assembled_observations(reports, nodes)
    corr = hub.run_cross_ear_correlation(
        reports, nodes, allow_long_range_sim_ears=True
    )
    return scenario, reports, members, nodes, corr


def _temporal_snapshot(corr, nodes):
    assert len(corr.events) == 1
    event = corr.events[0]
    before_id = id(event.localization)
    report = evaluate_event_localization_epochs(event, nodes)
    assert id(event.localization) == before_id
    assert report.production_localization is event.localization
    return event, report


def _segment_summary(movement: dict[str, Any]) -> list[dict[str, Any]]:
    rows = []
    for seg in movement["segments"]:
        row: dict[str, Any] = {
            "type": seg["type"],
            "distance_m": float(seg["distance_m"]),
            "duration_seconds": float(seg["duration_seconds"]),
        }
        if seg["type"] == "transit":
            row["speed_m_s"] = float(seg["speed_m_s"])
            row["start"] = dict(seg["start"])
            row["end"] = dict(seg["end"])
        else:
            row["position"] = dict(seg["position"])
            row["duration_s"] = float(seg["duration_s"])
        rows.append(row)
    return rows


def _evaluate_label(label: str, path: Path) -> dict[str, Any]:
    scenario, reports, members, nodes, corr = _run(path)
    _assert_no_gt_in_reports(reports)
    movement = scenario["movement"]
    gt = ground_truth_from_loaded_scenario(scenario)

    event_promoted = len(corr.events) == 1
    result: dict[str, Any] = {
        "label": label,
        "scenario_id": scenario["scenario_id"],
        "path": str(path.relative_to(_ROOT)),
        "movement_form": movement["form"],
        "segments": _segment_summary(movement),
        "total_distance_m": float(movement["distance_m"]),
        "total_duration_seconds": float(movement["duration_seconds"]),
        "report_count": len(reports),
        "assembled_observation_count": len(members),
        "event_promoted": event_promoted,
        "candidate_count": len(getattr(corr, "candidates", []) or []),
        "rejection_notes": list(getattr(corr, "rejection_notes", []) or []),
    }

    if not event_promoted:
        result["event_member_ear_count"] = 0
        result["temporal"] = None
        result["ground_truth_post_hoc"] = None
        result["sim_gt_parity"] = []
        return result

    event, report = _temporal_snapshot(corr, nodes)
    result["event_member_ear_count"] = len(event.ear_ids)
    result["event_member_ear_ids"] = sorted(event.ear_ids)

    discovered = len(report.discovered_epochs)
    evaluated = sum(1 for row in report.epochs if row.evaluated)
    unavailable = sum(1 for row in report.epochs if row.unavailable)
    epoch_rows = []
    errors: list[float] = []
    parity: list[dict[str, Any]] = []
    for row in report.epochs:
        t = (row.epoch_utc - _START).total_seconds()
        gt_lat, gt_lon = gt.position_at(t)
        sim_pos = position_at(movement, t)
        parity.append(
            {
                "t_s": t,
                "sim": sim_pos,
                "gt": {"latitude": gt_lat, "longitude": gt_lon},
                "agree_m": distance_m(
                    sim_pos, {"latitude": gt_lat, "longitude": gt_lon}
                ),
            }
        )
        err = None
        if row.localization is not None and row.latitude is not None:
            post = evaluate_against_ground_truth(
                row.localization,
                ground_truth_latitude=gt_lat,
                ground_truth_longitude=gt_lon,
                ground_truth_time_utc=row.epoch_utc,
            )
            err = post.error_m
            if err is not None:
                errors.append(float(err))
        epoch_rows.append(
            {
                "t_s": t,
                "status": row.status,
                "evaluated": row.evaluated,
                "unavailable": row.unavailable,
                "unavailable_reason": row.unavailable_reason,
                "participating_ear_ids": list(row.participating_ear_ids),
                "contributing_ears": list(row.contributing_ears),
                "forward_pair_count": row.forward_pair_count,
                "aligned_forward_pair_count": row.aligned_forward_pair_count,
                "outlier_pairs": list(row.outlier_pairs),
                "network_geometric_sensitivity_m": row.network_geometric_sensitivity_m,
                "latitude": row.latitude,
                "longitude": row.longitude,
                "gt_latitude": gt_lat,
                "gt_longitude": gt_lon,
                "gt_error_m_post_hoc": err,
            }
        )

    prod = report.production_localization
    prod_epoch = report.production_common_epoch_utc
    prod_t = (
        None
        if prod_epoch is None
        else (prod_epoch - _START).total_seconds()
    )
    result["temporal"] = {
        "discovered_epochs": discovered,
        "evaluated_epochs": evaluated,
        "unavailable_epochs": unavailable,
        "epochs": epoch_rows,
        "production_status": None if prod is None else prod.status,
        "production_epoch_t_s": prod_t,
        "production_contributing_ears": (
            None if prod is None else list(prod.contributing_ears)
        ),
        "production_latitude": None if prod is None else prod.latitude,
        "production_longitude": None if prod is None else prod.longitude,
    }
    result["ground_truth_post_hoc"] = {
        "label": (
            "Post-hoc simulator evidence only — not physical accuracy or "
            "calibrated uncertainty."
        ),
        "errors_m": errors,
        "median_error_m": (
            None if not errors else float(sorted(errors)[len(errors) // 2])
        ),
        "max_error_m": None if not errors else float(max(errors)),
        "min_error_m": None if not errors else float(min(errors)),
    }
    result["sim_gt_parity"] = parity

    # Motion-specific derived checks
    segs = movement["segments"]
    if label in {"ACC", "DEC"}:
        boundaries = []
        cursor = 0.0
        for seg in segs:
            cursor += float(seg["duration_seconds"])
            boundaries.append(
                {
                    "t_s": cursor,
                    "speed_m_s": float(seg["speed_m_s"]),
                    "position": position_at(movement, min(cursor, movement["duration_seconds"])),
                }
            )
        result["motion_checks"] = {
            "piecewise_constant_speeds": [float(s["speed_m_s"]) for s in segs],
            "note": (
                "Piecewise constant-speed transit approximation — not continuous "
                "acceleration/deceleration."
            ),
            "boundaries": boundaries,
        }
    elif label == "HOVER":
        samples = [
            position_at(movement, t)
            for t in (0.0, 10.0, 20.0, 30.0, float(movement["duration_seconds"]))
        ]
        result["motion_checks"] = {
            "constant_hold": all(
                distance_m(samples[0], p) <= CONTINUITY_TOLERANCE_M for p in samples
            ),
            "samples": samples,
        }
    elif label == "STOP":
        d0 = float(segs[0]["duration_seconds"])
        hold = float(segs[1]["duration_seconds"])
        hold_samples = [
            position_at(movement, t) for t in (d0, d0 + hold / 2, d0 + hold)
        ]
        after = position_at(
            movement, min(d0 + hold + 5.0, float(movement["duration_seconds"]))
        )
        result["motion_checks"] = {
            "hold_duration_s": hold,
            "hold_constant": all(
                distance_m(hold_samples[0], p) <= CONTINUITY_TOLERANCE_M
                for p in hold_samples
            ),
            "resumes_after_hold": distance_m(hold_samples[-1], after) > 1.0,
            "hold_position": hold_samples[0],
            "after_hold_position": after,
        }
    elif label in {"TURN-G", "TURN-P"}:
        br1 = _bearing_deg(segs[0]["start"], segs[0]["end"])
        br2 = _bearing_deg(segs[1]["start"], segs[1]["end"])
        delta = _heading_delta(br1, br2)
        result["motion_checks"] = {
            "leg1_bearing_deg": br1,
            "leg2_bearing_deg": br2,
            "heading_delta_deg": delta,
        }
    return result


def evaluate_all() -> dict[str, Any]:
    scenarios = {
        label: _evaluate_label(label, path)
        for label, path in _SCENARIO_FILES.items()
    }
    return {
        "generated_at_utc": datetime.now(UTC).isoformat(),
        "started_at_utc": _START.isoformat(),
        "analysis": {
            "allow_long_range_sim_ears": True,
            "common_epoch_path": "evaluate_event_localization_epochs",
            "ground_truth": "post-hoc only via scenario_ground_truth",
        },
        "scenarios": scenarios,
    }


def _md_escape(value: Any) -> str:
    return str(value)


def write_artifacts(bundle: dict[str, Any] | None = None) -> Path:
    bundle = bundle or evaluate_all()
    _RESULTS.mkdir(parents=True, exist_ok=True)
    (_RESULTS / "results.json").write_text(
        json.dumps(bundle, indent=2) + "\n", encoding="utf-8"
    )
    lines = [
        "# Phase 5C Tier 2 – Motion Scenario Evaluation",
        "",
        (
            "Offline simulator evidence only. Ground-truth errors are **post-hoc** "
            "and are not physical accuracy or calibrated uncertainty."
        ),
        "",
        f"Generated: `{bundle['generated_at_utc']}`",
        "",
        "## Scenario definitions",
        "",
        "| Label | File | Segments | Distance (m) | Duration (s) |",
        "| --- | --- | --- | ---: | ---: |",
    ]
    for label, row in bundle["scenarios"].items():
        seg_txt = "; ".join(
            (
                f"{s['type']}@{s['speed_m_s']:g}m/s/{s['duration_seconds']:.3f}s"
                if s["type"] == "transit"
                else f"hold/{s['duration_seconds']:.3f}s"
            )
            for s in row["segments"]
        )
        lines.append(
            f"| {label} | `{Path(row['path']).name}` | {seg_txt} | "
            f"{row['total_distance_m']:.3f} | {row['total_duration_seconds']:.6f} |"
        )

    tg = bundle["scenarios"]["TURN-G"]["motion_checks"]
    tp = bundle["scenarios"]["TURN-P"]["motion_checks"]
    lines += [
        "",
        "## Turn bearings (calculated)",
        "",
        (
            f"- **TURN-G:** leg1={tg['leg1_bearing_deg']:.6f}°, "
            f"leg2={tg['leg2_bearing_deg']:.6f}°, "
            f"Δheading={tg['heading_delta_deg']:.6f}° (target 20–40°)"
        ),
        (
            f"- **TURN-P:** leg1={tp['leg1_bearing_deg']:.6f}°, "
            f"leg2={tp['leg2_bearing_deg']:.6f}°, "
            f"Δheading={tp['heading_delta_deg']:.6f}° (target ≥90°)"
        ),
        "",
        "## Association / Event",
        "",
        "| Label | Event? | Member EARs | Assembled Obs | Reports |",
        "| --- | --- | ---: | ---: | ---: |",
    ]
    for label, row in bundle["scenarios"].items():
        lines.append(
            f"| {label} | {row['event_promoted']} | "
            f"{row['event_member_ear_count']} | "
            f"{row['assembled_observation_count']} | {row['report_count']} |"
        )

    lines += ["", "## Temporal localization", ""]
    for label, row in bundle["scenarios"].items():
        lines.append(f"### {label} (`{row['scenario_id']}`)")
        lines.append("")
        temporal = row["temporal"]
        if temporal is None:
            lines.append("No Event promoted; temporal diagnostic not applicable.")
            lines.append("")
            continue
        lines.append(
            f"- discovered={temporal['discovered_epochs']} "
            f"evaluated={temporal['evaluated_epochs']} "
            f"unavailable={temporal['unavailable_epochs']}"
        )
        lines.append(
            f"- production: status={temporal['production_status']} "
            f"t={temporal['production_epoch_t_s']} "
            f"ears={temporal['production_contributing_ears']}"
        )
        lines.append("")
        lines.append(
            "| t (s) | status | contrib | fwd | outliers | sens (m) | "
            "GT err (m, post-hoc) |"
        )
        lines.append("| ---: | --- | --- | ---: | --- | ---: | ---: |")
        for epoch in temporal["epochs"]:
            lines.append(
                f"| {epoch['t_s']:g} | {epoch['status']} | "
                f"{len(epoch['contributing_ears'])} | "
                f"{epoch['forward_pair_count']} | "
                f"{epoch['outlier_pairs'] or '—'} | "
                f"{_md_escape(epoch['network_geometric_sensitivity_m'])} | "
                f"{_md_escape(epoch['gt_error_m_post_hoc'])} |"
            )
        gt = row["ground_truth_post_hoc"]
        lines.append("")
        lines.append(
            f"- GT error summary (post-hoc): "
            f"min={gt['min_error_m']} median={gt['median_error_m']} "
            f"max={gt['max_error_m']}"
        )
        lines.append("")

    lines += [
        "## Motion-specific notes",
        "",
        (
            "- ACC/DEC: piecewise constant speeds on equal ~400 m thirds of the ABDE "
            "eastbound corridor (not continuous accel/decel curves)."
        ),
        "- HOVER: single 40 s hold at corridor midpoint; no crawl.",
        "- STOP: transit → 15 s hold → transit; continuity at both joins.",
        "- TURN-G/TURN-P: bearings computed from spherical leg endpoints.",
        "",
        "## Reproduction",
        "",
        "```bash",
        (
            "uv run --project hub/bootstrap python "
            "tests/scripts/test_phase5c_tier2_motion_scenarios.py"
        ),
        "```",
        "",
    ]
    (_RESULTS / "REPORT.md").write_text("\n".join(lines), encoding="utf-8")
    runner = """#!/usr/bin/env python3
\"\"\"Refresh Phase 5C Tier 2 REPORT.md / results.json (gitignored artifacts).\"\"\"
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[4]
cmd = [
    "uv",
    "run",
    "--project",
    "hub/bootstrap",
    "python",
    str(_ROOT / "tests/scripts/test_phase5c_tier2_motion_scenarios.py"),
]
raise SystemExit(subprocess.call(cmd, cwd=_ROOT))
"""
    (_RESULTS / "run_phase5c_tier2_eval.py").write_text(runner, encoding="utf-8")
    return _RESULTS


# --- Deterministic tests ---------------------------------------------------


@pytest.mark.parametrize("label,path", list(_SCENARIO_FILES.items()))
def test_tier2_loads_as_segmented(label: str, path: Path) -> None:
    sim = _load(f"load_{path.stem}", _ROOT / "simulator/ear/simulate_scenario.py")
    scenario = sim.load_scenario(path)
    assert scenario["movement"]["form"] == "segments"
    assert scenario["movement"]["speed_m_s"] is None
    assert scenario["ears"].keys() == {"ear_a", "ear_b", "ear_d", "ear_e"}
    abde = sim.load_scenario(_ABDE)
    assert scenario["ears"] == abde["ears"]


def test_acc_dec_segment_speeds_and_shared_distances() -> None:
    sim = _load("accdec", _ROOT / "simulator/ear/simulate_scenario.py")
    acc = sim.load_scenario(_SCENARIO_FILES["ACC"])
    dec = sim.load_scenario(_SCENARIO_FILES["DEC"])
    assert [s["speed_m_s"] for s in acc["movement"]["segments"]] == [10.0, 30.0, 50.0]
    assert [s["speed_m_s"] for s in dec["movement"]["segments"]] == [50.0, 30.0, 10.0]
    for a, d in zip(acc["movement"]["segments"], dec["movement"]["segments"]):
        assert a["distance_m"] == pytest.approx(d["distance_m"], abs=1e-6)
        assert a["distance_m"] == pytest.approx(400.0, abs=1e-3)
        assert a["start"] == d["start"]
        assert a["end"] == d["end"]
    assert acc["movement"]["duration_seconds"] == pytest.approx(61.333333, abs=1e-4)
    assert dec["movement"]["duration_seconds"] == pytest.approx(61.333333, abs=1e-4)


def test_hover_is_pure_hold() -> None:
    sim = _load("hover", _ROOT / "simulator/ear/simulate_scenario.py")
    scenario = sim.load_scenario(_SCENARIO_FILES["HOVER"])
    segs = scenario["movement"]["segments"]
    assert len(segs) == 1
    assert segs[0]["type"] == "hold"
    assert segs[0]["duration_s"] == 40.0
    assert scenario["movement"]["distance_m"] == 0.0
    pos0 = position_at(scenario["movement"], 0.0)
    for t in (0.0, 10.0, 20.0, 30.0, 40.0):
        assert distance_m(pos0, position_at(scenario["movement"], t)) <= CONTINUITY_TOLERANCE_M


def test_stop_and_go_hold_and_resume() -> None:
    sim = _load("stop", _ROOT / "simulator/ear/simulate_scenario.py")
    scenario = sim.load_scenario(_SCENARIO_FILES["STOP"])
    segs = scenario["movement"]["segments"]
    assert [s["type"] for s in segs] == ["transit", "hold", "transit"]
    assert segs[1]["duration_s"] == 15.0
    # Continuity at joins
    assert distance_m(segs[0]["end"], segs[1]["position"]) <= CONTINUITY_TOLERANCE_M
    assert distance_m(segs[1]["position"], segs[2]["start"]) <= CONTINUITY_TOLERANCE_M
    d0 = float(segs[0]["duration_seconds"])
    hold = float(segs[1]["duration_seconds"])
    hold_pos = position_at(scenario["movement"], d0 + hold / 2)
    assert distance_m(hold_pos, segs[1]["position"]) <= CONTINUITY_TOLERANCE_M
    after = position_at(scenario["movement"], d0 + hold + 5.0)
    assert distance_m(hold_pos, after) > 1.0


def test_turn_angles_in_requested_ranges() -> None:
    sim = _load("turns", _ROOT / "simulator/ear/simulate_scenario.py")
    gentle = sim.load_scenario(_SCENARIO_FILES["TURN-G"])
    sharp = sim.load_scenario(_SCENARIO_FILES["TURN-P"])
    for scenario, low, high in (
        (gentle, 20.0, 40.0),
        (sharp, 90.0, 180.0),
    ):
        s0, s1 = scenario["movement"]["segments"]
        assert s0["distance_m"] > 0 and s1["distance_m"] > 0
        br1 = _bearing_deg(s0["start"], s0["end"])
        br2 = _bearing_deg(s1["start"], s1["end"])
        delta = abs(_heading_delta(br1, br2))
        assert low - 1e-6 <= delta <= high + 1e-6
    # Exact documented values for these authored routes
    g0, g1 = gentle["movement"]["segments"]
    assert _heading_delta(
        _bearing_deg(g0["start"], g0["end"]), _bearing_deg(g1["start"], g1["end"])
    ) == pytest.approx(30.0, abs=1e-6)
    p0, p1 = sharp["movement"]["segments"]
    assert _heading_delta(
        _bearing_deg(p0["start"], p0["end"]), _bearing_deg(p1["start"], p1["end"])
    ) == pytest.approx(90.0, abs=1e-6)


@pytest.mark.parametrize("label,path", list(_SCENARIO_FILES.items()))
def test_continuous_joins_and_boundary_positions(label: str, path: Path) -> None:
    sim = _load(f"join_{path.stem}", _ROOT / "simulator/ear/simulate_scenario.py")
    scenario = sim.load_scenario(path)
    segs = scenario["movement"]["segments"]
    for prev, cur in pairwise(segs):
        exit_pt = prev["end"] if prev["type"] == "transit" else prev["position"]
        entry_pt = cur["start"] if cur["type"] == "transit" else cur["position"]
        assert distance_m(exit_pt, entry_pt) <= CONTINUITY_TOLERANCE_M
    # Boundary positions
    cursor = 0.0
    for seg in segs:
        start_pos = position_at(scenario["movement"], cursor)
        if seg["type"] == "transit":
            assert distance_m(start_pos, seg["start"]) <= CONTINUITY_TOLERANCE_M
        else:
            assert distance_m(start_pos, seg["position"]) <= CONTINUITY_TOLERANCE_M
        cursor += float(seg["duration_seconds"])
        end_pos = position_at(
            scenario["movement"], min(cursor, scenario["movement"]["duration_seconds"])
        )
        if seg["type"] == "transit":
            assert distance_m(end_pos, seg["end"]) <= CONTINUITY_TOLERANCE_M
        else:
            assert distance_m(end_pos, seg["position"]) <= CONTINUITY_TOLERANCE_M


@pytest.mark.parametrize("label,path", list(_SCENARIO_FILES.items()))
def test_sim_gt_parity_and_gt_isolation(label: str, path: Path) -> None:
    scenario, reports, _members, nodes, corr = _run(path)
    _assert_no_gt_in_reports(reports)
    params = inspect.signature(evaluate_event_localization_epochs).parameters
    assert "ground_truth" not in params
    gt = ground_truth_from_loaded_scenario(scenario)
    assert gt.speed_m_s is None
    for t in sorted({float(item["at_seconds"]) for item in scenario["observations"]}):
        sim_pos = position_at(scenario["movement"], t)
        glat, glon = gt.position_at(t)
        assert distance_m(sim_pos, {"latitude": glat, "longitude": glon}) < 1e-6
    assert len(corr.events) == 1
    _event, report = _temporal_snapshot(corr, nodes)
    for row in report.epochs:
        if row.localization is None or row.latitude is None:
            continue
        t = (row.epoch_utc - _START).total_seconds()
        glat, glon = gt.position_at(t)
        post = evaluate_against_ground_truth(
            row.localization,
            ground_truth_latitude=glat,
            ground_truth_longitude=glon,
            ground_truth_time_utc=row.epoch_utc,
        )
        assert post.error_m is not None


@pytest.mark.parametrize("label,path", list(_SCENARIO_FILES.items()))
def test_offline_eval_deterministic(label: str, path: Path) -> None:
    _scenario, base_reports, _m, nodes, corr0 = _run(path)
    _e0, r0 = _temporal_snapshot(corr0, nodes)

    def key(report):
        return (
            tuple(report.discovered_epochs),
            tuple(row.status for row in report.epochs),
            tuple(row.contributing_ears for row in report.epochs),
            tuple(
                (
                    None if row.latitude is None else round(row.latitude, 7),
                    None if row.longitude is None else round(row.longitude, 7),
                )
                for row in report.epochs
            ),
        )

    base = key(r0)
    for kwargs in ({"reverse": True}, {"shuffle_seed": 7}):
        _s, _r, _m, nodes_i, corr_i = _run(path, reports=base_reports, **kwargs)
        _e, rep = _temporal_snapshot(corr_i, nodes_i)
        assert key(rep) == base


def test_no_cross_scenario_report_contamination() -> None:
    buffers: dict[str, set[str]] = {}
    for label, path in _SCENARIO_FILES.items():
        _s, reports, _m, _n, _c = _run(path)
        ids = {item["identity"]["observation_id"] for item in reports}
        buffers[label] = ids
        assert ids
    labels = list(buffers)
    for i, a in enumerate(labels):
        for b in labels[i + 1 :]:
            assert buffers[a].isdisjoint(buffers[b])


@pytest.mark.parametrize(
    "label",
    ["ACC", "DEC", "STOP", "TURN-G", "TURN-P"],
)
def test_aim_for_at_least_three_useful_epochs_when_possible(label: str) -> None:
    _s, _r, _m, nodes, corr = _run(_SCENARIO_FILES[label])
    _event, report = _temporal_snapshot(corr, nodes)
    useful = [row for row in report.epochs if row.status == "useful"]
    # Preserve natural outcome; assert the soft evidence goal when geometry allows.
    assert len(report.discovered_epochs) >= 3
    assert len(useful) >= 3


def test_hover_reports_natural_epoch_count() -> None:
    _s, _r, _m, nodes, corr = _run(_SCENARIO_FILES["HOVER"])
    _event, report = _temporal_snapshot(corr, nodes)
    # Do not invent extra stamps; just record that evaluation ran.
    assert len(report.discovered_epochs) >= 1
    assert len(report.epochs) == len(report.discovered_epochs)


if __name__ == "__main__":
    out = write_artifacts()
    print(f"Wrote artifacts under {out}")
