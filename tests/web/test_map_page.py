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
    assert "legend-toggle" in html
    assert "dbmap.map.legendOpen" in html
    assert "L.control.scale" in html
    assert "metric: true" in html
    assert "imperial: false" in html
    assert "position: 'bottomleft'" in html
    assert "markerLayer = L.layerGroup().addTo(map)" in html
    assert "analysisLayer = L.layerGroup().addTo(map)" in html
    assert LEAFLET_JS.is_file()


def test_map_page_optionally_loads_analysis_scratch_datasets() -> None:
    html = MAP_INDEX.read_text(encoding="utf-8")
    assert 'id="view-select"' in html
    assert 'id="view-meta"' in html
    assert 'id="dataset-select"' not in html
    assert 'value="">Live</option>' in html
    assert "Devices (default)" not in html
    assert "Device map" not in html
    assert "<h1>dBmap</h1>" in html
    assert "view-cluster" in html
    assert "No layers found" in html
    assert "Analysis unavailable" in html
    assert "obs (15m)" in html
    assert "Live · updated" in html
    assert "collectLayerTimeIsos" in html
    assert "formatTimeSpan" in html
    # Compact header: no repeated 15-min blurb in the title block.
    assert "observations in the last 15 min" not in html
    assert "Live · last 15 min" not in html
    # Hub counts stay in #status; dataset title/time live in #view-meta.
    assert "syncViewMetaFromSelection" in html
    assert "analysisNotice" not in html
    assert "/api/v1/analysis/scratch" in html
    assert "include_content=false" in html
    assert "discoverDatasets" in html
    assert "loadSelectedDataset" in html
    assert "drawHelicopterLayers" in html
    # Hub device/observation fetches remain the Live path.
    assert "fetchDevices" in html
    assert "fetchObservations" in html
    assert "analysisLayer" in html


def test_map_page_localization_popups_explain_estimate_and_evidence() -> None:
    html = MAP_INDEX.read_text(encoding="utf-8")
    assert "localizationAnalysisPopup" in html
    assert "eventAnalysisPopup" in html
    assert "observationAnalysisPopup" in html
    assert "wrapAnalysisPopup" in html
    assert "wireAnalysisPopupCopy" in html
    assert "Estimated position" in html
    assert "Published estimate only — not ground truth." in html
    assert "localizationStatusExplanation" in html
    assert "evidence has limitations" in html
    assert "Meets current quality rules" in html
    assert "Bearing selection mode" in html
    assert "Contributing EAR IDs" in html
    assert "Event member EAR IDs" in html
    assert "Membership ≠ contributors" in html
    assert "Geometric sensitivity indicator only" in html
    assert "popup-copy" in html
    assert "maxHeight: 260" in html
    assert "ANALYSIS_POPUP_OPTS" in html
    assert "Blue circle = Observation (EAR install)" in html
    assert "Orange circle = Event (published estimate when available)" in html
    assert "Purple diamond = Estimated localization (not ground truth)" in html
    assert "discoverDatasets" in html
    assert "loadSelectedDataset" in html


def test_hub_exposes_map_session_helper() -> None:
    hub = (Path(__file__).resolve().parents[2] / "web" / "index.html").read_text(
        encoding="utf-8"
    )
    assert 'id="open-map"' in hub
    assert "dbmapGetMapSession" in hub
    assert "/map/" in hub
