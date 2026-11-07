"""Scenario ground-truth trajectory helpers (evaluation only).

Ground truth is derived from scenario movement metadata and must never be
passed into association or localization inputs.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from typing import Any

from lib.cross_ear_localization import (
    GroundTruthEvaluation,
    LocalizationResult,
    evaluate_against_ground_truth,
    source_position_on_route,
)


@dataclass(frozen=True)
class ScenarioGroundTruthTrajectory:
    """Separate ground-truth route for post-hoc localization evaluation."""

    ground_truth_id: str
    source_family: str
    start: dict[str, float]
    end: dict[str, float]
    speed_m_s: float
    distance_m: float
    duration_seconds: float

    def position_at(self, at_seconds: float) -> tuple[float, float]:
        """Return ground-truth lat/lon at scenario-relative seconds."""
        return source_position_on_route(
            self.start,
            self.end,
            speed_m_s=self.speed_m_s,
            at_seconds=at_seconds,
        )

    def position_at_wall_time(
        self,
        *,
        evaluation_time_utc: datetime,
        scenario_started_at_utc: datetime,
    ) -> tuple[float, float]:
        """Return GT position for a localization evaluation timestamp."""
        at_seconds = (evaluation_time_utc - scenario_started_at_utc).total_seconds()
        return self.position_at(at_seconds)


def ground_truth_from_loaded_scenario(
    scenario: dict[str, Any],
) -> ScenarioGroundTruthTrajectory:
    """Build a GT trajectory from a scenario returned by ``load_scenario``."""
    movement = scenario["movement"]
    return ScenarioGroundTruthTrajectory(
        ground_truth_id=str(scenario["source_id"]),
        source_family=str(scenario["source_family"]),
        start=dict(movement["start"]),
        end=dict(movement["end"]),
        speed_m_s=float(movement["speed_m_s"]),
        distance_m=float(movement["distance_m"]),
        duration_seconds=float(movement["duration_seconds"]),
    )


def evaluate_localization_against_trajectory(
    localization: LocalizationResult,
    trajectory: ScenarioGroundTruthTrajectory,
    *,
    scenario_started_at_utc: datetime,
) -> GroundTruthEvaluation:
    """Post-hoc GT evaluation; never feeds association or localization math."""
    if localization.evaluation_time_utc is None:
        return evaluate_against_ground_truth(
            localization,
            ground_truth_latitude=trajectory.start["latitude"],
            ground_truth_longitude=trajectory.start["longitude"],
            ground_truth_time_utc=None,
        )
    gt_lat, gt_lon = trajectory.position_at_wall_time(
        evaluation_time_utc=localization.evaluation_time_utc,
        scenario_started_at_utc=scenario_started_at_utc,
    )
    return evaluate_against_ground_truth(
        localization,
        ground_truth_latitude=gt_lat,
        ground_truth_longitude=gt_lon,
        ground_truth_time_utc=localization.evaluation_time_utc,
    )
