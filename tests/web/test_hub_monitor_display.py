"""Hub availability presentation keeps coverage concise and period labels stable."""

import shutil
import subprocess
from pathlib import Path

import pytest

INDEX = Path(__file__).resolve().parents[2] / "web" / "index.html"


def test_operations_tab_is_separate_and_uses_operations_api() -> None:
    html = INDEX.read_text()
    assert 'data-tab="operations"' in html
    assert 'id="tab-operations"' in html
    assert "/api/v1/operations/status" in html
    assert "/api/v1/operations/events" in html
    assert "Monitoring details" in html
    assert "Monitored services" in html
    assert "Recent monitoring events" in html
    assert "/api/v1/operations/services/" in html
    assert "Database details for ${service.name || service.id}" in html
    assert "? 'Hide DB'" in html
    assert ": 'DB'" in html
    assert "Log retrieval is not implemented" not in html


def test_operations_summary_and_sections_are_compact() -> None:
    html = INDEX.read_text()
    assert "#tab-operations .overview-grid.hub-identity { grid-template-columns: repeat(3" in html
    assert "#tab-operations > section > h3:first-child { margin: 0" in html
    assert "Operations status loaded." not in html
    assert "statusLine.hidden = true;" in html
    assert "rawState === 'healthy' ? 'ok' : rawState" in html
    assert "`DB: ${String(databaseStatus).replaceAll('_', ' ')}`" in html
    assert "`component-status ${databaseStatus}`" in html


def test_operations_details_preserve_hub_fields_and_panel_controls() -> None:
    html = INDEX.read_text()
    operations_renderer = html.split(
        "function renderOperationsServiceDetails(", 1
    )[1].split("function renderOperationsWorkers(", 1)[0]
    operations_controls = html.split(
        "function syncOperationsServiceButtons()", 1
    )[1].split("async function toggleOperationsServicePanel(", 1)[0]

    assert "Browser route', diagnostic.browser_route" in operations_renderer
    assert "Profile', diagnostic.profile" in operations_renderer
    assert "'Hub diagnostic message'" in operations_renderer
    assert "Operations ${directLabel}; Hub diagnostic ${diagnosticLabel}" in html
    assert "status += ` (CONNACK ${connection.connack_code})`" in html
    assert "Supplementary details: ${details.hub_diagnostics_source}" in html
    assert "? 'Hide details'" in html
    assert "? 'Hide logs'" in html
    assert "Hide details" in operations_controls
    assert "Hide logs" in operations_controls
    assert "renderComponentLogs(body, 'operations-service-detail')" in html


def test_legacy_hub_monitoring_and_mqtt_failure_controls_are_retired() -> None:
    html = INDEX.read_text()
    assert 'id="hub-monitor"' not in html
    assert 'id="hub-monitor-events-panel"' not in html
    assert "/api/v1/hub/monitoring/events" not in html
    assert "/api/v1/hub/components/${component}/logs" not in html
    assert 'id="hub-connection-failures"' not in html
    assert "/api/v1/hub/components/mosquitto/connection-failures" not in html
    assert "data-operations-connection-failures-service" in html
    assert "if (service.id === 'mqtt')" in html
    assert "/api/v1/operations/services/mosquitto/connection-failures" in html
    assert "Hide failed connections" in html
    assert "window_hours || 3" in html


def test_operations_availability_uses_short_parenthetical_coverage() -> None:
    node = shutil.which("node")
    if not node:
        pytest.skip("Node is required for JavaScript formatting tests")
    html = INDEX.read_text()
    duration = html.split("      function formatDurationSeconds(", 1)[1].split(
        "      function formatAvailabilityWindow(", 1
    )[0]
    formatter = html.split("      function formatHubAvailabilityWindow(", 1)[1].split(
        "      const operationsEventsState", 1
    )[0]
    script = (
        f"function formatDurationSeconds({duration}\n"
        f"function formatHubAvailabilityWindow({formatter}\n"
        """
        const assert = require('node:assert/strict');
        assert.equal(formatHubAvailabilityWindow(), '—');
        assert.equal(formatHubAvailabilityWindow({
          percent: 100, eligible_seconds: 1800, unavailable_seconds: 20,
          not_running_seconds: 800, low_coverage: true, score_ready: true,
        }), '100% (30m observed)');
        assert.equal(formatHubAvailabilityWindow({
          percent: 99, eligible_seconds: 1800, score_ready: false,
        }), 'Incomplete (30m observed)');
        assert.equal(formatHubAvailabilityWindow({
          percent: null, eligible_seconds: 59, insufficient_data: true,
        }), 'Insufficient data (59s observed)');
        """
    )
    subprocess.run([node, "-e", script], check=True, timeout=10)
