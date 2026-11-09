"""Validate official Swedish outdoor-warning (Hesa Fredrik / MCF) scenarios."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).parents[2]
SCRIPT_DIRECTORY = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPT_DIRECTORY))

from lib.audio_scenario import (
    estimated_wait_seconds,
    load_scenario_file,
    run_scenario,
)

AUDIO_DIR = REPO_ROOT / "tests" / "audio"

OFFICIAL_URLS = {
    "vma": (
        "https://www.mcf.se/contentassets/ffc078def2284745ae47c8282f83839f/"
        "01-viktigt-meddelande.mp3"
    ),
    "beredskapslarm": (
        "https://www.mcf.se/contentassets/ffc078def2284745ae47c8282f83839f/"
        "04-beredskapslarm.mp3"
    ),
    "flyglarm": (
        "https://www.mcf.se/contentassets/ffc078def2284745ae47c8282f83839f/"
        "03-flyglarm.mp3"
    ),
    "faran_over": (
        "https://www.mcf.se/contentassets/ffc078def2284745ae47c8282f83839f/"
        "02-faran-over-.mp3"
    ),
}

SCENARIO_FILES = (
    "vma-signal.json",
    "vma-quarterly-test.json",
    "beredskapslarm.json",
    "flyglarm.json",
    "faran-over.json",
)


@pytest.mark.parametrize("filename", SCENARIO_FILES)
def test_swedish_warning_scenarios_validate(filename: str) -> None:
    scenario = load_scenario_file(AUDIO_DIR / filename)
    assert scenario["schema_version"] == 1
    assert "TEST ONLY" in scenario.get("description", "")
    for source in scenario["media_sources"].values():
        assert source["url"].startswith("https://www.mcf.se/")


def test_standalone_scenarios_resolve_official_sources() -> None:
    mapping = {
        "vma-signal.json": ("vma", OFFICIAL_URLS["vma"]),
        "beredskapslarm.json": ("beredskapslarm", OFFICIAL_URLS["beredskapslarm"]),
        "flyglarm.json": ("flyglarm", OFFICIAL_URLS["flyglarm"]),
        "faran-over.json": ("faran_over", OFFICIAL_URLS["faran_over"]),
    }
    for filename, (source_key, url) in mapping.items():
        scenario = load_scenario_file(AUDIO_DIR / filename)
        assert scenario["media_sources"][source_key]["url"] == url
        play_steps = [step for step in scenario["steps"] if step["action"] == "play"]
        assert len(play_steps) == 1
        assert play_steps[0]["source"] == source_key


def test_vma_quarterly_test_sequence_and_300s_timing() -> None:
    scenario = load_scenario_file(AUDIO_DIR / "vma-quarterly-test.json")
    assert scenario["media_sources"]["vma"]["url"] == OFFICIAL_URLS["vma"]
    assert scenario["media_sources"]["faran_over"]["url"] == OFFICIAL_URLS["faran_over"]

    actions = [(step["action"], step) for step in scenario["steps"]]
    play_steps = [step for action, step in actions if action == "play"]
    wait_steps = [step for action, step in actions if action == "wait"]

    assert len(play_steps) == 2
    assert play_steps[0]["source"] == "vma"
    assert play_steps[1]["source"] == "faran_over"

    # Exactly one 300-second wait, immediately after the VMA play request.
    assert scenario["steps"][0]["action"] == "play"
    assert scenario["steps"][0]["source"] == "vma"
    assert scenario["steps"][1]["action"] == "wait"
    assert scenario["steps"][1]["seconds"] == pytest.approx(300.0)
    assert sum(1 for step in wait_steps if step["seconds"] == 300.0) == 1

    # No second 300s wait after VMA finishes; only the one timed from play.
    assert estimated_wait_seconds(scenario) >= 300.0

    sleeps: list[float] = []
    report = run_scenario(
        scenario,
        client=None,
        dry_run=True,
        sleep=sleeps.append,
    )
    assert report.error is None
    assert [step.action for step in report.steps] == [
        "play",
        "wait",
        "play",
        "wait",
        "stop",
    ]
    assert report.estimated_wait_s == pytest.approx(340.0)
    assert sleeps == []
    assert OFFICIAL_URLS["vma"] in report.steps[0].detail
    assert OFFICIAL_URLS["faran_over"] in report.steps[2].detail
