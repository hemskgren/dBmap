"""Hub availability presentation keeps coverage concise and period labels stable."""

import shutil
import subprocess
from pathlib import Path

import pytest

INDEX = Path(__file__).resolve().parents[2] / "web" / "index.html"


def test_hub_availability_displays_all_periods_for_services() -> None:
    html = INDEX.read_text()
    panel = html.split('<section id="hub-monitor">', 1)[1].split("</section>", 1)[0]
    assert "Hub services availability" in panel
    assert "Public endpoints</th>" in panel
    assert "Core Hub availability" not in panel
    for service in ("hub", "hub-api", "hub-mqtt", "hub-web"):
        for period in ("24h", "7d", "30d"):
            assert panel.count(f'id="{service}-availability-{period}"') == 1
    assert "Observation details" in panel


def test_hub_availability_uses_short_parenthetical_coverage() -> None:
    node = shutil.which("node")
    if not node:
        pytest.skip("Node is required for JavaScript formatting tests")
    html = INDEX.read_text()
    duration = html.split("      function formatDurationSeconds(", 1)[1].split(
        "      function formatAvailabilityWindow(", 1
    )[0]
    formatter = html.split("      function formatHubAvailabilityWindow(", 1)[1].split(
        "      function addHubWindowDetails(", 1
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


def test_public_transition_labels_do_not_confuse_observation_with_recovery() -> None:
    node = shutil.which("node")
    if not node:
        pytest.skip("Node is required for JavaScript formatting tests")
    html = INDEX.read_text()
    formatter = html.split("      function formatPublicTransition(", 1)[1].split(
        "      function renderPublicMonitoring(", 1
    )[0]
    script = f"function formatPublicTransition({formatter}\n" + """
        const assert = require('node:assert/strict');
        const label = (previous_state, new_state) => formatPublicTransition({previous_state, new_state});
        assert.equal(label(null, 'ok'), 'Initial observation: Available');
        assert.equal(label(null, 'offline'), 'Initial observation: Unavailable');
        assert.equal(label('unknown', 'ok'), 'Available after unobserved period');
        assert.equal(label('unknown', 'offline'), 'Unavailable after unobserved period');
        assert.equal(label('offline', 'ok'), 'Recovered');
        assert.equal(label('ok', 'offline'), 'Became unavailable');
        assert.equal(label('ok', 'unknown'), 'Observation became unobserved');
        assert.equal(label(undefined, 'ok'), 'Available');
        assert.equal(formatPublicTransition({
          event_type: 'monitoring_resumed', previous_state: 'unknown', new_state: 'ok',
        }), 'Monitoring resumed: Available');
    """
    subprocess.run([node, "-e", script], check=True, timeout=10)
