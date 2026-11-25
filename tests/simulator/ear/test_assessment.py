from __future__ import annotations

import sys
from datetime import UTC, datetime
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "hub" / "bootstrap"))
sys.path.insert(0, str(ROOT / "simulator" / "ear"))
sys.path.insert(0, str(ROOT / "scripts"))

from app.schemas import ObservationBody
from assessment import (
    SOURCE_ASSESSMENT_PATHS,
    AssessmentGenerationError,
    annotate_assessment_progression,
    assessment_path_depth,
    build_assessment,
)
from simulate_observation import attach_l0_with_assessment, make_observation
from simulate_scenario import make_scenario_observation


@pytest.mark.parametrize(
    ("source_family", "expected"),
    [
        ("aircraft", ("AIRCRAFT", "Plane")),
        ("helicopter", ("AIRCRAFT", "Helicopter")),
        ("vehicle", ("VEHICLE", "Light", "Petrol")),
    ],
)
def test_source_family_assessment_mappings_are_exact(
    source_family: str, expected: tuple[str, ...]
) -> None:
    assert SOURCE_ASSESSMENT_PATHS[source_family] == expected
    assert set(SOURCE_ASSESSMENT_PATHS) == {"aircraft", "helicopter", "vehicle"}


@pytest.mark.parametrize(
    ("source_family", "emission_kind", "updates_before", "expected_depth"),
    [
        ("vehicle", "start", 0, 1),
        ("vehicle", "periodic", 0, 2),
        ("vehicle", "periodic", 1, 3),
        ("vehicle", "end", 0, 1),
        ("vehicle", "end", 1, 2),
        ("vehicle", "end", 2, 3),
        ("helicopter", "periodic", 1, 2),
        ("aircraft", "periodic", 0, 2),
        ("vehicle", "complete", 0, 1),
    ],
)
def test_assessment_depth_tracks_report_phase(
    source_family: str,
    emission_kind: str,
    updates_before: int,
    expected_depth: int,
) -> None:
    assert (
        assessment_path_depth(
            source_family,
            emission_kind,
            periodic_updates_before=updates_before,
        )
        == expected_depth
    )


def test_assessment_generation_rejects_unknown_sources_and_invalid_confidence() -> None:
    with pytest.raises(AssessmentGenerationError, match="no simulator assessment mapping"):
        build_assessment("boat", 0.7, path_depth=1)
    with pytest.raises(AssessmentGenerationError, match=r"confidence must be finite and in \[0, 1\]"):
        build_assessment("vehicle", 1.1, path_depth=1)


@pytest.mark.parametrize(
    ("source_family", "path_depth", "classification_confidence", "expected"),
    [
        ("vehicle", 1, 0.64, [0.64]),
        ("vehicle", 2, 0.64, [0.69, 0.64]),
        ("vehicle", 3, 0.64, [0.74, 0.69, 0.64]),
        ("vehicle", 3, 0.98, [1.0, 1.0, 0.98]),
        ("vehicle", 3, 0.0, [0.1, 0.05, 0.0]),
    ],
)
def test_assessment_confidence_is_per_level_bounded_and_root_adjusted(
    source_family: str,
    path_depth: int,
    classification_confidence: float,
    expected: list[float],
) -> None:
    assessment = build_assessment(
        source_family, classification_confidence, path_depth=path_depth
    )
    assert [node["confidence"] for node in assessment["path"]] == expected


def _scenario_events(source_family: str, kinds: list[str]) -> list[dict]:
    return [
        {
            "observation_id": "00000000-0000-0000-0000-000000000001",
            "source_family": source_family,
            "emission_kind": kind,
            "at_seconds": float(index),
            "classification_confidence": 0.73,
        }
        for index, kind in enumerate(kinds)
    ]


def _build_scenario_reports(events: list[dict]) -> list[dict]:
    return [
        make_scenario_observation(
            "SIM-EAR-001",
            index + 1,
            "SIM-SOURCE-001",
            event,
            datetime(2026, 11, 25, tzinfo=UTC),
            1_000_000,
        )
        for index, event in enumerate(events)
    ]


@pytest.mark.parametrize(
    ("source_family", "kinds", "expected_paths"),
    [
        (
            "helicopter",
            ["start", "periodic", "end"],
            [["AIRCRAFT"], ["AIRCRAFT", "Helicopter"], ["AIRCRAFT", "Helicopter"]],
        ),
        (
            "aircraft",
            ["start", "periodic", "end"],
            [["AIRCRAFT"], ["AIRCRAFT", "Plane"], ["AIRCRAFT", "Plane"]],
        ),
        (
            "vehicle",
            ["start", "periodic", "periodic", "end"],
            [
                ["VEHICLE"],
                ["VEHICLE", "Light"],
                ["VEHICLE", "Light", "Petrol"],
                ["VEHICLE", "Light", "Petrol"],
            ],
        ),
        ("vehicle", ["start", "end"], [["VEHICLE"], ["VEHICLE"]]),
    ],
)
def test_scenario_progression_populates_dual_assessments_and_validates(
    source_family: str,
    kinds: list[str],
    expected_paths: list[list[str]],
) -> None:
    events = annotate_assessment_progression(_scenario_events(source_family, kinds))
    reports = _build_scenario_reports(events)

    for event, report, expected_path in zip(events, reports, expected_paths, strict=True):
        classification = report["acoustic"]["classification"]
        assessment = report["acoustic"]["assessment"]
        assert classification["source_family"] == source_family
        assert classification["confidence"] == 0.73
        assert classification["hints"] == {"simulated_source_id": "SIM-SOURCE-001"}
        assert assessment["schema"] == "ear_hyp_v2"
        assert assessment["version"] == 1
        assert [node["id"] for node in assessment["path"]] == expected_path
        assert assessment["path"][-1]["confidence"] == classification["confidence"]
        assert all(0 <= node["confidence"] <= 1 for node in assessment["path"])
        ObservationBody.model_validate(report)


def test_observation_simulator_preserves_classification_and_adds_assessment() -> None:
    report = make_observation("SIM-EAR-001", vehicle_id="SIM-VEHICLE-77")
    classification = report["acoustic"]["classification"]
    assessment = report["acoustic"]["assessment"]

    assert classification == {
        "schema": "l2",
        "version": 1,
        "source_family": "vehicle",
        "confidence": 0.72,
        "hints": {"simulated_source_id": "SIM-VEHICLE-77"},
    }
    assert assessment["schema"] == "ear_hyp_v2"
    assert assessment["version"] == 1
    assert [node["id"] for node in assessment["path"]] == ["VEHICLE", "Light"]
    assert assessment["path"][-1]["confidence"] == classification["confidence"]
    assert assessment["path"][0]["confidence"] == pytest.approx(0.77)
    ObservationBody.model_validate(report)


def test_observation_start_fixture_emits_root_only_assessment() -> None:
    report = make_observation(
        "SIM-EAR-001",
        include_l0=True,
        l0_scenario="insufficient_data_nulls",
    )

    assert report["context"]["emission_kind"] == "start"
    assert [node["id"] for node in report["acoustic"]["assessment"]["path"]] == [
        "VEHICLE"
    ]
    ObservationBody.model_validate(report)


def test_l0_attachment_refreshes_assessment_for_changed_emission_kind() -> None:
    observation = make_observation("SIM-EAR-001")
    assert [node["id"] for node in observation["acoustic"]["assessment"]["path"]] == [
        "VEHICLE",
        "Light",
    ]

    updated = attach_l0_with_assessment(
        observation, scenario="insufficient_data_nulls"
    )

    assert updated["context"]["emission_kind"] == "start"
    assert [node["id"] for node in updated["acoustic"]["assessment"]["path"]] == [
        "VEHICLE"
    ]
    assert updated["acoustic"]["assessment"]["path"][0]["confidence"] == pytest.approx(
        0.72
    )
    ObservationBody.model_validate(updated)


def test_protocol_accepts_independent_non_monotonic_path_confidence() -> None:
    report = _build_scenario_reports(
        annotate_assessment_progression(
            _scenario_events("vehicle", ["start", "periodic", "periodic"])
        )
    )[-1]
    for node, confidence in zip(
        report["acoustic"]["assessment"]["path"],
        [0.2, 0.9, 0.4],
        strict=True,
    ):
        node["confidence"] = confidence

    ObservationBody.model_validate(report)


def test_assessment_is_deterministic_for_identical_scenario_inputs() -> None:
    events = _scenario_events(
        "vehicle", ["start", "periodic", "periodic", "end"]
    )
    first = _build_scenario_reports(annotate_assessment_progression(events))
    second = _build_scenario_reports(annotate_assessment_progression(events))

    assert [report["acoustic"]["assessment"] for report in first] == [
        report["acoustic"]["assessment"] for report in second
    ]
