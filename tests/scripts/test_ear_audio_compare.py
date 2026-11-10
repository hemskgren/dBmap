import json
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
                            "audio_transport_status": "ok",
                            "microphone_status": "signal_present",
                            "microphones": [
                                {
                                    "index": 0,
                                    "model": "INMP441",
                                    "rms_dbfs": -42.5,
                                    "peak_dbfs": -28.1,
                                    "sample_count": 16000,
                                    "expected_sample_count": 16000,
                                    "timeout_reads": 0,
                                    "empty_reads": 0,
                                    "read_errors": 0,
                                    "status": "ok",
                                    "microphone_status": "signal_present",
                                    "classification_reason": "ambient_like_energy",
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
    assert lines[0].startswith("Transport =")
    assert lines[1].startswith("MicId =")
    assert lines[2].startswith("Fetched at ")
    assert lines[3] == ""
    assert lines[4].startswith("Device")
    assert "Status" in lines[4]
    assert "MicId" in lines[4]
    assert "Transport" in lines[4]
    assert "Mic" in lines[4]
    assert "Reason" in lines[4]
    assert "RMS dBFS" in lines[4]
    assert "Peak dBFS" in lines[4]
    assert "Expected" in lines[4]
    assert "Timeout" in lines[4]
    assert "Empty" in lines[4]
    assert "Errors" in lines[4]
    assert lines[5].startswith("EAR-001")
    assert "offline" in lines[5]
    assert "—" in lines[5]
    assert "never" in lines[5]
    assert lines[6].startswith("EAR-002")
    assert "online" in lines[6]
    assert "signal_present" in lines[6]
    assert "ambient_like_energy" in lines[6]
    assert "-42.5" in lines[6]
    assert "-28.1" in lines[6]
    assert "16000" in lines[6]
    assert "2026-10-29T12:01:00+00:00" not in text
    assert "OUT-001" not in text
    assert not lines[-1].startswith("Transport =")


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


def test_format_ear_audio_watch_includes_window_and_background_rms() -> None:
    text = ear_audio_compare.format_ear_audio_watch(
        [
            {
                "node_id": "EAR-002",
                "node_type": "ear",
                "status": "online",
                "last_seen_at": "2026-11-10T08:00:00+00:00",
                "reported": {
                    "payload": {
                        "audio": {
                            "microphone_status": "signal_present",
                            "microphones": [
                                {
                                    "index": 0,
                                    "rms_dbfs": -45.88581085205078,
                                    "peak_dbfs": -37.3879737854004,
                                    "window_ms": 1000,
                                    "sample_count": 16000,
                                    "timeout_reads": 2,
                                    "empty_reads": 1,
                                    "read_errors": 0,
                                    "background": {
                                        "window_ms": 180000,
                                        "sample_count": 177,
                                        "status": "full",
                                        "rms_min_dbfs": -47.57557678222656,
                                        "rms_max_dbfs": -24.861051559448242,
                                        "rms_mean_dbfs": -41.35853958129883,
                                    },
                                }
                            ],
                        }
                    }
                },
            },
            {
                "node_id": "EAR-001",
                "node_type": "EAR",
                "status": "online",
                "last_seen_at": "2026-11-10T08:00:01+00:00",
                "reported": {
                    "payload": {
                        "audio": {
                            "microphone_status": "signal_present",
                            "microphones": [
                                {
                                    "index": 0,
                                    "rms_dbfs": -51.9,
                                    "peak_dbfs": -39.2,
                                    "window_ms": 1000,
                                    "sample_count": 16000,
                                    "timeout_reads": 0,
                                    "empty_reads": 0,
                                    "read_errors": 0,
                                }
                            ],
                        }
                    }
                },
            },
            {
                "node_id": "OUT-001",
                "node_type": "output",
                "status": "online",
                "reported": {"payload": {}},
            },
        ]
    )
    legend = ear_audio_compare.format_ear_audio_watch_legend()
    legend_lines = legend.splitlines()
    assert legend_lines[0].startswith("RMS/Peak =")
    assert "analysis window" in legend_lines[0]
    assert "not the interval between Hub last_seen" in legend_lines[0]
    assert legend_lines[1].startswith("Timeout/Empty/Errors =")

    lines = text.splitlines()
    assert lines[0].startswith("Device")
    assert "MicId" in lines[0]
    assert "Win_ms" in lines[0]
    assert "BgMin" in lines[0]
    assert "BgMean" in lines[0]
    assert "BgMax" in lines[0]
    assert "BgWin_ms" in lines[0]
    assert "Samples" in lines[0]
    assert "Timeout" in lines[0]
    assert "Empty" in lines[0]
    assert "Errors" in lines[0]
    assert "Expected" not in lines[0]
    assert lines[1].startswith("EAR-001")
    assert "-51.9" in lines[1]
    assert "-39.2" in lines[1]
    assert "1000" in lines[1]
    assert "—" in lines[1]  # missing background
    assert "2026-11-10T08:00:01+00:00" not in lines[1]
    assert ear_audio_compare.format_local_timestamp(
        "2026-11-10T08:00:01+00:00"
    ) in lines[1]
    assert lines[2].startswith("EAR-002")
    assert "-45.9" in lines[2]
    assert "-37.4" in lines[2]
    assert "-47.6" in lines[2]
    assert "-41.4" in lines[2]
    assert "-24.9" in lines[2]
    assert "180000" in lines[2]
    # Samples / Timeout / Empty / Errors remain in the row before Last seen.
    assert " 16000 " in f" {lines[2]} "
    assert " 2 " in f" {lines[2]} "
    assert " 1 " in f" {lines[2]} "
    assert lines[2].rstrip().endswith(
        ear_audio_compare.format_local_timestamp("2026-11-10T08:00:00+00:00")
    )
    assert "2026-11-10T08:00:00+00:00" not in lines[2]
    assert "OUT-001" not in text
    assert "RMS/Peak" not in text


def test_format_ear_audio_watch_empty() -> None:
    assert (
        ear_audio_compare.format_ear_audio_watch(
            [{"node_id": "OUT-1", "node_type": "output"}]
        )
        == "No Ear nodes registered."
    )


def test_format_ear_audio_watch_online_only() -> None:
    nodes = [
        {
            "node_id": "EAR-001",
            "node_type": "ear",
            "status": "online",
            "last_seen_at": "2026-11-10T08:00:01+00:00",
            "reported": {
                "payload": {
                    "audio": {
                        "microphone_status": "signal_present",
                        "microphones": [
                            {
                                "index": 0,
                                "rms_dbfs": -50.0,
                                "peak_dbfs": -40.0,
                                "window_ms": 1000,
                                "timeout_reads": 0,
                                "empty_reads": 0,
                                "read_errors": 0,
                            }
                        ],
                    }
                }
            },
        },
        {
            "node_id": "EAR-004",
            "node_type": "ear",
            "status": "offline",
            "last_seen_at": "2026-10-31T08:30:23+00:00",
            "reported": {
                "payload": {
                    "audio": {
                        "microphone_status": "disconnected_suspected",
                        "microphones": [
                            {
                                "index": 0,
                                "rms_dbfs": -120,
                                "peak_dbfs": -120,
                                "window_ms": 1000,
                            }
                        ],
                    }
                }
            },
        },
    ]
    text = ear_audio_compare.format_ear_audio_watch(nodes, online_only=True)
    assert "EAR-001" in text
    assert "EAR-004" not in text
    assert (
        ear_audio_compare.format_ear_audio_watch(
            [nodes[1]], online_only=True
        )
        == "No online Ear nodes."
    )
    list_text = ear_audio_compare.format_ear_audio_list(nodes, online_only=True)
    assert "EAR-001" in list_text
    assert "EAR-004" not in list_text


def test_list_indents_secondary_mic() -> None:
    text = ear_audio_compare.format_ear_audio_list(
        [
            {
                "node_id": "EAR-001",
                "node_type": "ear",
                "status": "online",
                "last_seen_at": "2026-11-10T08:00:01+00:00",
                "reported": {
                    "payload": {
                        "audio": {
                            "audio_transport_status": "ok",
                            "microphone_status": "signal_present",
                            "microphones": [
                                {
                                    "index": 0,
                                    "microphone_status": "signal_present",
                                    "classification_reason": "ambient_like_energy",
                                    "rms_dbfs": -50.0,
                                    "peak_dbfs": -40.0,
                                    "sample_count": 16000,
                                    "expected_sample_count": 16000,
                                    "timeout_reads": 0,
                                    "empty_reads": 0,
                                    "read_errors": 0,
                                },
                                {
                                    "index": 1,
                                    "microphone_status": "disconnected_suspected",
                                    "classification_reason": "digital_silence",
                                    "rms_dbfs": -120,
                                    "peak_dbfs": -120,
                                    "sample_count": 16000,
                                    "expected_sample_count": 16000,
                                    "timeout_reads": 0,
                                    "empty_reads": 0,
                                    "read_errors": 0,
                                },
                            ],
                        }
                    }
                },
            }
        ]
    )
    # After legend: header then device rows — find table body by MicId header.
    body = text.splitlines()
    header_i = next(i for i, line in enumerate(body) if line.startswith("Device"))
    primary = body[header_i + 1]
    secondary = body[header_i + 2]
    assert primary.startswith("EAR-001")
    assert secondary.startswith(" ")
    assert "EAR-001" not in secondary
    assert "online" not in secondary
    assert "disconnected_suspected" in secondary
    assert "-120.0" in secondary
    # MicId digits share the same column (no leading pad on "1").
    mic_id_col = body[header_i].index("MicId")
    assert primary[mic_id_col] == "0"
    assert secondary[mic_id_col] == "1"


def test_watch_indents_secondary_mic() -> None:
    text = ear_audio_compare.format_ear_audio_watch(
        [
            {
                "node_id": "EAR-001",
                "node_type": "ear",
                "status": "online",
                "last_seen_at": "2026-11-10T08:00:01+00:00",
                "reported": {
                    "payload": {
                        "audio": {
                            "microphone_status": "signal_present",
                            "microphones": [
                                {
                                    "index": 0,
                                    "microphone_status": "signal_present",
                                    "rms_dbfs": -50.0,
                                    "peak_dbfs": -40.0,
                                    "window_ms": 1000,
                                    "sample_count": 16000,
                                    "timeout_reads": 0,
                                    "empty_reads": 0,
                                    "read_errors": 0,
                                },
                                {
                                    "index": 1,
                                    "microphone_status": "disconnected_suspected",
                                    "rms_dbfs": -120,
                                    "peak_dbfs": -120,
                                    "window_ms": 1000,
                                    "sample_count": 16000,
                                    "timeout_reads": 0,
                                    "empty_reads": 0,
                                    "read_errors": 0,
                                },
                            ],
                        }
                    }
                },
            }
        ]
    )
    lines = text.splitlines()
    assert lines[0].startswith("Device")
    assert "MicId" in lines[0]
    assert lines[1].startswith("EAR-001")
    assert lines[2].startswith(" ")
    assert "EAR-001" not in lines[2]
    assert "online" not in lines[2]
    assert "disconnected_suspected" in lines[2]
    assert "-120.0" in lines[2]
    mic_id_col = lines[0].index("MicId")
    assert lines[1][mic_id_col] == "0"
    assert lines[2][mic_id_col] == "1"


def test_list_json_includes_all_mics() -> None:
    payload = ear_audio_compare.format_ear_audio_list_json(
        [
            {
                "node_id": "EAR-001",
                "node_type": "ear",
                "status": "online",
                "last_seen_at": "2026-11-10T08:00:01+00:00",
                "reported": {
                    "payload": {
                        "audio": {
                            "audio_transport_status": "ok",
                            "microphone_status": "signal_present",
                            "microphones": [
                                {
                                    "index": 0,
                                    "rms_dbfs": -50.123,
                                    "peak_dbfs": -40.0,
                                    "window_ms": 1000,
                                    "sample_count": 16000,
                                    "expected_sample_count": 16000,
                                    "timeout_reads": 0,
                                    "empty_reads": 0,
                                    "read_errors": 0,
                                    "microphone_status": "signal_present",
                                    "background": {
                                        "window_ms": 180000,
                                        "sample_count": 10,
                                        "status": "partial",
                                        "rms_min_dbfs": -55.0,
                                        "rms_mean_dbfs": -52.0,
                                        "rms_max_dbfs": -48.0,
                                    },
                                },
                                {
                                    "index": 1,
                                    "rms_dbfs": -120,
                                    "peak_dbfs": -120,
                                    "window_ms": 1000,
                                    "microphone_status": "disconnected_suspected",
                                },
                            ],
                        }
                    }
                },
            },
            {
                "node_id": "OUT-001",
                "node_type": "output",
                "status": "online",
                "reported": {"payload": {}},
            },
        ]
    )
    data = json.loads(payload)
    assert len(data) == 1
    assert data[0]["node_id"] == "EAR-001"
    assert data[0]["status"] == "online"
    assert data[0]["last_seen_at"] == "2026-11-10T08:00:01+00:00"
    assert len(data[0]["microphones"]) == 2
    assert data[0]["microphones"][0]["index"] == 0
    assert data[0]["microphones"][0]["rms_dbfs"] == -50.123
    assert data[0]["microphones"][0]["background"]["rms_mean_dbfs"] == -52.0
    assert data[0]["microphones"][1]["index"] == 1
    assert data[0]["microphones"][1]["rms_dbfs"] == -120


def test_format_local_timestamp_second_precision() -> None:
    assert ear_audio_compare.format_local_timestamp(None) == "never"
    assert ear_audio_compare.format_local_timestamp("") == "never"
    formatted = ear_audio_compare.format_local_timestamp(
        "2026-11-10T08:00:01.456789+00:00"
    )
    assert formatted == "2026-11-10 09:00:01 CET"
    assert ear_audio_compare.format_local_timestamp(
        "2026-07-10T08:00:01+00:00"
    ) == "2026-07-10 10:00:01 CEST"
    assert ear_audio_compare.format_now_local_timestamp().count(":") >= 2


def test_list_json_keeps_raw_last_seen_timestamp() -> None:
    payload = ear_audio_compare.format_ear_audio_list_json(
        [
            {
                "node_id": "EAR-001",
                "node_type": "ear",
                "status": "online",
                "last_seen_at": "2026-11-10T08:00:01.456789+00:00",
                "reported": {"payload": {"audio": {"microphones": []}}},
            }
        ]
    )
    assert "2026-11-10T08:00:01.456789+00:00" in payload
    assert "09:00:01 CET" not in payload
