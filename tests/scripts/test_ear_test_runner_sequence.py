"""Tests for EAR test runner timing sequence (mocked clocks; no real sleeps)."""

from __future__ import annotations

import sys
from datetime import UTC, datetime
from pathlib import Path

SCRIPT_LIB = Path(__file__).resolve().parents[2] / "scripts"
if str(SCRIPT_LIB) not in sys.path:
    sys.path.insert(0, str(SCRIPT_LIB))

import ear_test_runner as runner
from lib.ear_test.job import JobController
from lib.ear_test.models import JobMetadata
from lib.ear_test.outputs import write_job_outputs
from lib.ear_test.summary import build_summary

T0 = datetime(2026, 11, 2, 18, 0, 0, tzinfo=UTC)


class FakeClock:
    def __init__(self, start: float = 0.0) -> None:
        self.now = start
        self.sleeps: list[float] = []

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.sleeps.append(seconds)
        self.now += seconds


def test_cli_defaults_are_eight_seconds() -> None:
    parser = runner.build_parser()
    args = parser.parse_args(
        [
            "run",
            "--node-id",
            "EAR-001",
            "--period-seconds",
            "30",
            "--periods",
            "4",
        ]
    )
    assert args.pause_seconds == 8.0
    assert args.final_collection_pause_seconds == 8.0


def test_cli_custom_pause_durations() -> None:
    parser = runner.build_parser()
    args = parser.parse_args(
        [
            "run",
            "--node-id",
            "EAR-001",
            "--period-seconds",
            "30",
            "--periods",
            "2",
            "--pause-seconds",
            "12",
            "--final-collection-pause-seconds",
            "15",
        ]
    )
    assert args.pause_seconds == 12.0
    assert args.final_collection_pause_seconds == 15.0


def test_sequence_no_inter_period_after_last_but_final_collection(
    tmp_path, monkeypatch
) -> None:
    monkeypatch.setattr(runner, "reconcile_sqlite", lambda *_a, **_k: None)
    monkeypatch.setattr(runner, "reassociate_observations", lambda *_a, **_k: 0)
    monkeypatch.setenv("HOME", str(tmp_path))
    # Point results root at temp via store.results_root patch
    from lib.ear_test import store

    monkeypatch.setattr(store, "RESULTS_ROOT", tmp_path / "ear-tests")
    monkeypatch.setattr(runner, "job_dir", lambda job_id: store.job_dir(job_id))
    monkeypatch.setattr(
        runner, "create_job_directory", lambda controller: store.create_job_directory(controller)
    )
    monkeypatch.setattr(
        runner, "persist_controller", lambda controller: store.persist_controller(controller)
    )

    clock = FakeClock()
    polls: list[str] = []

    def poll_fn(controller, **_kwargs):
        phase = "period"
        if controller.current_pause is not None:
            phase = controller.current_pause.kind
        elif controller.current_period is not None:
            phase = "active"
        polls.append(phase)

    controller = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=2,
        period_count=2,
        pause_seconds=3,
        final_collection_pause_seconds=4,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        metadata=JobMetadata(test_name="seq"),
        now=T0,
    )
    store.create_job_directory(controller)

    runner.run_job_sequence(
        controller,
        progression="automatic",
        base_url="https://hub.example:8443",
        ca_file="",
        admin_token="t",
        poll_interval=1.0,
        sleep_fn=clock.sleep,
        monotonic_fn=clock.monotonic,
        poll_fn=poll_fn,
    )

    assert controller.manifest.status == "completed"
    assert [p.status for p in controller.periods] == ["completed", "completed"]
    inter = controller.inter_period_pauses()
    assert len(inter) == 1
    assert inter[0].status == "completed"
    assert inter[0].planned_seconds == 3
    final = controller.final_collection_pause()
    assert final is not None
    assert final.status == "completed"
    assert final.planned_seconds == 4
    # Polling happened during final collection
    assert "final_collection" in polls
    assert "inter_period" in polls
    # Fake clock advanced through both periods + pauses (exact sleeps depend on poll chunking)
    assert clock.now >= 2 + 3 + 2 + 4


def test_sequence_interrupt_during_inter_period_pause(tmp_path, monkeypatch) -> None:
    from lib.ear_test import store

    monkeypatch.setattr(store, "RESULTS_ROOT", tmp_path / "ear-tests")
    monkeypatch.setattr(runner, "reconcile_sqlite", lambda *_a, **_k: None)
    monkeypatch.setattr(runner, "reassociate_observations", lambda *_a, **_k: 0)

    clock = FakeClock()
    controller = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=1,
        period_count=2,
        pause_seconds=5,
        final_collection_pause_seconds=5,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    store.create_job_directory(controller)

    call_count = {"n": 0}

    def poll_fn(controller, **_kwargs):
        call_count["n"] += 1
        # Interrupt once the first inter-period pause is active.
        if controller.current_pause is not None and controller.current_pause.kind == "inter_period":
            raise KeyboardInterrupt

    try:
        runner.run_job_sequence(
            controller,
            progression="automatic",
            base_url="https://hub.example:8443",
            ca_file="",
            admin_token="t",
            poll_interval=1.0,
            sleep_fn=clock.sleep,
            monotonic_fn=clock.monotonic,
            poll_fn=poll_fn,
        )
    except KeyboardInterrupt:
        controller.interrupt()
        store.persist_controller(controller)

    assert controller.manifest.status == "interrupted"
    inter = controller.inter_period_pauses()[0]
    assert inter.status == "interrupted"
    assert inter.actual_start_utc is not None
    assert inter.actual_end_utc is not None
    assert controller.periods[1].status == "skipped"
    final = controller.final_collection_pause()
    assert final is not None
    assert final.status == "skipped"


def test_final_collection_observation_association_and_report_distinction(
    tmp_path, monkeypatch
) -> None:
    from lib.ear_test import store

    monkeypatch.setattr(store, "RESULTS_ROOT", tmp_path / "ear-tests")

    controller = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=10,
        period_count=1,
        pause_seconds=0,
        final_collection_pause_seconds=8,
        progression="automatic",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    store.create_job_directory(controller)
    controller.start_next_period(now=T0)
    controller.complete_current_period(now=T0.replace(second=10))
    pause = controller.start_next_pause(now=T0.replace(second=10))
    assert pause.kind == "final_collection"
    controller.complete_current_pause(now=T0.replace(second=18))
    controller.mark_completed(now=T0.replace(second=18))
    store.persist_controller(controller)

    # Observation received during final collection; event_time still in active period.
    record = store.build_observation_record(
        controller=controller,
        payload={
            "observation_id": "obs-final-1",
            "node_id": "EAR-001",
            "sequence_number": 1,
            "event_time_utc": "2026-11-02T18:00:08Z",
            "duration_ms": 5100,
            "signal_level_dbfs": -48.0,
        },
        received_time_utc="2026-11-02T18:00:14.000Z",
        source="http",
    )
    store.append_observation_record(controller.manifest.job_id, record)
    assoc = record["association"]
    assert assoc["phase_by_received"] == "final_collection"
    assert assoc["period_id_by_received"] is None

    summary = build_summary(controller)
    assert summary["phase_by_received_counts"].get("final_collection") == 1
    kinds = {item["kind"] for item in summary["pauses"]}
    assert kinds == {"final_collection"}
    html = write_job_outputs(controller)
    assert html["observation_count_total"] == 1
    report_text = (store.job_dir(controller.manifest.job_id) / "report.html").read_text()
    assert "final collection" in report_text
    assert "Pauses (not active periods)" in report_text
    analysis = (store.job_dir(controller.manifest.job_id) / "analysis.md").read_text()
    assert "final_collection" in analysis
    assert "Pauses (not active periods)" in analysis


def test_manual_progression_compatible(tmp_path, monkeypatch) -> None:
    from lib.ear_test import store

    monkeypatch.setattr(store, "RESULTS_ROOT", tmp_path / "ear-tests")
    monkeypatch.setattr(runner, "reconcile_sqlite", lambda *_a, **_k: None)
    monkeypatch.setattr(runner, "reassociate_observations", lambda *_a, **_k: 0)

    clock = FakeClock()
    prompts: list[str] = []

    def wait_manual(prompt: str) -> str:
        prompts.append(prompt)
        return ""

    controller = JobController.create(
        node_ids=["EAR-001"],
        period_seconds=1,
        period_count=2,
        pause_seconds=1,
        final_collection_pause_seconds=1,
        progression="manual",
        hub_base_url="https://hub.example:8443",
        now=T0,
    )
    store.create_job_directory(controller)
    runner.run_job_sequence(
        controller,
        progression="manual",
        base_url="https://hub.example:8443",
        ca_file="",
        admin_token="t",
        poll_interval=1.0,
        wait_manual_fn=wait_manual,
        sleep_fn=clock.sleep,
        monotonic_fn=clock.monotonic,
        poll_fn=lambda *_a, **_k: None,
    )
    assert len(prompts) == 2
    assert controller.manifest.status == "completed"
    assert all(p.status == "completed" for p in controller.periods)
    assert all(p.status == "completed" for p in controller.pauses)
