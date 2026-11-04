import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import dbmap_config
import server

REPO_FIRMWARE = Path(__file__).resolve().parents[2] / "firmware"


@pytest.fixture
def real_firmware_targets(monkeypatch):
    monkeypatch.setattr(server, "FIRMWARE_ROOT", REPO_FIRMWARE)
    monkeypatch.setattr(server, "FIRMWARE_SOURCE", REPO_FIRMWARE / "esp-output")
    monkeypatch.setattr(server, "NTP_SERVER", "se.pool.ntp.org")
    monkeypatch.setitem(
        server.TARGETS,
        "output-esp32",
        {
            **server.TARGETS["output-esp32"],
            "project": REPO_FIRMWARE / "esp-output",
            "components": REPO_FIRMWARE / "components",
            "extra_component_trees": [REPO_FIRMWARE / "output_components"],
        },
    )
    monkeypatch.setitem(
        server.TARGETS,
        "ear-esp32-s3",
        {
            **server.TARGETS["ear-esp32-s3"],
            "project": REPO_FIRMWARE / "esp-ear",
            "components": REPO_FIRMWARE / "components",
            "extra_component_trees": [REPO_FIRMWARE / "ear_components"],
        },
    )


def test_parse_sdkconfig_bool_int_string_and_not_set():
    values = dbmap_config.parse_sdkconfig_dbmap(
        'CONFIG_DBMAP_FIRMWARE_VERSION="0.7.0"\n'
        "CONFIG_DBMAP_EAR_OBS_L2=y\n"
        "# CONFIG_DBMAP_EAR_OBS_L2_PUBLISH is not set\n"
        "CONFIG_DBMAP_EAR_OBS_SEQ_BLOCK_SIZE=64\n"
    )
    assert values == {
        "FIRMWARE_VERSION": "0.7.0",
        "EAR_OBS_L2": True,
        "EAR_OBS_L2_PUBLISH": False,
        "EAR_OBS_SEQ_BLOCK_SIZE": 64,
    }


def test_ear_effective_config_highlights_defaults_overrides(real_firmware_targets):
    payload = server._target_dbmap_config("ear-esp32-s3")
    assert payload["target"] == "ear-esp32-s3"
    assert payload["source"] == "effective"
    observation = payload["dbmap_config"]["observation"]
    assert observation["EAR_OBS_PUBLISH_L0"] is True
    assert observation["EAR_OBS_L2"] is True
    assert observation["EAR_OBS_L2_PUBLISH"] is False
    assert observation["EAR_OBS_SERIAL_DIAG"] is True
    assert payload["dbmap_config"]["node"]["NODE_TYPE_EAR"] is True
    assert payload["dbmap_config"]["node"]["WIFI_SSID"] == ""
    assert payload["dbmap_config"]["node"]["NTP_SERVER"] == "se.pool.ntp.org"
    highlight_keys = {row["key"] for row in payload["highlights"]}
    assert "EAR_OBS_SERIAL_DIAG" in highlight_keys
    assert "EAR_OBS_PUBLISH_L0" in highlight_keys
    assert "EAR_OBS_L2" in highlight_keys


def test_output_effective_config_includes_audio_group(real_firmware_targets):
    payload = server._target_dbmap_config("output-esp32")
    assert "audio" in payload["dbmap_config"]
    assert "mic" not in payload["dbmap_config"]
    assert payload["dbmap_config"]["node"]["NODE_TYPE_OUTPUT"] is True
    assert payload["dbmap_config"]["audio"]["AUDIO_I2S_BCLK_GPIO"] == 26


def test_format_log_block_mentions_pre_build():
    text = dbmap_config.format_log_block(
        {
            "source": "effective",
            "dbmap_config": {"node": {"FIRMWARE_VERSION": "0.7.0"}},
            "highlights": [],
        }
    )
    assert "Effective CONFIG_DBMAP_* (pre-build" in text
    assert '"FIRMWARE_VERSION": "0.7.0"' in text
    json.loads(text.split(":\n", 1)[1])


def test_defaults_file_dbmap_config_is_overrides_only(real_firmware_targets):
    ear = dbmap_config.resolve_defaults_file_dbmap_config(
        node_type="ear",
        chip="esp32s3",
        project=REPO_FIRMWARE / "esp-ear",
    )
    assert set(ear) <= {"node", "mic", "observation"}
    assert "diag" not in ear
    assert "backoff" not in ear
    assert ear["observation"]["EAR_OBS_SERIAL_DIAG"] is True
    assert ear["observation"]["EAR_OBS_L2"] is True
    assert ear["observation"]["EAR_OBS_L2_PUBLISH"] is False
    assert "EAR_MIC_SAMPLE_RATE_HZ" not in ear.get("mic", {})
    assert "NTP_SERVER" not in ear["node"]
    assert "WIFI_SSID" not in ear["node"]

    output = dbmap_config.resolve_defaults_file_dbmap_config(
        node_type="output",
        chip="esp32",
        project=REPO_FIRMWARE / "esp-output",
    )
    assert set(output) == {"node"}
    assert output["node"]["NODE_TYPE_OUTPUT"] is True
    assert output["node"]["FIRMWARE_VERSION"] == "0.7.0"
    assert "audio" not in output
