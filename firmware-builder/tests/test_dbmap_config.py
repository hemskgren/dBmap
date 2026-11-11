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
            "components": REPO_FIRMWARE / "platform",
            "extra_component_trees": [
                {
                    "src": REPO_FIRMWARE / "output_components",
                    "dest": "output_components",
                }
            ],
        },
    )
    monkeypatch.setitem(
        server.TARGETS,
        "ear-esp32-s3",
        {
            **server.TARGETS["ear-esp32-s3"],
            "project": REPO_FIRMWARE / "esp-ear",
            "components": REPO_FIRMWARE / "platform",
            "extra_component_trees": [
                {
                    "src": REPO_FIRMWARE / "products" / "dbmap" / "ear",
                    "dest": "products/dbmap/ear",
                }
            ],
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


def test_relative_end_kconfig_default_is_enabled():
    kconfig = (
        REPO_FIRMWARE / "products" / "dbmap" / "ear" / "ear_observation" / "Kconfig"
    ).read_text(encoding="utf-8")
    defaults = dbmap_config.parse_kconfig_dbmap_defaults(kconfig)
    assert defaults["EAR_OBS_RELATIVE_END"] is True
    assert defaults["EAR_OBS_REL_RETURN_MARGIN_DB_X1000"] == 4000
    assert defaults["EAR_OBS_REL_EXIT_HOLD_MS"] == 2000
    assert defaults["EAR_OBS_REL_CANCEL_HYST_DB_X1000"] == 1500
    assert defaults["EAR_OBS_REL_MIN_CONTRAST_DB_X1000"] == 5000
    assert defaults["EAR_OBS_REL_BASELINE_STALE_MS"] == 15000
    assert defaults["EAR_OBS_REL_LATE_ARM"] is False


def test_builder_does_not_disable_relative_end(real_firmware_targets):
    """Effective merge must keep Relative END enabled (no builder overlay to n)."""
    payload = server._target_dbmap_config("ear-esp32-s3")
    observation = payload["config"]["product"]["dbmap"]["observation"]
    assert observation["EAR_OBS_RELATIVE_END"] is True
    # Secrets/NTP overlays only — Relative END must not appear as flipped off.
    assert not any(
        row["key"] == "EAR_OBS_RELATIVE_END" and row["value"] is False
        for row in payload["highlights"]
    )


def test_ear_effective_config_highlights_defaults_overrides(real_firmware_targets):
    payload = server._target_dbmap_config("ear-esp32-s3")
    assert payload["target"] == "ear-esp32-s3"
    assert payload["source"] == "effective"
    observation = payload["config"]["product"]["dbmap"]["observation"]
    assert observation["EAR_OBS_PUBLISH_L0"] is True
    assert observation["EAR_OBS_L2"] is True
    assert observation["EAR_OBS_L2_PUBLISH"] is True
    assert observation["EAR_OBS_SERIAL_DIAG"] is True
    assert observation["EAR_OBS_RELATIVE_END"] is True
    assert observation["EAR_OBS_REL_RETURN_MARGIN_DB_X1000"] == 4000
    assert observation["EAR_OBS_REL_EXIT_HOLD_MS"] == 2000
    assert observation["EAR_OBS_REL_CANCEL_HYST_DB_X1000"] == 1500
    assert observation["EAR_OBS_REL_MIN_CONTRAST_DB_X1000"] == 5000
    assert observation["EAR_OBS_REL_BASELINE_STALE_MS"] == 15000
    assert observation["EAR_OBS_REL_LATE_ARM"] is True
    assert payload["config"]["sp"]["node"]["NODE_TYPE_EAR"] is True
    assert payload["config"]["sp"]["node"]["WIFI_SSID"] == ""
    assert payload["config"]["sp"]["node"]["NTP_SERVER"] == "se.pool.ntp.org"
    highlight_keys = {row["key"] for row in payload["highlights"]}
    assert "EAR_OBS_SERIAL_DIAG" in highlight_keys
    assert "EAR_OBS_PUBLISH_L0" in highlight_keys
    assert "EAR_OBS_L2" in highlight_keys
    # Relative END matches Kconfig default y, so it is not a defaults-file highlight.


def test_output_effective_config_includes_output_group(real_firmware_targets):
    payload = server._target_dbmap_config("output-esp32")
    sp = payload["config"]["sp"]
    assert "output" in sp
    assert "mic" not in sp
    assert sp["node"]["NODE_TYPE_OUTPUT"] is True
    assert sp["output"]["AUDIO_I2S_BCLK_GPIO"] == 26
    assert payload["config"]["product"]["dbmap"] == {}


def test_format_log_block_mentions_pre_build():
    text = dbmap_config.format_log_block(
        {
            "source": "effective",
            "config": {"sp": {"node": {"NODE_TYPE_EAR": True}}, "product": {"dbmap": {}}},
            "highlights": [],
        }
    )
    assert "Effective firmware config (pre-build" in text
    assert '"NODE_TYPE_EAR": true' in text
    json.loads(text.split(":\n", 1)[1])


def test_defaults_file_dbmap_config_is_overrides_only(real_firmware_targets):
    ear = dbmap_config.resolve_defaults_file_dbmap_config(
        node_type="ear",
        chip="esp32s3",
        project=REPO_FIRMWARE / "esp-ear",
    )
    assert set(ear) == {"sp", "product"}
    assert set(ear["sp"]) <= {"node", "mic"}
    assert "diag" not in ear["sp"]
    assert "backoff" not in ear["sp"]
    observation = ear["product"]["dbmap"]["observation"]
    assert observation["EAR_OBS_SERIAL_DIAG"] is True
    assert observation["EAR_OBS_L2"] is True
    assert observation["EAR_OBS_L2_PUBLISH"] is True
    assert observation["EAR_OBS_RELATIVE_END"] is True
    assert observation["EAR_OBS_REL_LATE_ARM"] is True
    # Dependent Relative END ints come from Kconfig defaults, not sdkconfig.defaults.
    assert "EAR_OBS_REL_RETURN_MARGIN_DB_X1000" not in observation
    assert "EAR_MIC_SAMPLE_RATE_HZ" not in ear["sp"].get("mic", {})
    assert "NTP_SERVER" not in ear["sp"]["node"]
    assert "WIFI_SSID" not in ear["sp"]["node"]
    assert "FIRMWARE_VERSION" not in ear["sp"]["node"]

    output = dbmap_config.resolve_defaults_file_dbmap_config(
        node_type="output",
        chip="esp32",
        project=REPO_FIRMWARE / "esp-output",
    )
    assert set(output) == {"sp", "product"}
    assert set(output["sp"]) == {"node"}
    assert output["sp"]["node"]["NODE_TYPE_OUTPUT"] is True
    assert "FIRMWARE_VERSION" not in output["sp"]["node"]
    assert "output" not in output["sp"]
    assert output["product"]["dbmap"] == {}


def test_effective_and_defaults_omit_nested_firmware_version(real_firmware_targets):
    effective = server._target_dbmap_config("ear-esp32-s3")
    assert "FIRMWARE_VERSION" not in effective["config"]["sp"]["node"]
    defaults = dbmap_config.resolve_defaults_file_dbmap_config(
        node_type="ear",
        chip="esp32s3",
        project=REPO_FIRMWARE / "esp-ear",
    )
    assert "FIRMWARE_VERSION" not in defaults.get("sp", {}).get("node", {})
