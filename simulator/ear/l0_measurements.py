"""Deterministic combined-scene L0 measurement fixtures for Observation v1 extension.

Field names, types, units, and nullability match the Ear serializer /
docs/ear-l0-measurement-contract.md. Not per-source.

`bands.mean_dbfs`, `bands.fraction`, and `hl.mean_db` are always derived from the
same linear band energies (same production formulas as Ear finalize).
"""

from __future__ import annotations

import math
from collections.abc import Sequence
from copy import deepcopy
from typing import Any, Literal

EmissionKind = Literal["start", "periodic", "end", "complete"]

SCENARIO_IDS = (
    "quiet_ambient",
    "stable_tonal",
    "varying_level",
    "high_crest_transient",
    "clipped_windows",
    "insufficient_data_nulls",
)


def bands_and_hl_from_linear_energies(
    mean_linear_energies: Sequence[float],
) -> tuple[dict[str, list[float]], dict[str, float]]:
    """Match Ear finalize: mean_dbfs=10*log10(E_k), fraction=E_k/ΣE, hl from Σ high/low."""
    if len(mean_linear_energies) != 4:
        raise ValueError("expected 4 band energies")
    energies = [float(e) for e in mean_linear_energies]
    if any(e <= 0.0 for e in energies):
        raise ValueError("linear energies must be > 0 for non-null band/HL fixtures")
    total = sum(energies)
    mean_dbfs = [10.0 * math.log10(e) for e in energies]
    fraction = [e / total for e in energies]
    e_low = energies[0] + energies[1]
    e_high = energies[2] + energies[3]
    hl_mean_db = 10.0 * math.log10(e_high / e_low)
    return (
        {"mean_dbfs": mean_dbfs, "fraction": fraction},
        {"mean_db": hl_mean_db},
    )


def assert_band_hl_consistent(
    measurements: dict[str, Any], *, tol: float = 1e-4
) -> None:
    """Raise AssertionError if band means / fractions / hl disagree (when present)."""
    bands = measurements.get("bands") or {}
    means = bands.get("mean_dbfs")
    fracs = bands.get("fraction")
    hl = (measurements.get("hl") or {}).get("mean_db")
    if means is None or fracs is None:
        return
    if any(v is None for v in means) or any(v is None for v in fracs):
        if hl is not None:
            raise AssertionError("hl.mean_db set while band means/fractions are null")
        return
    energies = [10.0 ** (m / 10.0) for m in means]
    total = sum(energies)
    expect_frac = [e / total for e in energies]
    for got, exp in zip(fracs, expect_frac, strict=True):
        if abs(got - exp) > tol:
            raise AssertionError(
                f"bands.fraction inconsistent with mean_dbfs: {fracs} vs {expect_frac}"
            )
    e_low = energies[0] + energies[1]
    e_high = energies[2] + energies[3]
    expect_hl = 10.0 * math.log10(e_high / e_low)
    if hl is None:
        raise AssertionError("hl.mean_db null while band energies are valid")
    if abs(hl - expect_hl) > tol:
        raise AssertionError(f"hl.mean_db {hl} != expected {expect_hl} from band means")


def _base_meta(**overrides: Any) -> dict[str, Any]:
    base = {
        "schema": "l0",
        "version": 1,
        "band_plan_version": 1,
        "crest_threshold_version": 1,
        "crest_threshold_db": 12.0,
        "mic_index": 0,
        "sample_rate_hz": 16000,
        "analysis_window_ms": 1000,
    }
    base.update(overrides)
    return base


def empty_l0_measurements() -> dict[str, Any]:
    """Minimal valid L0 object (all metric slots null, window_count=0)."""
    return {
        **_base_meta(),
        "window_count": 0,
        "clipped_window_count": 0,
        "n_crest": 0,
        "rms": {"mean_dbfs": None, "min_dbfs": None, "max_dbfs": None},
        "bands": {
            "mean_dbfs": [None, None, None, None],
            "fraction": [None, None, None, None],
        },
        "hl": {"mean_db": None},
        "t1": {"rms_stdev_db": None, "rms_range_db": None},
        "t2": {"rms_slope_db_per_s": None},
        "t3": {
            "crest_max_db": None,
            "high_crest_count": None,
            "high_crest_rate": None,
        },
        "t6": {"hl_slope_db_per_s": None},
    }


def make_l0_measurements(scenario: str) -> dict[str, Any]:
    """Return a complete measurements object for a named deterministic scene."""
    if scenario == "quiet_ambient":
        # Low-band-weighted quiet scene; energies well above silence floor.
        bands, hl = bands_and_hl_from_linear_energies(
            [4.0e-6, 1.2e-6, 2.0e-7, 6.0e-8]
        )
        return {
            **_base_meta(),
            "window_count": 5,
            "clipped_window_count": 0,
            "n_crest": 5,
            "rms": {"mean_dbfs": -62.0, "min_dbfs": -64.0, "max_dbfs": -60.0},
            "bands": bands,
            "hl": hl,
            "t1": {"rms_stdev_db": 1.2, "rms_range_db": 4.0},
            "t2": {"rms_slope_db_per_s": 0.05},
            "t3": {
                "crest_max_db": 8.0,
                "high_crest_count": 0,
                "high_crest_rate": 0.0,
            },
            "t6": {"hl_slope_db_per_s": 0.02},
        }
    if scenario == "stable_tonal":
        # B1-dominant stable tone.
        bands, hl = bands_and_hl_from_linear_energies(
            [1.0e-5, 4.0e-4, 2.0e-6, 5.0e-7]
        )
        return {
            **_base_meta(),
            "window_count": 5,
            "clipped_window_count": 0,
            "n_crest": 5,
            "rms": {"mean_dbfs": -36.0, "min_dbfs": -36.5, "max_dbfs": -35.5},
            "bands": bands,
            "hl": hl,
            "t1": {"rms_stdev_db": 0.3, "rms_range_db": 1.0},
            "t2": {"rms_slope_db_per_s": 0.0},
            "t3": {
                "crest_max_db": 6.0,
                "high_crest_count": 0,
                "high_crest_rate": 0.0,
            },
            "t6": {"hl_slope_db_per_s": 0.0},
        }
    if scenario == "varying_level":
        bands, hl = bands_and_hl_from_linear_energies(
            [4.0e-5, 3.0e-5, 2.0e-5, 1.0e-5]
        )
        return {
            **_base_meta(),
            "window_count": 5,
            "clipped_window_count": 0,
            "n_crest": 5,
            "rms": {"mean_dbfs": -42.0, "min_dbfs": -55.0, "max_dbfs": -28.0},
            "bands": bands,
            "hl": hl,
            "t1": {"rms_stdev_db": 9.5, "rms_range_db": 27.0},
            "t2": {"rms_slope_db_per_s": 4.5},
            "t3": {
                "crest_max_db": 11.0,
                "high_crest_count": 0,
                "high_crest_rate": 0.0,
            },
            "t6": {"hl_slope_db_per_s": 0.4},
        }
    if scenario == "high_crest_transient":
        # Slightly high-band weighted.
        bands, hl = bands_and_hl_from_linear_energies(
            [2.0e-5, 3.0e-5, 3.5e-5, 1.5e-5]
        )
        return {
            **_base_meta(),
            "window_count": 5,
            "clipped_window_count": 0,
            "n_crest": 5,
            "rms": {"mean_dbfs": -40.0, "min_dbfs": -48.0, "max_dbfs": -32.0},
            "bands": bands,
            "hl": hl,
            "t1": {"rms_stdev_db": 5.0, "rms_range_db": 16.0},
            "t2": {"rms_slope_db_per_s": -0.2},
            "t3": {
                "crest_max_db": 18.0,
                "high_crest_count": 2,
                "high_crest_rate": 0.4,
            },
            "t6": {"hl_slope_db_per_s": -0.15},
        }
    if scenario == "clipped_windows":
        bands, hl = bands_and_hl_from_linear_energies(
            [1.0e-2, 1.5e-2, 2.0e-2, 0.5e-2]
        )
        return {
            **_base_meta(),
            "window_count": 5,
            "clipped_window_count": 2,
            "n_crest": 3,
            "rms": {"mean_dbfs": -18.0, "min_dbfs": -30.0, "max_dbfs": -6.0},
            "bands": bands,
            "hl": hl,
            "t1": {"rms_stdev_db": 8.0, "rms_range_db": 24.0},
            "t2": {"rms_slope_db_per_s": 1.0},
            "t3": {
                "crest_max_db": 10.0,
                "high_crest_count": 0,
                "high_crest_rate": 0.0,
            },
            "t6": {"hl_slope_db_per_s": 0.1},
        }
    if scenario == "insufficient_data_nulls":
        # START-like short interval: N=1 → T1/T2/T6 null; crest present.
        bands, hl = bands_and_hl_from_linear_energies(
            [4.0e-5, 3.0e-5, 2.0e-5, 1.0e-5]
        )
        m = empty_l0_measurements()
        m.update(
            {
                "window_count": 1,
                "clipped_window_count": 0,
                "n_crest": 1,
                "rms": {"mean_dbfs": -50.0, "min_dbfs": -50.0, "max_dbfs": -50.0},
                "bands": bands,
                "hl": hl,
                "t1": {"rms_stdev_db": None, "rms_range_db": None},
                "t2": {"rms_slope_db_per_s": None},
                "t3": {
                    "crest_max_db": 7.0,
                    "high_crest_count": 0,
                    "high_crest_rate": 0.0,
                },
                "t6": {"hl_slope_db_per_s": None},
            }
        )
        return m
    raise ValueError(f"unknown L0 scenario: {scenario!r}; choose from {SCENARIO_IDS}")


def default_emission_kind(scenario: str) -> EmissionKind:
    if scenario == "insufficient_data_nulls":
        return "start"
    return "periodic"


def default_termination_reason(kind: EmissionKind) -> str | None:
    if kind in ("end", "complete"):
        return "natural"
    return None


def attach_l0_extension(
    observation: dict[str, Any],
    *,
    scenario: str = "quiet_ambient",
    emission_kind: EmissionKind | None = None,
    termination_reason: str | None = None,
) -> dict[str, Any]:
    """Return a copy of a hierarchical observation with L0 scene + emission context."""
    out = deepcopy(observation)
    kind = emission_kind or default_emission_kind(scenario)
    out["context"]["emission_kind"] = kind
    term = termination_reason if termination_reason is not None else default_termination_reason(kind)
    if term is not None:
        out["context"]["termination_reason"] = term
    else:
        out["context"].pop("termination_reason", None)
    out["acoustic"]["measurements"] = make_l0_measurements(scenario)
    return out


def empty_l1_signature() -> dict[str, Any]:
    """Valid empty L1 object (window_count=0, all feature means/stdevs null)."""
    null_stats = {"mean": None, "stdev": None}
    return {
        "schema": "l1",
        "version": 1,
        "sample_rate_hz": 16000,
        "analysis_window_ms": 1000,
        "fft_size": 1024,
        "freq_min_hz": 80.0,
        "freq_max_hz": 7600.0,
        "window_count": 0,
        "invalid_window_count": 0,
        "centroid_hz": dict(null_stats),
        "bandwidth_hz": dict(null_stats),
        "dominant_hz": dict(null_stats),
        "flatness": dict(null_stats),
    }


def example_l1_signature() -> dict[str, Any]:
    """Deterministic populated L1 fixture (1 kHz-ish tonal scene)."""
    return {
        "schema": "l1",
        "version": 1,
        "sample_rate_hz": 16000,
        "analysis_window_ms": 1000,
        "fft_size": 1024,
        "freq_min_hz": 80.0,
        "freq_max_hz": 7600.0,
        "window_count": 5,
        "invalid_window_count": 0,
        "centroid_hz": {"mean": 1002.3, "stdev": 12.5},
        "bandwidth_hz": {"mean": 48.0, "stdev": 6.2},
        "dominant_hz": {"mean": 1000.0, "stdev": 0.0},
        "flatness": {"mean": 0.082, "stdev": 0.011},
    }


def attach_l1_signature(
    observation: dict[str, Any],
    *,
    mode: str = "populated",
) -> dict[str, Any]:
    """Attach L1 signature: populated | empty | null."""
    out = deepcopy(observation)
    if mode == "null":
        out["acoustic"]["signature"] = None
    elif mode == "empty":
        out["acoustic"]["signature"] = empty_l1_signature()
    elif mode == "populated":
        out["acoustic"]["signature"] = example_l1_signature()
    else:
        raise ValueError(f"unknown L1 mode {mode!r}; use populated|empty|null")
    return out
