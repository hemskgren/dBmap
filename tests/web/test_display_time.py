"""Web presentation datetime: runtime config + shared formatter contract."""

from __future__ import annotations

import importlib.util
from pathlib import Path

WEB_ROOT = Path(__file__).resolve().parents[2] / "web"


def _load_serve():
    spec = importlib.util.spec_from_file_location(
        "dbmap_web_serve",
        WEB_ROOT / "serve.py",
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_display_config_js_default_stockholm(monkeypatch) -> None:
    serve = _load_serve()
    monkeypatch.delenv("DBMAP_DISPLAY_TIMEZONE", raising=False)
    body = serve.build_display_config_js().decode("utf-8")
    assert 'window.DBMAP_DISPLAY_TIMEZONE = "Europe/Stockholm";' in body


def test_display_config_js_uses_env_and_rejects_unsafe(monkeypatch) -> None:
    serve = _load_serve()
    monkeypatch.setenv("DBMAP_DISPLAY_TIMEZONE", "UTC")
    assert '"UTC"' in serve.build_display_config_js().decode("utf-8")
    monkeypatch.setenv("DBMAP_DISPLAY_TIMEZONE", 'Evil"; alert(1);//')
    assert (
        'window.DBMAP_DISPLAY_TIMEZONE = "Europe/Stockholm";'
        in serve.build_display_config_js().decode("utf-8")
    )


def test_display_time_js_is_shared_contract() -> None:
    text = (WEB_ROOT / "display-time.js").read_text(encoding="utf-8")
    assert "Europe/Stockholm" in text
    assert "formatDisplayDateTime" in text
    assert "YYYY-MM-DD" in text or "${year}-${month}-${day}" in text
    index_html = (WEB_ROOT / "index.html").read_text(encoding="utf-8")
    map_html = (WEB_ROOT / "map" / "index.html").read_text(encoding="utf-8")
    assert "/display-config.js" in index_html
    assert "/display-time.js" in index_html
    assert "formatDisplayDateTime" in index_html
    assert "EU_DATE_TIME_FORMATTER" not in index_html
    assert "/display-config.js" in map_html
    assert "/display-time.js" in map_html
    assert "formatDisplayDateTime" in map_html
    assert "typeof formatDisplayDateTime === 'function'" in index_html
    assert "typeof formatDisplayDateTime === 'function'" in map_html


def test_compose_passes_display_timezone_without_rebuild_requirement() -> None:
    compose = (
        Path(__file__).resolve().parents[2] / "compose.yaml"
    ).read_text(encoding="utf-8")
    assert "DBMAP_DISPLAY_TIMEZONE: ${DBMAP_DISPLAY_TIMEZONE:-Europe/Stockholm}" in compose


def test_nginx_allows_display_time_js_when_base_path_empty() -> None:
    template = (
        Path(__file__).resolve().parents[2] / "nginx" / "default.conf.template"
    ).read_text(encoding="utf-8")
    assert "display-time\\.js" in template
    assert "display-config\\.js" in template
    asset_route = template.index("display-time\\.js")
    only_base = template.index("(?<only_base>")
    assert asset_route < only_base
