"""Serialize AnalysisResult into the map-demo Analysis scratch layer contract.

Produces analysis-only map-layer content. Never fabricates ground truth, source
coordinates, confidence scores, or withheld localization positions.

Observation markers use EAR installation coordinates only.
Event/localization markers use published LocalizationResult coordinates only.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any

from lib.cross_ear_event_correlation import CorrelationResult
from lib.cross_ear_localization import LocalizationResult

MAP_DEMO_NAMESPACE = "map-demo"


class IncompleteSnapshotError(RuntimeError):
    """Raised when Hub pagination did not yield a complete frozen snapshot."""


@dataclass(frozen=True)
class AnalysisRunCounts:
    """Counts printed by the Hub→map analysis CLI."""

    reports_fetched: int
    observations_assembled: int
    observations_after_filter: int
    events: int
    localizations_published: int
    localizations_withheld: int
    reports_after_filter: int = 0


def localization_is_published(localization: LocalizationResult | None) -> bool:
    """True only when a LocalizationResult publishes usable map coordinates."""
    if localization is None:
        return False
    if localization.details.get("position_withheld"):
        return False
    lat = localization.latitude
    lon = localization.longitude
    if lat is None or lon is None:
        return False
    if not isinstance(lat, (int, float)) or not isinstance(lon, (int, float)):
        return False
    if isinstance(lat, bool) or isinstance(lon, bool):
        return False
    return math.isfinite(float(lat)) and math.isfinite(float(lon))


def _optional_str(value: Any) -> str | None:
    if isinstance(value, str) and value:
        return value
    return None


def _optional_int(value: Any) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int):
        return None
    return value


def _optional_finite_float(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    number = float(value)
    if not math.isfinite(number):
        return None
    return number


def _optional_str_list(value: Any) -> list[str] | None:
    if not isinstance(value, list):
        return None
    items = [item for item in value if isinstance(item, str) and item]
    return items


def _optional_exclusion_rows(value: Any) -> list[dict[str, str]] | None:
    if not isinstance(value, list):
        return None
    rows: list[dict[str, str]] = []
    for item in value:
        if not isinstance(item, dict):
            continue
        ear_id = item.get("ear_id")
        if not isinstance(ear_id, str) or not ear_id:
            continue
        row: dict[str, str] = {"ear_id": ear_id}
        reason = item.get("reason")
        if isinstance(reason, str) and reason:
            row["reason"] = reason
        observation_id = item.get("observation_id")
        if isinstance(observation_id, str) and observation_id:
            row["observation_id"] = observation_id
        rows.append(row)
    return rows


def localization_evidence_fields(localization: LocalizationResult) -> dict[str, Any]:
    """Map-facing diagnostic fields reused from LocalizationResult.details.

    Omits keys when the underlying diagnostic is absent so older scratch
    datasets and partial results stay compatible.
    """
    details = localization.details or {}
    evidence: dict[str, Any] = {
        "contributing_ear_count": len(localization.contributing_ears),
    }
    mode = _optional_str(details.get("bearing_selection_mode"))
    if mode is not None:
        evidence["bearing_selection_mode"] = mode
    common_epoch = _optional_str(details.get("common_epoch_utc"))
    if common_epoch is not None:
        evidence["common_epoch_utc"] = common_epoch
    forward = _optional_int(details.get("forward_pair_count"))
    if forward is not None:
        evidence["forward_pair_count"] = forward
    aligned = _optional_int(details.get("aligned_forward_pair_count"))
    if aligned is not None:
        evidence["aligned_forward_pair_count"] = aligned
    consensus_count = _optional_int(details.get("consensus_pair_count"))
    if consensus_count is not None:
        evidence["consensus_pair_count"] = consensus_count
    consensus_ears = _optional_str_list(details.get("consensus_selected_ears"))
    if consensus_ears is not None:
        evidence["consensus_selected_ears"] = consensus_ears
    outliers = _optional_str_list(details.get("outlier_pairs"))
    if outliers is not None:
        evidence["outlier_pairs"] = outliers
    sensitivity = _optional_finite_float(
        details.get("network_geometric_sensitivity_m")
    )
    if sensitivity is not None:
        evidence["network_geometric_sensitivity_m"] = sensitivity
    excluded = _optional_exclusion_rows(details.get("common_epoch_excluded_ears"))
    if excluded is not None:
        evidence["common_epoch_excluded_ears"] = excluded
    return evidence


def count_localization_outcomes(
    correlation: CorrelationResult | None,
) -> tuple[int, int]:
    """Return (published, withheld_or_missing) for Events in the correlation."""
    if correlation is None:
        return 0, 0
    published = 0
    withheld = 0
    for event in correlation.events:
        if localization_is_published(event.localization):
            published += 1
        else:
            withheld += 1
    return published, withheld


def require_complete_snapshot(fetch_meta: dict[str, Any]) -> None:
    """Refuse analyze/publish when the Hub freeze was incomplete."""
    if fetch_meta.get("complete_snapshot") is True:
        return
    snapshot_id = fetch_meta.get("snapshot_id")
    fetched = fetch_meta.get("fetched_report_count")
    total = fetch_meta.get("total_count")
    warning = fetch_meta.get("completeness_warning")
    detail = warning or (
        f"fetched_report_count={fetched!r} total_count={total!r} "
        f"snapshot_id={snapshot_id!r}"
    )
    raise IncompleteSnapshotError(
        "Refusing to analyze or publish an incomplete Hub observation snapshot. "
        f"{detail}"
    )


def analysis_config_dict(config: Any) -> dict[str, Any]:
    """Stable JSON-friendly AnalysisConfig summary for provenance."""
    return {
        "event_gap_seconds": float(config.event_gap_seconds),
        "max_association_distance_m": float(config.max_association_distance_m),
        "allow_long_range_sim_ears": bool(config.allow_long_range_sim_ears),
        "association_temporal_gap_seconds": float(
            config.association_temporal_gap_seconds
        ),
        "min_ears_for_event": int(config.min_ears_for_event),
        "min_promote_score": int(config.min_promote_score),
        "include_cross_ear_correlation": bool(config.include_cross_ear_correlation),
    }


def _iso_utc(value: datetime | None) -> str | None:
    if value is None:
        return None
    if value.tzinfo is None or value.utcoffset() is None:
        return None
    return value.astimezone(UTC).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z"
    )


def _finite_coord(value: Any) -> float | None:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return None
    number = float(value)
    if not math.isfinite(number):
        return None
    return number


def scratch_object_ids_for_dataset(
    objects: list[dict[str, Any]],
    dataset: str,
) -> list[str]:
    """Return scratch object ids whose metadata.dataset matches ``dataset``.

    Used for safe replacement: delete only these ids, never the whole namespace.
    """
    matched: list[str] = []
    for item in objects:
        if not isinstance(item, dict):
            continue
        object_id = item.get("id")
        if not isinstance(object_id, str) or not object_id:
            continue
        metadata = item.get("metadata")
        if not isinstance(metadata, dict):
            continue
        if metadata.get("dataset") == dataset:
            matched.append(object_id)
    return matched


def serialize_analysis_map_layer(
    result: Any,
    *,
    dataset: str,
    dataset_title: str,
    run_id: str,
    run_timestamp: datetime,
    title: str | None = None,
) -> dict[str, Any]:
    """Build one analysis-only map-layer content object for Analysis scratch.

    Does not emit route/ground-truth layers. Omits confidence unless
    ``LocalizationResult.calibrated_uncertainty_m`` is a finite calibrated value
    (today always absent — so confidence is never fabricated).
    """
    if not dataset or not isinstance(dataset, str):
        raise ValueError("dataset must be a non-empty string")
    if not dataset_title or not isinstance(dataset_title, str):
        raise ValueError("dataset_title must be a non-empty string")
    if not run_id or not isinstance(run_id, str):
        raise ValueError("run_id must be a non-empty string")

    snapshot = result.snapshot
    correlation: CorrelationResult | None = result.correlation
    config = snapshot.config
    fetch_meta = dict(snapshot.fetch_meta or {})

    observations_out: list[dict[str, Any]] = []
    events_out: list[dict[str, Any]] = []
    localizations_out: list[dict[str, Any]] = []

    assembled = list(correlation.observations) if correlation is not None else []
    events = list(correlation.events) if correlation is not None else []

    membership_by_observation: dict[str, str] = {}
    for event in events:
        for observation_id in event.observation_ids:
            membership_by_observation[observation_id] = event.event_id

    for item in assembled:
        lat = _finite_coord(item.installation_latitude)
        lon = _finite_coord(item.installation_longitude)
        if lat is None or lon is None:
            continue
        marker: dict[str, Any] = {
            "id": item.observation_id,
            "lat": lat,
            "lon": lon,
            "label": f"{item.node_id} (installation)",
            "node_id": item.node_id,
            "coordinate_source": "ear_installation",
            "report_count": int(item.report_count),
        }
        time_utc = _iso_utc(item.interval_start or item.first_time)
        if time_utc is not None:
            marker["time_utc"] = time_utc
        interval_start = _iso_utc(item.interval_start or item.first_time)
        interval_end = _iso_utc(item.interval_end or item.last_time)
        if interval_start is not None:
            marker["interval_start_utc"] = interval_start
        if interval_end is not None:
            marker["interval_end_utc"] = interval_end
        if isinstance(item.signal_level_dbfs, (int, float)) and not isinstance(
            item.signal_level_dbfs, bool
        ):
            marker["signal_level_dbfs"] = float(item.signal_level_dbfs)
        event_id = membership_by_observation.get(item.observation_id)
        if event_id is not None:
            marker["event_id"] = event_id
        observations_out.append(marker)

    published_count = 0
    withheld_count = 0
    for event in events:
        localization = event.localization
        published = localization_is_published(localization)
        if published:
            published_count += 1
        else:
            withheld_count += 1

        event_marker: dict[str, Any] = {
            "id": event.event_id,
            "label": f"Event {event.event_id}",
            "member_count": len(event.ear_ids),
            "membership_ear_count": len(event.ear_ids),
            "membership_observation_count": len(event.observation_ids),
            "membership_ear_ids": list(event.ear_ids),
            "membership_observation_ids": list(event.observation_ids),
            "status": "promoted",
            "score": event.score,
        }
        promoted_at = _iso_utc(event.promoted_at or event.created_at)
        if promoted_at is not None:
            event_marker["time_utc"] = promoted_at

        contributing: list[str] = []
        if published and localization is not None:
            assert localization.latitude is not None
            assert localization.longitude is not None
            event_marker["lat"] = float(localization.latitude)
            event_marker["lon"] = float(localization.longitude)
            event_marker["coordinate_source"] = "published_localization"
            contributing = list(localization.contributing_ears)
            event_marker["localization_contributing_ears"] = contributing
            event_marker["localization_contributing_ear_count"] = len(contributing)
            event_marker["localization_status"] = localization.status
            event_marker.update(localization_evidence_fields(localization))
        else:
            event_marker["localization_contributing_ears"] = []
            event_marker["localization_contributing_ear_count"] = 0
            event_marker["localization_status"] = (
                localization.status if localization is not None else "unavailable"
            )
            event_marker["position_withheld"] = True
            if localization is not None:
                # Diagnostics may still help explain withheld/unavailable cases.
                event_marker.update(localization_evidence_fields(localization))
        events_out.append(event_marker)

        if not published or localization is None:
            continue

        loc_marker: dict[str, Any] = {
            "id": f"loc-{event.event_id}",
            "lat": float(localization.latitude),
            "lon": float(localization.longitude),
            "label": "Estimated position",
            "status": localization.status,
            "event_id": event.event_id,
            "membership_ear_ids": list(event.ear_ids),
            "membership_ear_count": len(event.ear_ids),
            "membership_observation_ids": list(event.observation_ids),
            "membership_observation_count": len(event.observation_ids),
            "contributing_ears": list(localization.contributing_ears),
            "coordinate_source": "published_localization",
            "selection_method": localization.selection_method,
            "not_ground_truth": True,
        }
        loc_marker.update(localization_evidence_fields(localization))
        loc_time = _iso_utc(localization.evaluation_time_utc)
        if loc_time is not None:
            loc_marker["time_utc"] = loc_time
        # Only attach confidence when a calibrated model exists (never fabricate).
        calibrated = localization.calibrated_uncertainty_m
        if (
            isinstance(calibrated, (int, float))
            and not isinstance(calibrated, bool)
            and math.isfinite(float(calibrated))
        ):
            loc_marker["calibrated_uncertainty_m"] = float(calibrated)
        localizations_out.append(loc_marker)

    run_ts = run_timestamp.astimezone(UTC).replace(microsecond=0)
    provenance = {
        "run_id": run_id,
        "run_timestamp_utc": run_ts.isoformat().replace("+00:00", "Z"),
        "snapshot_id": snapshot.snapshot_id,
        "fetched_report_count": fetch_meta.get("fetched_report_count", result.report_count),
        "complete_snapshot": fetch_meta.get("complete_snapshot"),
        "report_count": result.report_count,
        "unique_observation_count": result.unique_observation_count,
        "observations_assembled": len(assembled),
        "events": len(events),
        "localizations_published": published_count,
        "localizations_withheld": withheld_count,
        "analysis_config": analysis_config_dict(config),
        "scratch_ephemeral": True,
        "not_durable_event_history": True,
    }

    layer_title = title or f"{dataset_title} analysis"
    return {
        "kind": "map-layer",
        "dataset": dataset,
        "dataset_title": dataset_title,
        "layer": "analysis",
        "title": layer_title,
        "role": "analysis",
        "description": (
            "Estimated Cross-EAR analysis overlay from a Hub Observation snapshot. "
            "Temporary Analysis scratch data — not durable Event history. "
            "Observation markers use EAR installation coordinates; "
            "localization markers use published estimates only (not ground truth)."
        ),
        "provenance": provenance,
        "observations": observations_out,
        "events": events_out,
        "localizations": localizations_out,
    }


def format_run_counts(counts: AnalysisRunCounts) -> list[str]:
    """Human-readable count lines for CLI stdout."""
    return [
        f"reports_fetched={counts.reports_fetched}",
        f"reports_after_filter={counts.reports_after_filter}",
        f"observations_assembled={counts.observations_assembled}",
        f"observations_after_filter={counts.observations_after_filter}",
        f"events={counts.events}",
        f"localizations_published={counts.localizations_published}",
        f"localizations_withheld={counts.localizations_withheld}",
    ]


def counts_from_result(
    result: Any,
    *,
    reports_fetched: int,
    reports_after_filter: int,
    observations_after_filter: int | None = None,
) -> AnalysisRunCounts:
    correlation = result.correlation
    assembled = len(correlation.observations) if correlation is not None else 0
    events = len(correlation.events) if correlation is not None else 0
    published, withheld = count_localization_outcomes(correlation)
    remaining = (
        assembled if observations_after_filter is None else observations_after_filter
    )
    return AnalysisRunCounts(
        reports_fetched=reports_fetched,
        reports_after_filter=reports_after_filter,
        observations_assembled=assembled,
        observations_after_filter=remaining,
        events=events,
        localizations_published=published,
        localizations_withheld=withheld,
    )

