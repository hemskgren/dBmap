"""Simulator L0 fixtures match Hub ObservationBody and Ear field names."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "hub" / "bootstrap"))
sys.path.insert(0, str(ROOT / "simulator" / "ear"))

from app.schemas import ObservationBody
from l0_measurements import (
    SCENARIO_IDS,
    assert_band_hl_consistent,
    attach_l0_extension,
    make_l0_measurements,
)
from simulate_observation import make_observation


def test_legacy_observation_still_validates() -> None:
    body = make_observation("SIM-EAR-001")
    assert "emission_kind" not in body
    assert "measurements" not in body
    ObservationBody.model_validate(body)


@pytest.mark.parametrize("scenario", SCENARIO_IDS)
def test_extended_scenarios_validate(scenario: str) -> None:
    body = make_observation(
        "SIM-EAR-001",
        include_l0=True,
        l0_scenario=scenario,
    )
    assert body["protocol_version"] == 1
    assert body["emission_kind"] in {"start", "periodic", "end"}
    assert body["measurements"]["schema"] == "l0"
    assert_band_hl_consistent(body["measurements"])
    parsed = ObservationBody.model_validate(body)
    assert parsed.measurements is not None
    dumped = parsed.model_dump(mode="json", by_alias=True)
    assert dumped["measurements"]["schema"] == "l0"
    # Nulls stay null where the fixture sets them.
    if scenario == "insufficient_data_nulls":
        assert dumped["measurements"]["t1"]["rms_stdev_db"] is None
        assert dumped["measurements"]["t2"]["rms_slope_db_per_s"] is None
        assert dumped["measurements"]["t6"]["hl_slope_db_per_s"] is None


def test_band_hl_consistency_helper_catches_mismatch() -> None:
    bad = make_l0_measurements("varying_level")
    bad["hl"]["mean_db"] = 99.0
    with pytest.raises(AssertionError, match="hl.mean_db"):
        assert_band_hl_consistent(bad)


def test_attach_preserves_existing_fields() -> None:
    base = make_observation("SIM-EAR-002", sequence_number=9)
    oid = base["observation_id"]
    extended = attach_l0_extension(base, scenario="clipped_windows")
    assert extended["observation_id"] == oid
    assert extended["sequence_number"] == 9
    assert extended["measurements"]["clipped_window_count"] == 2
    ObservationBody.model_validate(extended)


def test_measurement_field_set_matches_contract_keys() -> None:
    m = make_l0_measurements("quiet_ambient")
    assert set(m) == {
        "schema",
        "version",
        "band_plan_version",
        "crest_threshold_version",
        "crest_threshold_db",
        "mic_index",
        "sample_rate_hz",
        "analysis_window_ms",
        "window_count",
        "clipped_window_count",
        "n_crest",
        "rms",
        "bands",
        "hl",
        "t1",
        "t2",
        "t3",
        "t6",
    }
