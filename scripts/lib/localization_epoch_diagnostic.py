"""Offline diagnostic: pairwise vs common-epoch bearing selection.

Does **not** change production ``estimate_localization`` behaviour. Uses the
same geometry/gates by calling ``estimate_localization`` on either the full
sample set (current pairwise ``select_time_aware_bearing_pair``) or on
samples restricted to one shared timestamp (common-epoch mode).

Ground truth must not be used to select epochs or bearings; callers may
evaluate positions afterward.
"""

from __future__ import annotations

import contextlib
import io
import math
import sys
from copy import deepcopy
from dataclasses import dataclass, field, replace
from datetime import UTC, datetime
from itertools import combinations
from pathlib import Path
from typing import Any

_SCRIPTS_DIR = Path(__file__).resolve().parents[1]
if __name__ == "__main__" and str(_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_DIR))

from lib.cross_ear_localization import (
    LOCALIZATION_MAX_BEARING_DT_S,
    BearingSample,
    LocalizationResult,
    estimate_localization,
    haversine_m,
)

ADE_SCENARIO_REL = "simulator/ear/scenarios/helicopter_ade_overlap.json"
ADE_ALIAS_TO_NODE = {
    "ear_a": "SIM-EAR-001",
    "ear_d": "SIM-EAR-004",
    "ear_e": "SIM-EAR-005",
}
ADE_STARTED_AT_UTC = datetime(2026, 11, 7, 12, 0, tzinfo=UTC)


@dataclass(frozen=True)
class PairSelectionSummary:
    ear_a_id: str
    ear_b_id: str
    bearing_a_time: datetime | None
    bearing_b_time: datetime | None
    bearing_dt_s: float | None
    time_aligned: bool
    selection_method: str
    intersection_status: str
    outlier: bool
    latitude: float | None
    longitude: float | None
    intersection_angle_deg: float | None
    geometric_dilution_m: float | None


@dataclass
class EpochEvaluation:
    epoch_utc: datetime
    ear_ids_present: list[str]
    localization: LocalizationResult
    pair_summaries: list[PairSelectionSummary] = field(default_factory=list)


@dataclass
class EpochComparisonReport:
    """Side-by-side current pairwise vs common-epoch diagnostic."""

    current: LocalizationResult
    current_pairs: list[PairSelectionSummary]
    candidate_epochs: list[datetime]
    epoch_evaluations: list[EpochEvaluation]
    selected_common_epoch: datetime | None
    common_epoch: LocalizationResult | None
    common_epoch_pairs: list[PairSelectionSummary]
    selection_notes: list[str]
    # Diagnostic-only heuristic description (not a production claim).
    common_epoch_selection_rule: str


def summarize_pairs(result: LocalizationResult) -> list[PairSelectionSummary]:
    return [
        PairSelectionSummary(
            ear_a_id=item.ear_a_id,
            ear_b_id=item.ear_b_id,
            bearing_a_time=item.bearing_a_time,
            bearing_b_time=item.bearing_b_time,
            bearing_dt_s=item.bearing_dt_s,
            time_aligned=item.time_aligned,
            selection_method=item.selection_method,
            intersection_status=item.intersection_status,
            outlier=item.outlier,
            latitude=item.latitude,
            longitude=item.longitude,
            intersection_angle_deg=item.intersection_angle_deg,
            geometric_dilution_m=item.geometric_dilution_m,
        )
        for item in result.pairs
    ]


def unique_sample_epochs(members: list[Any]) -> list[datetime]:
    """All distinct bearing event times across members (stable ascending)."""
    stamps: set[datetime] = set()
    for member in members:
        for sample in getattr(member, "bearing_samples", None) or []:
            if sample.event_time_utc is not None:
                stamps.add(sample.event_time_utc.astimezone(UTC))
    return sorted(stamps)


def ears_present_at_epoch(
    members: list[Any],
    epoch: datetime,
    *,
    max_dt_s: float = LOCALIZATION_MAX_BEARING_DT_S,
) -> list[str]:
    present: list[str] = []
    epoch = epoch.astimezone(UTC)
    for member in members:
        for sample in getattr(member, "bearing_samples", None) or []:
            if sample.event_time_utc is None:
                continue
            dt = abs((sample.event_time_utc.astimezone(UTC) - epoch).total_seconds())
            if dt <= max_dt_s:
                present.append(member.node_id)
                break
    return sorted(set(present))


def common_epochs(
    members: list[Any],
    *,
    min_ears: int = 2,
    max_dt_s: float = LOCALIZATION_MAX_BEARING_DT_S,
) -> list[datetime]:
    """Timestamps where at least ``min_ears`` Observations have a sample."""
    return [
        epoch
        for epoch in unique_sample_epochs(members)
        if len(ears_present_at_epoch(members, epoch, max_dt_s=max_dt_s)) >= min_ears
    ]


def _pick_sample_at_epoch(
    samples: list[BearingSample],
    epoch: datetime,
    *,
    max_dt_s: float,
) -> BearingSample | None:
    epoch = epoch.astimezone(UTC)
    candidates: list[tuple[float, float, BearingSample]] = []
    for sample in samples:
        if sample.event_time_utc is None:
            continue
        dt = abs((sample.event_time_utc.astimezone(UTC) - epoch).total_seconds())
        if dt > max_dt_s:
            continue
        conf = sample.confidence if sample.confidence is not None else -1.0
        candidates.append((dt, -conf, sample))
    if not candidates:
        return None
    candidates.sort(key=lambda item: (item[0], item[1]))
    return candidates[0][2]


def members_restricted_to_epoch(
    members: list[Any],
    epoch: datetime,
    *,
    max_dt_s: float = LOCALIZATION_MAX_BEARING_DT_S,
) -> list[Any]:
    """Return shallow-copied members retaining only the sample at ``epoch``.

    Ears without a sample within the timing gate are omitted. Production
    ``estimate_localization`` is then run unchanged on the restricted set, so
    pairwise selection sees one sample per Ear and cannot mix epochs.
    """
    restricted: list[Any] = []
    for member in members:
        samples = list(getattr(member, "bearing_samples", None) or [])
        chosen = _pick_sample_at_epoch(samples, epoch, max_dt_s=max_dt_s)
        if chosen is None:
            continue
        if hasattr(member, "__dataclass_fields__"):
            clone = replace(
                member,
                bearing_samples=[chosen],
                bearing_deg=chosen.bearing_deg,
                bearing_confidence=chosen.confidence,
            )
        else:
            clone = deepcopy(member)
            clone.bearing_samples = [chosen]
            clone.bearing_deg = chosen.bearing_deg
            clone.bearing_confidence = chosen.confidence
        restricted.append(clone)
    return restricted


def _status_rank(status: str) -> int:
    return {"useful": 3, "weak": 2, "unavailable": 1}.get(status, 0)


def _network_sensitivity(result: LocalizationResult) -> float | None:
    details = result.details or {}
    value = details.get("network_geometric_sensitivity_m")
    if isinstance(value, (int, float)):
        return float(value)
    return None


def select_common_epoch_diagnostic(
    evaluations: list[EpochEvaluation],
) -> tuple[EpochEvaluation | None, list[str]]:
    """Pick one epoch without ground truth (diagnostic heuristic only).

    Prefer: more ears present → better status → more aligned forward pairs →
    lower network sensitivity → higher min intersection angle → earlier time.
    """
    notes = [
        "Common-epoch pick is diagnostic-only (not production policy).",
        (
            "Order: ear count, status, aligned_forward_pair_count, "
            "lower network sensitivity, higher min intersection angle, earlier epoch."
        ),
    ]
    if not evaluations:
        notes.append("No candidate epochs with ≥2 Ears present.")
        return None, notes

    def sort_key(item: EpochEvaluation) -> tuple:
        loc = item.localization
        details = loc.details or {}
        aligned = int(details.get("aligned_forward_pair_count") or 0)
        sens = _network_sensitivity(loc)
        sens_key = (
            sens if sens is not None and math.isfinite(sens) else float("inf")
        )
        min_angle = details.get("min_intersection_angle_deg")
        angle_key = (
            float(min_angle) if isinstance(min_angle, (int, float)) else -1.0
        )
        return (
            len(item.ear_ids_present),
            _status_rank(loc.status),
            aligned,
            -sens_key,
            angle_key,
            -item.epoch_utc.timestamp(),
        )

    ranked = sorted(evaluations, key=sort_key, reverse=True)
    chosen = ranked[0]
    notes.append(
        f"Selected epoch {chosen.epoch_utc.isoformat()} "
        f"(ears={chosen.ear_ids_present}, status={chosen.localization.status})."
    )
    return chosen, notes


def compare_pairwise_vs_common_epoch(
    members: list[Any],
    nodes: dict[str, dict[str, Any]],
    *,
    max_dt_s: float = LOCALIZATION_MAX_BEARING_DT_S,
    min_ears_for_epoch: int = 2,
) -> EpochComparisonReport:
    """Compare forced pairwise_only vs production auto (common-epoch when possible)."""
    current = estimate_localization(
        members, nodes, max_dt_s=max_dt_s, bearing_selection="pairwise_only"
    )
    current_pairs = summarize_pairs(current)

    epochs = common_epochs(
        members, min_ears=min_ears_for_epoch, max_dt_s=max_dt_s
    )
    evaluations: list[EpochEvaluation] = []
    for epoch in epochs:
        restricted = members_restricted_to_epoch(
            members, epoch, max_dt_s=max_dt_s
        )
        if len(restricted) < min_ears_for_epoch:
            continue
        loc = estimate_localization(restricted, nodes, max_dt_s=max_dt_s)
        evaluations.append(
            EpochEvaluation(
                epoch_utc=epoch,
                ear_ids_present=ears_present_at_epoch(
                    members, epoch, max_dt_s=max_dt_s
                ),
                localization=loc,
                pair_summaries=summarize_pairs(loc),
            )
        )

    # Production auto path (common-epoch when candidates exist).
    production = estimate_localization(members, nodes, max_dt_s=max_dt_s)
    selected, notes = select_common_epoch_diagnostic(evaluations)
    # Prefer production's chosen epoch when auto selected common_epoch.
    prod_mode = (production.details or {}).get("bearing_selection_mode")
    prod_epoch_raw = (production.details or {}).get("common_epoch_utc")
    if prod_mode == "common_epoch" and isinstance(prod_epoch_raw, str):
        selected_epoch = datetime.fromisoformat(prod_epoch_raw)
        common_loc = production
        common_pairs = summarize_pairs(production)
        notes.append(
            f"Production auto selected common_epoch_utc={prod_epoch_raw} "
            f"(status={production.status})."
        )
    else:
        selected_epoch = selected.epoch_utc if selected else None
        common_loc = selected.localization if selected else None
        common_pairs = selected.pair_summaries if selected else []
        notes.append(
            f"Production auto mode={prod_mode!r}; "
            "diagnostic epoch table still lists per-epoch restricted runs."
        )

    return EpochComparisonReport(
        current=current,
        current_pairs=current_pairs,
        candidate_epochs=epochs,
        epoch_evaluations=evaluations,
        selected_common_epoch=selected_epoch,
        common_epoch=common_loc,
        common_epoch_pairs=common_pairs,
        selection_notes=notes,
        common_epoch_selection_rule=(
            "Production ranking: ear_count > status > aligned_forward_pair_count > "
            "lower network sensitivity > higher min intersection angle > "
            "earlier common_epoch_utc"
        ),
    )


def format_comparison_report(report: EpochComparisonReport) -> str:
    """Human-readable diagnostic dump for reproduction notes."""
    lines: list[str] = []
    lines.append(
        "=== Pairwise-only fallback (bearing_selection=pairwise_only) ==="
    )
    lines.append(
        f"status={report.current.status} "
        f"contributors={report.current.contributing_ears} "
        f"pos={report.current.latitude},{report.current.longitude} "
        f"max_dt={report.current.max_bearing_dt_s} "
        f"spread={report.current.pairwise_spread_m}"
    )
    for pair in report.current_pairs:
        lines.append(
            f"  {pair.ear_a_id}↔{pair.ear_b_id}: "
            f"tA={pair.bearing_a_time} tB={pair.bearing_b_time} "
            f"Δt={pair.bearing_dt_s} aligned={pair.time_aligned} "
            f"status={pair.intersection_status} outlier={pair.outlier} "
            f"∠={pair.intersection_angle_deg} dil={pair.geometric_dilution_m} "
            f"method={pair.selection_method}"
        )

    lines.append("")
    lines.append(
        f"=== Common-epoch candidates ({len(report.candidate_epochs)}) "
        f"selected={report.selected_common_epoch} ==="
    )
    lines.append(f"rule: {report.common_epoch_selection_rule}")
    for note in report.selection_notes:
        lines.append(f"  note: {note}")
    for evaluation in report.epoch_evaluations:
        loc = evaluation.localization
        lines.append(
            f"  epoch={evaluation.epoch_utc.isoformat()} "
            f"ears={evaluation.ear_ids_present} status={loc.status} "
            f"contributors={loc.contributing_ears} "
            f"pos={loc.latitude},{loc.longitude} "
            f"spread={loc.pairwise_spread_m} "
            f"sens={_network_sensitivity(loc)} "
            f"min∠={(loc.details or {}).get('min_intersection_angle_deg')}"
        )
        for pair in evaluation.pair_summaries:
            lines.append(
                f"    {pair.ear_a_id}↔{pair.ear_b_id}: "
                f"tA={pair.bearing_a_time} tB={pair.bearing_b_time} "
                f"Δt={pair.bearing_dt_s} aligned={pair.time_aligned} "
                f"status={pair.intersection_status} outlier={pair.outlier} "
                f"∠={pair.intersection_angle_deg}"
            )

    if (
        report.current.latitude is not None
        and report.common_epoch is not None
        and report.common_epoch.latitude is not None
    ):
        delta = haversine_m(
            report.current.latitude,
            report.current.longitude or 0.0,
            report.common_epoch.latitude,
            report.common_epoch.longitude or 0.0,
        )
        lines.append("")
        lines.append(f"Position delta current→common-epoch: {delta:.1f} m")
    return "\n".join(lines)


def explain_mixed_epoch_root_cause(report: EpochComparisonReport) -> str:
    """Describe why pairwise mode can pick different epochs per ear-pair."""
    epochs = {
        (pair.ear_a_id, pair.ear_b_id): (pair.bearing_a_time, pair.bearing_b_time)
        for pair in report.current_pairs
        if pair.bearing_a_time is not None and pair.bearing_b_time is not None
    }
    distinct = {item for item in epochs.values()}
    lines = [
        (
            "Production selects each Observation pair independently via "
            "select_time_aware_bearing_pair: minimize |Δt|, then maximize "
            "bearing.confidence on the first sample, then the second."
        ),
        "There is no shared localization epoch across all Event members.",
    ]
    if len(distinct) > 1:
        lines.append(
            "In this run, different ear-pairs selected different timestamps "
            f"({len(distinct)} distinct epoch pairs):"
        )
        for key, times in sorted(epochs.items()):
            lines.append(f"  {key[0]}↔{key[1]}: {times[0]} / {times[1]}")
        lines.append(
            "With synchronized Δt=0 candidates at every cadence tick, the "
            "confidence tie-break alone chooses the epoch — so pairs that "
            "peak on different Ears (e.g. A-led vs D-led) diverge."
        )
    else:
        lines.append("This run happened to select a single shared epoch across pairs.")
    return "\n".join(lines)


def compare_member_sample_sets(
    left: list[Any],
    right: list[Any],
) -> list[tuple[str, str]]:
    """Utility for tests: (node_id, iso_time) sample inventory."""
    rows: list[tuple[str, str]] = []
    for member in left + right:
        for sample in getattr(member, "bearing_samples", None) or []:
            if sample.event_time_utc is None:
                continue
            rows.append(
                (
                    member.node_id,
                    sample.event_time_utc.astimezone(UTC).isoformat(),
                )
            )
    return sorted(set(rows))


def epoch_coverage_matrix(
    members: list[Any],
) -> dict[datetime, set[str]]:
    coverage: dict[datetime, set[str]] = {}
    for member in members:
        for sample in getattr(member, "bearing_samples", None) or []:
            if sample.event_time_utc is None:
                continue
            stamp = sample.event_time_utc.astimezone(UTC)
            coverage.setdefault(stamp, set()).add(member.node_id)
    return coverage


def all_pair_ids(ear_ids: list[str]) -> list[tuple[str, str]]:
    return list(combinations(sorted(ear_ids), 2))


def _load_module(name: str, path: Path):
    from importlib.util import module_from_spec, spec_from_file_location

    spec = spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_ade_overlap_members_offline(
    *,
    repo_root: Path | None = None,
    started_at_utc: datetime = ADE_STARTED_AT_UTC,
) -> tuple[list[Any], dict[str, dict[str, Any]], dict[str, Any], list[dict[str, Any]]]:
    """Offline ADE scenario → assembled Observations + nodes (no MQTT).

    Bearings are the scenario simulator's authored measurements (geometry from
    installations + route). Ground truth is not used for localization inputs.
    """
    root = repo_root or Path(__file__).resolve().parents[2]
    sim = _load_module(
        "dbmap_sim_ade_epoch_diag",
        root / "simulator" / "ear" / "simulate_scenario.py",
    )
    hub = _load_module(
        "dbmap_hub_ade_epoch_diag",
        root / "scripts" / "hub_event_track.py",
    )
    scenario_path = root / ADE_SCENARIO_REL
    scenario = sim.load_scenario(scenario_path)
    credentials = {
        alias: {
            "node_id": ADE_ALIAS_TO_NODE[alias],
            "next_sequence_number": 1,
        }
        for alias in scenario["ears"]
    }
    published: list[dict[str, Any]] = []
    # run_scenario prints keepalive / progress lines; keep diagnostic stdout clean.
    with contextlib.redirect_stdout(io.StringIO()):
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
            started_at_utc=started_at_utc,
            started_at_monotonic_us=0,
            save=lambda *_args, **_kwargs: None,
            publish_keepalive=lambda *_args, **_kwargs: None,
            make_keepalive=lambda _credentials, uptime_s: {"uptime_s": uptime_s},
            announce=False,
        )
    nodes = {
        ADE_ALIAS_TO_NODE[alias]: {
            "installation": {
                "latitude": ear["latitude"],
                "longitude": ear["longitude"],
                "orientation_deg": ear["orientation_deg"],
            }
        }
        for alias, ear in scenario["ears"].items()
    }
    members = hub.build_assembled_observations(published, nodes)
    return members, nodes, scenario, published


def run_ade_overlap_epoch_comparison(
    *,
    repo_root: Path | None = None,
) -> EpochComparisonReport:
    members, nodes, _scenario, _published = load_ade_overlap_members_offline(
        repo_root=repo_root
    )
    return compare_pairwise_vs_common_epoch(members, nodes)


if __name__ == "__main__":
    comparison = run_ade_overlap_epoch_comparison()
    print(format_comparison_report(comparison))
    print()
    print(explain_mixed_epoch_root_cause(comparison))
