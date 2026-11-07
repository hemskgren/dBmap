"""Offline helicopter_east_west → B2 golden path (no MQTT / Hub / Analysis).

Uses ``independent_detection`` to generate overlapping multi-EAR observations,
builds an ``AnalysisSnapshot``, runs ``analyze()``, and evaluates localization
against a separate ground-truth trajectory derived from scenario movement.

Ground truth is never included in Observation reports or analysis inputs.
"""

from __future__ import annotations

import contextlib
import io
from dataclasses import dataclass, field
from datetime import UTC, datetime
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
from typing import Any

from lib.cross_ear_localization import GroundTruthEvaluation, LocalizationResult
from lib.scenario_ground_truth import (
    ScenarioGroundTruthTrajectory,
    evaluate_localization_against_trajectory,
    ground_truth_from_loaded_scenario,
)

ROOT = Path(__file__).resolve().parents[2]
SCENARIO_PATH = ROOT / "simulator" / "ear" / "scenarios" / "helicopter_east_west.json"
HUB_EVENT_TRACK_PATH = ROOT / "scripts" / "hub_event_track.py"
SIMULATE_SCENARIO_PATH = ROOT / "simulator" / "ear" / "simulate_scenario.py"

# Deterministic offline parameters (documented for reproduction).
GOLDEN_MODE = "independent_detection"
GOLDEN_MAX_RANGE_M = 4000.0
GOLDEN_TIME_STEP_SECONDS = 1.0
GOLDEN_OBSERVATION_INTERVAL_SECONDS = 5.0
GOLDEN_REFERENCE_LEVEL_DBFS = -30.0
GOLDEN_REFERENCE_DISTANCE_M = 1000.0
GOLDEN_PATH_LOSS_EXPONENT = 1.0
GOLDEN_STARTED_AT_UTC = datetime(2026, 11, 7, 12, 0, tzinfo=UTC)
GOLDEN_STARTED_AT_MONOTONIC_US = 0
GOLDEN_MIN_EARS_FOR_EVENT = 3
GOLDEN_ALLOW_LONG_RANGE_SIM_EARS = True

ALIAS_TO_NODE = {
    "ear_a": "SIM-EAR-001",
    "ear_b": "SIM-EAR-002",
    "ear_c": "SIM-EAR-003",
}


def _load_module(module_name: str, path: Path):
    spec = spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_hub_event_track():
    return _load_module("dbmap_hub_event_track_heli_golden", HUB_EVENT_TRACK_PATH)


def load_simulate_scenario():
    return _load_module("dbmap_sim_heli_golden", SIMULATE_SCENARIO_PATH)


@dataclass(frozen=True)
class GoldenPathParams:
    """Explicit independent_detection + B2 analysis knobs for the golden path."""

    scenario_path: Path = SCENARIO_PATH
    mode: str = GOLDEN_MODE
    max_range_m: float = GOLDEN_MAX_RANGE_M
    time_step_seconds: float = GOLDEN_TIME_STEP_SECONDS
    observation_interval_seconds: float = GOLDEN_OBSERVATION_INTERVAL_SECONDS
    reference_level_dbfs: float = GOLDEN_REFERENCE_LEVEL_DBFS
    reference_distance_m: float = GOLDEN_REFERENCE_DISTANCE_M
    path_loss_exponent: float = GOLDEN_PATH_LOSS_EXPONENT
    started_at_utc: datetime = GOLDEN_STARTED_AT_UTC
    started_at_monotonic_us: int = GOLDEN_STARTED_AT_MONOTONIC_US
    min_ears_for_event: int = GOLDEN_MIN_EARS_FOR_EVENT
    allow_long_range_sim_ears: bool = GOLDEN_ALLOW_LONG_RANGE_SIM_EARS


@dataclass
class LocalizationEvaluationRow:
    event_id: str
    evaluation_time_utc: datetime | None
    estimated_latitude: float | None
    estimated_longitude: float | None
    ground_truth_latitude: float | None
    ground_truth_longitude: float | None
    error_m: float | None
    contributing_ears: list[str]
    localization_status: str | None
    position_withheld: bool | None
    membership_ears: list[str]
    notes: str


@dataclass
class GoldenPathBundle:
    """Collected offline simulation + analysis artifacts for one run."""

    params: GoldenPathParams
    scenario: dict[str, Any]
    diagnostics: Any
    reports: list[dict[str, Any]]
    nodes: dict[str, dict[str, Any]]
    ground_truth: ScenarioGroundTruthTrajectory
    analysis_result: Any
    localization_evaluations: list[LocalizationEvaluationRow] = field(
        default_factory=list
    )


def prepare_independent_scenario(
    params: GoldenPathParams | None = None,
    *,
    sim=None,
) -> tuple[dict[str, Any], Any]:
    """Load helicopter_east_west and apply independent_detection mode."""
    params = params or GoldenPathParams()
    sim = sim or load_simulate_scenario()
    loaded = sim.load_scenario(params.scenario_path)
    prepared, diagnostics = sim.apply_simulation_mode(
        loaded,
        mode=params.mode,
        time_step_seconds=params.time_step_seconds,
        max_range_m=params.max_range_m,
        reference_level_dbfs=params.reference_level_dbfs,
        reference_distance_m=params.reference_distance_m,
        path_loss_exponent=params.path_loss_exponent,
        observation_interval_seconds=params.observation_interval_seconds,
    )
    return prepared, diagnostics


def nodes_from_scenario(scenario: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """Map scenario Ear installations onto SIM-EAR node IDs."""
    nodes: dict[str, dict[str, Any]] = {}
    for alias, ear in scenario["ears"].items():
        node_id = ALIAS_TO_NODE[alias]
        nodes[node_id] = {
            "installation": {
                "latitude": ear["latitude"],
                "longitude": ear["longitude"],
                "orientation_deg": ear["orientation_deg"],
            }
        }
    return nodes


def collect_offline_reports(
    scenario: dict[str, Any],
    params: GoldenPathParams | None = None,
    *,
    sim=None,
) -> list[dict[str, Any]]:
    """Publish scenario observations into an in-memory list (no MQTT)."""
    params = params or GoldenPathParams()
    sim = sim or load_simulate_scenario()
    credentials = {
        alias: {
            "node_id": ALIAS_TO_NODE[alias],
            "next_sequence_number": 1,
        }
        for alias in scenario["ears"]
    }
    published: list[dict[str, Any]] = []
    sim.run_scenario(
        scenario,
        credentials,
        {alias: Path(f"{alias}.json") for alias in scenario["ears"]},
        "ca.crt",
        1.0,
        publish=lambda _credentials, payload, _ca: (
            published.append(payload) or payload["identity"]["observation_id"]
        ),
        sleep=lambda _seconds: None,
        monotonic=lambda: 10**12,
        started_at_utc=params.started_at_utc,
        started_at_monotonic_us=params.started_at_monotonic_us,
        save=lambda *_args, **_kwargs: None,
        publish_keepalive=lambda *_args, **_kwargs: None,
        make_keepalive=lambda _credentials, uptime_s: {"uptime_s": uptime_s},
        announce=False,
    )
    return published


def measurement_fingerprint(reports: list[dict[str, Any]]) -> list[tuple]:
    """Semantic measurement content excluding opaque observation_id values.

    Used to compare regenerated simulations where UUIDs differ but geometry,
    timing, levels, and emission kinds must match.
    """
    rows: list[tuple] = []
    for report in reports:
        identity = report.get("identity") or {}
        context = report.get("context") or {}
        acoustic = report.get("acoustic") or {}
        spatial = report.get("spatial") or {}
        bearing = spatial.get("bearing") or {}
        classification = acoustic.get("classification") or {}
        rows.append(
            (
                identity.get("node_id"),
                identity.get("sequence_number"),
                context.get("event_time_utc"),
                context.get("emission_kind"),
                context.get("interval_duration_ms"),
                context.get("episode_duration_ms"),
                acoustic.get("signal_level_dbfs"),
                classification.get("source_family"),
                classification.get("confidence"),
                # GT hint may appear in reports as simulator metadata; it must
                # not affect association. Included here only for content equality.
                (classification.get("hints") or {}).get("simulated_source_id"),
                bearing.get("deg"),
                bearing.get("confidence"),
            )
        )
    return rows


def analysis_config(params: GoldenPathParams, hub):
    return hub.AnalysisConfig(
        allow_long_range_sim_ears=params.allow_long_range_sim_ears,
        min_ears_for_event=params.min_ears_for_event,
        include_cross_ear_correlation=True,
        include_cross_ear_geometry=True,
    )


def analyze_reports(
    reports: list[dict[str, Any]],
    nodes: dict[str, dict[str, Any]],
    params: GoldenPathParams | None = None,
    *,
    hub=None,
):
    """Build ``AnalysisSnapshot`` and call shared ``analyze()``."""
    params = params or GoldenPathParams()
    hub = hub or load_hub_event_track()
    snapshot = hub.AnalysisSnapshot(
        reports=reports,
        nodes=nodes,
        config=analysis_config(params, hub),
    )
    return hub.analyze(snapshot)


def evaluate_event_localizations(
    analysis_result: Any,
    ground_truth: ScenarioGroundTruthTrajectory,
    *,
    scenario_started_at_utc: datetime,
) -> list[LocalizationEvaluationRow]:
    """Post-hoc GT evaluation for each Event localization (if any)."""
    rows: list[LocalizationEvaluationRow] = []
    correlation = analysis_result.correlation
    if correlation is None:
        return rows
    for event in correlation.events:
        localization: LocalizationResult | None = event.localization
        if localization is None:
            rows.append(
                LocalizationEvaluationRow(
                    event_id=event.event_id,
                    evaluation_time_utc=None,
                    estimated_latitude=None,
                    estimated_longitude=None,
                    ground_truth_latitude=None,
                    ground_truth_longitude=None,
                    error_m=None,
                    contributing_ears=[],
                    localization_status=None,
                    position_withheld=None,
                    membership_ears=list(event.ear_ids),
                    notes="Event has no localization result.",
                )
            )
            continue
        evaluation: GroundTruthEvaluation = evaluate_localization_against_trajectory(
            localization,
            ground_truth,
            scenario_started_at_utc=scenario_started_at_utc,
        )
        rows.append(
            LocalizationEvaluationRow(
                event_id=event.event_id,
                evaluation_time_utc=localization.evaluation_time_utc,
                estimated_latitude=localization.latitude,
                estimated_longitude=localization.longitude,
                ground_truth_latitude=evaluation.ground_truth_latitude,
                ground_truth_longitude=evaluation.ground_truth_longitude,
                error_m=evaluation.error_m,
                contributing_ears=list(localization.contributing_ears),
                localization_status=localization.status,
                position_withheld=bool(localization.details.get("position_withheld")),
                membership_ears=list(event.ear_ids),
                notes=evaluation.notes,
            )
        )
    return rows


def run_golden_path(
    params: GoldenPathParams | None = None,
    *,
    sim=None,
    hub=None,
) -> GoldenPathBundle:
    """Execute the full offline helicopter → B2 golden path once."""
    params = params or GoldenPathParams()
    sim = sim or load_simulate_scenario()
    hub = hub or load_hub_event_track()
    scenario, diagnostics = prepare_independent_scenario(params, sim=sim)
    ground_truth = ground_truth_from_loaded_scenario(scenario)
    # Offline collect reuses run_scenario; mute keepalive/progress prints.
    with contextlib.redirect_stdout(io.StringIO()):
        reports = collect_offline_reports(scenario, params, sim=sim)
    nodes = nodes_from_scenario(scenario)
    analysis_result = analyze_reports(reports, nodes, params, hub=hub)
    evaluations = evaluate_event_localizations(
        analysis_result,
        ground_truth,
        scenario_started_at_utc=params.started_at_utc,
    )
    return GoldenPathBundle(
        params=params,
        scenario=scenario,
        diagnostics=diagnostics,
        reports=reports,
        nodes=nodes,
        ground_truth=ground_truth,
        analysis_result=analysis_result,
        localization_evaluations=evaluations,
    )


def event_membership_signature(analysis_result: Any) -> list[tuple]:
    """Stable membership view for determinism checks."""
    correlation = analysis_result.correlation
    if correlation is None:
        return []
    rows: list[tuple] = []
    for event in correlation.events:
        rows.append(
            (
                event.event_id,
                tuple(sorted(event.ear_ids)),
                tuple(sorted(event.observation_ids)),
                event.score,
            )
        )
    return rows


def localization_signature(analysis_result: Any) -> list[tuple]:
    """Localization fields excluding non-semantic metadata."""
    correlation = analysis_result.correlation
    if correlation is None:
        return []
    rows: list[tuple] = []
    for event in correlation.events:
        localization = event.localization
        if localization is None:
            rows.append((event.event_id, None))
            continue
        rows.append(
            (
                event.event_id,
                localization.status,
                localization.latitude,
                localization.longitude,
                (
                    localization.evaluation_time_utc.isoformat()
                    if localization.evaluation_time_utc is not None
                    else None
                ),
                tuple(localization.contributing_ears),
                localization.max_bearing_dt_s,
                localization.pairwise_spread_m,
                localization.details.get("position_withheld"),
                localization.details.get("best_intersection_angle_deg"),
                tuple(localization.quality_notes),
            )
        )
    return rows


def format_golden_path_report(bundle: GoldenPathBundle) -> list[str]:
    """Human-readable offline golden-path report lines."""
    result = bundle.analysis_result
    correlation = result.correlation
    params = bundle.params
    diagnostics = bundle.diagnostics
    lines = [
        "Helicopter offline B2 golden path",
        f"scenario={params.scenario_path.name}",
        f"mode={params.mode}",
        (
            f"detection: max_range_m={params.max_range_m:g}, "
            f"time_step_s={params.time_step_seconds:g}, "
            f"observation_interval_s={params.observation_interval_seconds:g}"
        ),
        (
            f"signal: reference_level_dbfs={params.reference_level_dbfs:g}, "
            f"reference_distance_m={params.reference_distance_m:g}, "
            f"path_loss_exponent={params.path_loss_exponent:g}"
        ),
        (
            f"analysis: allow_long_range_sim_ears="
            f"{params.allow_long_range_sim_ears}, "
            f"min_ears_for_event={params.min_ears_for_event}"
        ),
        f"started_at_utc={params.started_at_utc.isoformat()}",
        f"ground_truth_id={bundle.ground_truth.ground_truth_id} (evaluation only)",
        (
            f"reports={result.report_count}, "
            f"unique_observations={result.unique_observation_count}"
        ),
    ]
    if diagnostics is not None:
        participating = sorted(
            alias
            for alias, stats in diagnostics.per_ear.items()
            if stats.observations_emitted > 0
        )
        lines.append(
            f"participating_ears={participating} "
            f"(simultaneous_three_intervals="
            f"{diagnostics.simultaneous_three_intervals})"
        )
        lines.append(
            f"emitted_per_ear="
            f"{ {a: s.observations_emitted for a, s in diagnostics.per_ear.items()} }"
        )
    if correlation is None:
        lines.append("correlation: disabled or unavailable")
        return lines

    lines.append(
        f"promoted_events={len(correlation.events)}, "
        f"unresolved={len(correlation.unresolved)}, "
        f"candidates={len(correlation.candidates)}"
    )
    for event in correlation.events:
        lines.append(
            f"event {event.event_id}: membership_ears={event.ear_ids}, "
            f"members={len(event.observation_ids)}, score={event.score}"
        )
        localization = event.localization
        if localization is None:
            lines.append("  localization: none")
            continue
        lines.append(
            f"  localization: status={localization.status}, "
            f"contributing_ears={localization.contributing_ears}, "
            f"position_withheld={localization.details.get('position_withheld')}, "
            f"lat={localization.latitude}, lon={localization.longitude}, "
            f"eval_time={localization.evaluation_time_utc}"
        )
        for note in localization.quality_notes:
            lines.append(f"  note: {note}")
    for unresolved in correlation.unresolved:
        lines.append(
            f"unresolved {unresolved.observation_id} node={unresolved.node_id}: "
            f"{unresolved.reason}"
        )
    for row in bundle.localization_evaluations:
        lines.append(
            f"GT eval event={row.event_id}: status={row.localization_status}, "
            f"eval_time={row.evaluation_time_utc}, "
            f"estimated=({row.estimated_latitude}, {row.estimated_longitude}), "
            f"ground_truth=({row.ground_truth_latitude}, "
            f"{row.ground_truth_longitude}), "
            f"error_m={row.error_m}, "
            f"contributors={row.contributing_ears}, "
            f"membership_ears={row.membership_ears}"
        )
    return lines
