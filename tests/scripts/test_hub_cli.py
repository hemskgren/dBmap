import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT_DIRECTORY = Path(__file__).parents[2] / "scripts"
HUB_SCRIPTS = (
    "ear_audio_compare.py",
    "hub_admin.py",
    "hub_event_track.py",
    "hub_firmware.py",
    "hub_summary.py",
)


@pytest.mark.parametrize("script_name", HUB_SCRIPTS)
def test_hub_script_without_arguments_prints_help(script_name):
    result = subprocess.run(
        [sys.executable, str(SCRIPT_DIRECTORY / script_name)],
        capture_output=True,
        check=False,
        text=True,
    )

    assert result.returncode == 0
    assert result.stdout.startswith("usage:")
    assert result.stderr == ""


@pytest.mark.parametrize(
    "script_name",
    ("ear_audio_compare.py", "hub_summary.py", "hub_event_track.py"),
)
def test_report_tools_show_list_and_watch_positional_commands(script_name):
    result = subprocess.run(
        [sys.executable, str(SCRIPT_DIRECTORY / script_name), "--help"],
        capture_output=True,
        check=False,
        text=True,
    )

    assert result.returncode == 0
    assert "positional arguments:" in result.stdout
    assert "{list,watch}" in result.stdout
