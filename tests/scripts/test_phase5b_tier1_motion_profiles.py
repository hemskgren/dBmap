"""Phase 5B Tier 1: JSON-only motion-profile scenarios + temporal diagnostics."""

from __future__ import annotations

import contextlib
import io
import math
import random
import sys
from datetime import UTC, datetime
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

from lib.cross_ear_localization import evaluate_against_ground_truth
from lib.localization_epoch_diagnostic import evaluate_event_localization_epochs
from lib.scenario_ground_truth import ground_truth_from_loaded_scenario

_ROOT = Path(__file__).parents[2]
_SCENARIOS = _ROOT / "simulator" / "ear" / "scenarios"
_ALIAS_TO_NODE = {
    "ear_a": "SIM-EAR-001",
    "ear_b": "SIM-EAR-002",
    "ear_d": "SIM-EAR-004",
    "ear_e": "SIM-EAR-005",
}
_ALL = set(_ALIAS_TO_NODE.values())
_START = datetime(2026, 11, 7, 17, 0, tzinfo=UTC)
_EARTH_RADIUS_M = 6_371_000.0

_NS = _SCENARIOS / "helicopter_abde_ns_overlap.json"
_DIAG = _SCENARIOS / "helicopter_abde_diagonal_overlap.json"
_SLOW = _SCENARIOS / "helicopter_abde_slow_overlap.json"
_FAST = _SCENARIOS / "helicopter_abde_fast_overlap.json"
_ABDE = _SCENARIOS / "helicopter_abde_overlap.json"


def _load(name: str, path: Path):
    spec = spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _haversine_m(a: dict[str, float], b: dict[str, float]) -> float:
    lat1 = math.radians(a["latitude"])
    lon1 = math.radians(a["longitude"])
    lat2 = math.radians(b["latitude"])
    lon2 = math.radians(b["longitude"])
    dlat = lat2 - lat1
    dlon = lon2 - lon1
    h = (
        math.sin(dlat / 2) ** 2
        + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    )
    return 2 * _EARTH_RADIUS_M * math.asin(math.sqrt(h))


def _run(
    scenario_path: Path,
    *,
    reports: list[dict] | None = None,
    reverse: bool = False,
    shuffle_seed: int | None = None,
):
    sim = _load(f"sim_{scenario_path.stem}", _ROOT / "simulator/ear/simulate_scenario.py")
    hub = _load(f"hub_{scenario_path.stem}", _ROOT / "scripts/hub_event_track.py")
    scenario = sim.load_scenario(scenario_path)
    credentials = {
        alias: {"node_id": node_id, "next_sequence_number": 1}
        for alias, node_id in _ALIAS_TO_NODE.items()
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


def _assert_no_gt_in_reports(reports: list[dict]) -> None:
    for payload in reports:
        spatial = payload.get("spatial") or {}
        assert spatial.get("position") is None
        assert "ground_truth" not in payload
        assert "ground_truth_id" not in (payload.get("identity") or {})
        hints = payload.get("hints") or {}
        # Simulator may stamp simulated_source_id; never include route/GT coordinates.
        assert "ground_truth" not in hints
        assert hints.get("route") is None


def _temporal_snapshot(corr, nodes):
    assert len(corr.events) == 1
    event = corr.events[0]
    before_id = id(event.localization)
    report = evaluate_event_localization_epochs(event, nodes)
    assert id(event.localization) == before_id
    assert report.production_localization is event.localization
    return event, report


@pytest.mark.parametrize(
    ("path", "speed", "lat_change", "lon_change"),
    [
        (_NS, 30, True, False),
        (_DIAG, 30, True, True),
        (_SLOW, 10, False, True),
        (_FAST, 50, False, True),
    ],
)
def test_tier1_motion_profile_shape(
    path: Path, speed: int, lat_change: bool, lon_change: bool
) -> None:
    sim = _load(f"shape_{path.stem}", _ROOT / "simulator/ear/simulate_scenario.py")
    scenario = sim.load_scenario(path)
    mov = scenario["movement"]
    assert mov["speed_m_s"] == speed
    dlat = abs(mov["end"]["latitude"] - mov["start"]["latitude"])
    dlon = abs(mov["end"]["longitude"] - mov["start"]["longitude"])
    if lat_change:
        assert dlat > 1e-4
    else:
        # eastbound corridor: latitude nearly constant
        assert dlat < 1e-4
    if lon_change:
        assert dlon > 1e-4
    elif path == _NS:
        assert dlon < 1e-8
    # slow/fast share ABDE start and eastbound heading
    if path in {_SLOW, _FAST}:
        abde = sim.load_scenario(_ABDE)
        assert mov["start"] == abde["movement"]["start"]
        assert mov["end"]["longitude"] > mov["start"]["longitude"]


def test_slow_vs_fast_displacement_and_same_layout() -> None:
    sim = _load("disp", _ROOT / "simulator/ear/simulate_scenario.py")
    slow = sim.load_scenario(_SLOW)
    fast = sim.load_scenario(_FAST)
    abde = sim.load_scenario(_ABDE)
    assert slow["ears"] == fast["ears"] == abde["ears"]
    assert slow["movement"]["start"] == fast["movement"]["start"] == abde["movement"]["start"]
    slow_dist = _haversine_m(slow["movement"]["start"], slow["movement"]["end"])
    fast_dist = _haversine_m(fast["movement"]["start"], fast["movement"]["end"])
    abde_dist = _haversine_m(abde["movement"]["start"], abde["movement"]["end"])
    # Same 5 synced stamps → four intervals; displacement scales with route length.
    assert slow_dist == pytest.approx(401.0, abs=1.0)
    assert fast_dist == pytest.approx(2001.0, abs=1.0)
    assert abde_dist == pytest.approx(1200.0, abs=1.0)
    assert slow_dist / 4 == pytest.approx(100.25, abs=1.0)
    assert fast_dist / 4 == pytest.approx(500.25, abs=1.0)
    assert slow_dist < abde_dist < fast_dist


@pytest.mark.parametrize("path", [_NS, _DIAG, _SLOW, _FAST])
def test_tier1_temporal_diagnostic_structure(path: Path) -> None:
    _scenario, reports, members, nodes, corr = _run(path)
    assert len(reports) == 20
    assert len(members) == 4
    _assert_no_gt_in_reports(reports)

    event, report = _temporal_snapshot(corr, nodes)
    assert set(event.ear_ids) == _ALL
    assert len(report.discovered_epochs) >= 4
    assert len(report.epochs) == len(report.discovered_epochs)
    assert all(row.evaluated for row in report.epochs)
    useful = [row for row in report.epochs if row.status == "useful"]
    assert len(useful) >= 4
    for row in report.epochs:
        assert len(row.participating_ear_ids) >= 3
        assert row.forward_pair_count >= 3
        assert row.aligned_forward_pair_count == row.forward_pair_count
        if row.status == "useful":
            assert len(row.contributing_ears) >= 3
            assert row.latitude is not None and row.longitude is not None
            assert row.outlier_pairs == ()
    # Production is one selected common-epoch result; diagnostic does not replace it.
    assert event.localization is not None
    assert event.localization.status == "useful"
    assert set(event.localization.contributing_ears) == _ALL


@pytest.mark.parametrize("path", [_NS, _DIAG, _SLOW, _FAST])
def test_tier1_deterministic_under_report_reordering(path: Path) -> None:
    _scenario, base_reports, _m, nodes, corr0 = _run(path)
    _e0, r0 = _temporal_snapshot(corr0, nodes)

    def key(report):
        return (
            tuple(report.discovered_epochs),
            tuple(row.status for row in report.epochs),
            tuple(row.participating_ear_ids for row in report.epochs),
            tuple(row.contributing_ears for row in report.epochs),
            tuple(
                (
                    row.forward_pair_count,
                    row.aligned_forward_pair_count,
                    row.consensus_pair_count,
                    row.outlier_pairs,
                )
                for row in report.epochs
            ),
            tuple(
                (
                    None if row.latitude is None else round(row.latitude, 7),
                    None if row.longitude is None else round(row.longitude, 7),
                )
                for row in report.epochs
            ),
        )

    base_key = key(r0)
    for kwargs in ({"reverse": True}, {"shuffle_seed": 1}, {"shuffle_seed": 2}):
        _s, _r, _m, nodes_i, corr_i = _run(path, reports=base_reports, **kwargs)
        _e, rep = _temporal_snapshot(corr_i, nodes_i)
        assert key(rep) == base_key


@pytest.mark.parametrize("path", [_NS, _DIAG, _SLOW, _FAST])
def test_tier1_gt_is_post_hoc_only(path: Path) -> None:
    scenario, reports, _members, nodes, corr = _run(path)
    _assert_no_gt_in_reports(reports)
    _event, report = _temporal_snapshot(corr, nodes)
    # evaluate_event_localization_epochs has no ground_truth parameter.
    import inspect

    params = inspect.signature(evaluate_event_localization_epochs).parameters
    assert "ground_truth" not in params
    # Post-hoc GT evaluation is separate and optional.
    gt = ground_truth_from_loaded_scenario(scenario)
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
        assert post.error_m < 50.0


def test_ns_and_diagonal_reuse_abde_installs_and_cadence() -> None:
    sim = _load("reuse", _ROOT / "simulator/ear/simulate_scenario.py")
    abde = sim.load_scenario(_ABDE)
    ns = sim.load_scenario(_NS)
    diag = sim.load_scenario(_DIAG)
    assert ns["ears"] == abde["ears"]
    assert diag["ears"] == abde["ears"]
    for path in (_NS, _DIAG, _SLOW, _FAST):
        sc = sim.load_scenario(path)
        stamps = sorted({obs["at_seconds"] for obs in sc["observations"]})
        assert stamps == [0, 10, 20, 30, 40]
        for t in stamps:
            ears = {obs["ear"] for obs in sc["observations"] if obs["at_seconds"] == t}
            assert ears == {"ear_a", "ear_b", "ear_d", "ear_e"}
