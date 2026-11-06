"""Nginx must route /api/v1/analysis before the Hub API catch-all."""

from pathlib import Path

TEMPLATE = (
    Path(__file__).resolve().parents[2] / "nginx" / "default.conf.template"
).read_text(encoding="utf-8")


def test_analysis_route_is_specific_and_has_unavailable_fallback() -> None:
    assert "/api/v1/analysis" in TEMPLATE
    assert "$analysis_upstream" in TEMPLATE
    assert "@analysis_unavailable" in TEMPLATE
    assert "COMPOSE_PROFILES=analysis" in TEMPLATE
    hub_catch_all = TEMPLATE.index("(?<api_uri>/api/.*)")
    analysis_route = TEMPLATE.index("/api/v1/analysis")
    assert analysis_route < hub_catch_all
