"""Shared forward-looking EAR assessment generation for simulator reports."""

import math
from typing import Any

SOURCE_ASSESSMENT_PATHS = {
    "aircraft": ("AIRCRAFT", "Plane"),
    "helicopter": ("AIRCRAFT", "Helicopter"),
    "vehicle": ("VEHICLE", "Light", "Petrol"),
}


class AssessmentGenerationError(ValueError):
    pass


def source_assessment_path(source_family: str) -> tuple[str, ...]:
    try:
        return SOURCE_ASSESSMENT_PATHS[source_family]
    except KeyError as exc:
        raise AssessmentGenerationError(
            f"no simulator assessment mapping for source_family {source_family!r}; "
            f"supported values: {', '.join(sorted(SOURCE_ASSESSMENT_PATHS))}"
        ) from exc


def assessment_path_depth(
    source_family: str,
    emission_kind: str,
    *,
    periodic_updates_before: int = 0,
) -> int:
    """Return the prefix depth for a report, without inventing taxonomy nodes."""
    path = source_assessment_path(source_family)
    if (
        isinstance(periodic_updates_before, bool)
        or not isinstance(periodic_updates_before, int)
        or periodic_updates_before < 0
    ):
        raise AssessmentGenerationError(
            "periodic_updates_before must be a non-negative integer"
        )

    if emission_kind == "start":
        return 1
    if emission_kind == "periodic":
        return min(len(path), 2 + periodic_updates_before)
    if emission_kind == "end":
        return min(len(path), 1 + periodic_updates_before)
    if emission_kind == "complete":
        return 1
    raise AssessmentGenerationError(
        f"unsupported emission_kind for assessment: {emission_kind!r}"
    )


def build_assessment(
    source_family: str,
    confidence: float,
    *,
    path_depth: int,
) -> dict[str, Any]:
    """Build an ear_hyp_v2 path using the existing source confidence."""
    if (
        isinstance(confidence, bool)
        or not isinstance(confidence, (int, float))
        or not math.isfinite(confidence)
        or not 0 <= confidence <= 1
    ):
        raise AssessmentGenerationError(
            "assessment confidence must be finite and in [0, 1]"
        )

    path = source_assessment_path(source_family)
    if isinstance(path_depth, bool) or not isinstance(path_depth, int):
        raise AssessmentGenerationError("assessment path depth must be an integer")
    if not 1 <= path_depth <= len(path):
        raise AssessmentGenerationError(
            f"assessment path depth must be between 1 and {len(path)} "
            f"for source_family {source_family!r}"
        )
    path_nodes = []
    for index, node_id in enumerate(path[:path_depth]):
        ancestor_levels = path_depth - index - 1
        node_confidence = (
            float(confidence)
            if ancestor_levels == 0
            else min(1.0, round(float(confidence) + 0.05 * ancestor_levels, 10))
        )
        path_nodes.append({"id": node_id, "confidence": node_confidence})
    return {
        "schema": "ear_hyp_v2",
        "version": 1,
        "path": path_nodes,
    }


def annotate_assessment_progression(
    events: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    """Annotate scenario reports with deterministic progressive path depths."""
    prepared = [dict(event) for event in events]
    periodic_updates: dict[object, int] = {}
    for index, event in enumerate(prepared):
        source_family = event.get("source_family")
        emission_kind = event.get("emission_kind", "complete")
        observation_id = event.get("observation_id")
        key: object = (
            observation_id
            if isinstance(observation_id, str) and observation_id
            else ("untracked", index)
        )
        updates_before = periodic_updates.get(key, 0)
        if emission_kind == "start":
            updates_before = 0
            periodic_updates[key] = 0
        elif emission_kind == "periodic":
            periodic_updates[key] = updates_before + 1

        if not isinstance(source_family, str):
            raise AssessmentGenerationError(
                "scenario observation is missing source_family"
            )
        event["_assessment_path_depth"] = assessment_path_depth(
            source_family,
            emission_kind,
            periodic_updates_before=updates_before,
        )
    return prepared
