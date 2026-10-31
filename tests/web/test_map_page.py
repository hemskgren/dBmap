from pathlib import Path

MAP_INDEX = Path(__file__).resolve().parents[2] / "web" / "map" / "index.html"
LEAFLET_JS = (
    Path(__file__).resolve().parents[2]
    / "web"
    / "map"
    / "vendor"
    / "leaflet"
    / "leaflet.js"
)


def test_map_page_uses_vendored_leaflet_and_devices_api() -> None:
    html = MAP_INDEX.read_text(encoding="utf-8")
    assert "vendor/leaflet/leaflet.js" in html
    assert "vendor/leaflet/leaflet.css" in html
    assert "/api/v1/devices" in html
    assert "dbmapGetMapSession" in html
    assert "tile.openstreetmap.org" in html
    assert "groupDevicesByCoordinate" in html
    assert "needleIcon" in html
    assert LEAFLET_JS.is_file()


def test_hub_exposes_map_session_helper() -> None:
    hub = (Path(__file__).resolve().parents[2] / "web" / "index.html").read_text(
        encoding="utf-8"
    )
    assert 'id="open-map"' in hub
    assert "dbmapGetMapSession" in hub
    assert "/map/" in hub
