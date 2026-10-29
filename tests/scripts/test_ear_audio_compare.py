import sys
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / "scripts"))

spec = spec_from_file_location(
    "dbmap_ear_audio_compare",
    Path(__file__).parents[2] / "scripts" / "ear_audio_compare.py",
)
assert spec is not None and spec.loader is not None
ear_audio_compare = module_from_spec(spec)
spec.loader.exec_module(ear_audio_compare)


def test_format_ear_audio_list_filters_to_ear_and_shows_mic_levels() -> None:
    text = ear_audio_compare.format_ear_audio_list(
        [
            {
                "node_id": "OUT-001",
                "node_type": "output",
                "status": "online",
                "last_seen_at": "2026-10-29T12:00:00+00:00",
                "reported": {"payload": {}},
            },
            {
                "node_id": "EAR-002",
                "node_type": "ear",
                "status": "online",
                "last_seen_at": "2026-10-29T12:01:00+00:00",
                "reported": {
                    "payload": {
                        "audio": {
                            "subsystem_status": "ok",
                            "microphones": [
                                {
                                    "index": 0,
                                    "model": "INMP441",
                                    "rms_dbfs": -42.5,
                                    "peak_dbfs": -28.1,
                                    "sample_count": 16000,
                                    "status": "ok",
                                }
                            ],
                        }
                    }
                },
            },
            {
                "node_id": "EAR-001",
                "node_type": "EAR",
                "status": "offline",
                "last_seen_at": None,
                "reported": {"payload": {}},
            },
        ]
    )

    lines = text.splitlines()
    assert lines[0].startswith("Device")
    assert "Status" in lines[0]
    assert "Audio" in lines[0]
    assert "Mic" in lines[0]
    assert "RMS dBFS" in lines[0]
    assert "Peak dBFS" in lines[0]
    assert lines[1].startswith("EAR-001")
    assert "offline" in lines[1]
    assert "—" in lines[1]
    assert lines[2].startswith("EAR-002")
    assert "online" in lines[2]
    assert "-42.5" in lines[2]
    assert "-28.1" in lines[2]
    assert "16000" in lines[2]
    assert "OUT-001" not in text
    assert "dBFS" in lines[-1]


def test_format_ear_audio_list_empty() -> None:
    assert (
        ear_audio_compare.format_ear_audio_list(
            [{"node_id": "OUT-1", "node_type": "output"}]
        )
        == "No Ear nodes registered."
    )


def test_is_ear_node_case_insensitive() -> None:
    assert ear_audio_compare.is_ear_node({"node_type": "ear"})
    assert ear_audio_compare.is_ear_node({"node_type": "EAR"})
    assert not ear_audio_compare.is_ear_node({"node_type": "output"})


def test_parse_watch_interval_rejects_non_positive() -> None:
    assert ear_audio_compare.parse_watch_interval(15) == 15
    try:
        ear_audio_compare.parse_watch_interval(0)
    except ValueError as exc:
        assert "greater than zero" in str(exc)
    else:
        raise AssertionError("expected ValueError")
