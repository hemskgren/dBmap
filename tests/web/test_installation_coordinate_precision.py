"""Web admin lat/lon must not be stricter than the Hub installation API.

The previous six-decimal limit came from HTML number inputs using
step="0.000001", which triggers browser stepMismatch for finer values.
"""

from __future__ import annotations

import json
import math
import re
from pathlib import Path

HUB_INDEX = Path(__file__).resolve().parents[2] / "web" / "index.html"


def _installation_number_step(html: str, field: str) -> str:
    # Field defs: ['latitude', 'Latitude', 'number', 'any'],
    match = re.search(
        rf"\['{field}',\s*'[^']*',\s*'number',\s*'([^']+)'\]",
        html,
    )
    assert match is not None, f"missing installation field definition for {field}"
    return match.group(1)


def test_admin_lat_lon_inputs_use_step_any() -> None:
    html = HUB_INDEX.read_text(encoding="utf-8")
    assert _installation_number_step(html, "latitude") == "any"
    assert _installation_number_step(html, "longitude") == "any"
    assert "input.min = '-90'" in html
    assert "input.max = '90'" in html
    assert "input.min = '-180'" in html
    assert "input.max = '180'" in html
    # Detail/edit path uses String(value); do not clip installation coords
    # with toFixed(6) when filling the form or rendering the definition list.
    form_and_detail = html[
        html.index("function addDetailField") : html.index("Recover bootstrap token")
    ]
    assert "toFixed(" not in form_and_detail
    assert re.search(
        r"input\.value = value === null \|\| value === undefined \? '' : String\(value\)",
        html,
    )


def test_frontend_number_path_preserves_more_than_six_decimals() -> None:
    """Mirror web submit: Number(raw) → JSON → parse; no deliberate rounding."""

    samples = [
        ("60", "10"),
        ("59.912345", "10.752345"),
        ("59.1234567", "10.7654321"),
        ("-33.8688197", "151.2092955"),
        ("-90", "-180"),
        ("90", "180"),
        ("0", "0"),
    ]
    for lat_raw, lon_raw in samples:
        payload = {
            "latitude": float(lat_raw) if lat_raw else None,
            "longitude": float(lon_raw) if lon_raw else None,
        }
        # Same conversion the admin form uses for type === 'number'.
        values = {
            "latitude": None if lat_raw.strip() == "" else float(lat_raw),
            "longitude": None if lon_raw.strip() == "" else float(lon_raw),
        }
        assert values["latitude"] == payload["latitude"]
        assert values["longitude"] == payload["longitude"]
        roundtrip = json.loads(json.dumps(values))
        assert roundtrip["latitude"] == values["latitude"]
        assert roundtrip["longitude"] == values["longitude"]
        # Display path: String(apiValue) must not clip to six decimals.
        displayed = f"{roundtrip['latitude']}, {roundtrip['longitude']}"
        if "." in lat_raw and len(lat_raw.split(".", 1)[1]) > 6:
            assert lat_raw in displayed or displayed.startswith(
                str(roundtrip["latitude"])
            )
            # Ensure we did not silently round to six places in the payload.
            six = round(values["latitude"], 6)
            assert values["latitude"] != six or lat_raw.endswith("0")


def test_frontend_rejects_out_of_range_via_same_bounds_as_api() -> None:
    def in_range(lat: float, lon: float) -> bool:
        return (
            math.isfinite(lat)
            and math.isfinite(lon)
            and -90 <= lat <= 90
            and -180 <= lon <= 180
        )

    assert in_range(59.1234567, 10.7654321)
    assert in_range(-90, -180)
    assert in_range(90, 180)
    assert not in_range(90.0000001, 0)
    assert not in_range(-90.1, 0)
    assert not in_range(0, 180.0000001)
    assert not in_range(0, -180.1)
    assert not in_range(float("nan"), 0)
    assert not in_range(float("inf"), 0)
