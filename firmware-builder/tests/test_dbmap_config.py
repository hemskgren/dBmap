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


def test_parse_show_zero_error_counters_from_defaults():
    assert dbmap_config.parse_show_zero_error_counters(
        "# CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS is not set\n"
    ) is False
    assert dbmap_config.parse_show_zero_error_counters(
        "CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS=y\n"
    ) is True
    assert dbmap_config.parse_show_zero_error_counters("CONFIG_OTHER=y\n") is None


def test_inject_show_zero_error_counters_metadata():
    config: dict = {"sp": {"version": "0.2.0"}, "product": {"dbmap": {}}}
    dbmap_config.inject_show_zero_error_counters(config, False)
    assert config["sp"]["mic"]["show_zero_error_counters"] is False
    dbmap_config.inject_show_zero_error_counters(config, True)
    assert config["sp"]["mic"]["show_zero_error_counters"] is True


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
    product = payload["config"]["product"]["dbmap"]
    observation = product["observation"]
    acoustic = product["acoustic"]
    assert acoustic["EAR_MIC_L0"] is True
    assert payload["config"]["sp"]["mic"]["show_zero_error_counters"] is False
    assert observation["EAR_OBS_PUBLISH_L0"] is True
    assert observation["EAR_OBS_L2"] is True
    assert observation["EAR_OBS_L2_PUBLISH"] is True
    assert observation["EAR_OBS_SERIAL_DIAG"] is True
    assert observation["EAR_OBS_DECISION_DIAG"] is True
    assert observation["EAR_AE_SIG_CAPTURE"] is True
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
    assert payload["config"]["sp"]["version"] == "0.2.0"
    highlight_keys = {row["key"] for row in payload["highlights"]}
    assert "EAR_OBS_SERIAL_DIAG" in highlight_keys
    assert "EAR_OBS_DECISION_DIAG" in highlight_keys
    assert "EAR_OBS_PUBLISH_L0" in highlight_keys
    assert "EAR_OBS_L2" in highlight_keys
    assert "EAR_AE_SIG_CAPTURE" in highlight_keys
    # EAR_MIC_L0 and Relative END match Kconfig default y → not defaults-file highlights.


def test_ear_decision_diag_can_be_disabled_by_target_override(
    tmp_path, monkeypatch, real_firmware_targets
):
    source = REPO_FIRMWARE / "esp-ear" / "sdkconfig.defaults"
    tmp_path.joinpath("sdkconfig.defaults").write_bytes(source.read_bytes())
    tmp_path.joinpath("sdkconfig.defaults.esp32s3").write_text(
        "# CONFIG_DBMAP_EAR_OBS_DECISION_DIAG is not set\n", encoding="utf-8"
    )
    target = server.TARGETS["ear-esp32-s3"]
    monkeypatch.setitem(server.TARGETS, "ear-esp32-s3", {**target, "project": tmp_path})

    payload = server._target_dbmap_config("ear-esp32-s3")
    assert (
        payload["config"]["product"]["dbmap"]["observation"]["EAR_OBS_DECISION_DIAG"]
        is False
    )


def test_output_effective_config_includes_output_group(real_firmware_targets):
    payload = server._target_dbmap_config("output-esp32")
    sp = payload["config"]["sp"]
    assert "output" in sp
    assert "mic" not in sp  # Output builds do not attach mic report metadata
    assert sp["node"]["NODE_TYPE_OUTPUT"] is True
    assert sp["output"]["AUDIO_I2S_BCLK_GPIO"] == 26
    assert sp["version"] == "0.2.0"
    assert payload["config"]["product"]["dbmap"] == {}


def test_format_log_block_mentions_pre_build():
    text = dbmap_config.format_log_block(
        {
            "source": "effective",
            "config": {
                "sp": {"node": {"NODE_TYPE_EAR": True}},
                "product": {"dbmap": {}},
            },
            "highlights": [],
        }
    )
    assert "Resolved firmware config preview (pre-build" in text
    assert '"NODE_TYPE_EAR": true' in text
    json.loads(text.split(":\n", 1)[1])


def test_defaults_file_dbmap_config_is_overrides_only(real_firmware_targets):
    ear = dbmap_config.resolve_defaults_file_dbmap_config(
        node_type="ear",
        chip="esp32s3",
        project=REPO_FIRMWARE / "esp-ear",
        platform_dir=REPO_FIRMWARE / "platform",
    )
    assert set(ear) == {"sp", "product"}
    assert set(ear["sp"]) <= {"node", "version", "mic"}
    assert ear["sp"]["version"] == "0.2.0"
    assert ear["sp"]["mic"]["show_zero_error_counters"] is False
    assert "diag" not in ear["sp"]
    assert "backoff" not in ear["sp"]
    observation = ear["product"]["dbmap"]["observation"]
    acoustic = ear["product"]["dbmap"]["acoustic"]
    assert acoustic["EAR_MIC_L0"] is True
    assert observation["EAR_OBS_SERIAL_DIAG"] is True
    assert observation["EAR_OBS_DECISION_DIAG"] is True
    assert observation["EAR_AE_SIG_CAPTURE"] is True
    assert observation["EAR_OBS_L2"] is True
    assert observation["EAR_OBS_L2_PUBLISH"] is True
    assert observation["EAR_OBS_RELATIVE_END"] is True
    assert observation["EAR_OBS_REL_LATE_ARM"] is True
    # Dependent Relative END ints come from Kconfig defaults, not sdkconfig.defaults.
    assert "EAR_OBS_REL_RETURN_MARGIN_DB_X1000" not in observation
    assert "NTP_SERVER" not in ear["sp"]["node"]
    assert "WIFI_SSID" not in ear["sp"]["node"]
    assert "FIRMWARE_VERSION" not in ear["sp"]["node"]

    output = dbmap_config.resolve_defaults_file_dbmap_config(
        node_type="output",
        chip="esp32",
        project=REPO_FIRMWARE / "esp-output",
        platform_dir=REPO_FIRMWARE / "platform",
    )
    assert set(output) == {"sp", "product"}
    assert set(output["sp"]) == {"node", "version"}
    assert output["sp"]["node"]["NODE_TYPE_OUTPUT"] is True
    assert output["sp"]["version"] == "0.2.0"
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
        platform_dir=REPO_FIRMWARE / "platform",
    )
    assert "FIRMWARE_VERSION" not in defaults.get("sp", {}).get("node", {})
    assert defaults["sp"]["version"] == "0.2.0"
    assert effective["config"]["sp"]["version"] == "0.2.0"


def test_platform_version_semver_validation(tmp_path):
    assert dbmap_config.is_valid_sp_version("0.1.0")
    assert dbmap_config.is_valid_sp_version("1.2.3")
    assert not dbmap_config.is_valid_sp_version("")
    assert not dbmap_config.is_valid_sp_version("0.1")
    assert not dbmap_config.is_valid_sp_version("v0.1.0")
    assert not dbmap_config.is_valid_sp_version("0.1.0-beta")

    missing = tmp_path / "missing"
    missing.mkdir()
    with pytest.raises(ValueError, match="Missing"):
        dbmap_config.read_platform_version(missing)

    bad = tmp_path / "bad"
    bad.mkdir()
    (bad / "VERSION").write_text("not-a-version\n", encoding="utf-8")
    with pytest.raises(ValueError, match="Malformed"):
        dbmap_config.read_platform_version(bad)

    good = tmp_path / "good"
    good.mkdir()
    (good / "VERSION").write_text("# comment\n0.1.0\n", encoding="utf-8")
    assert dbmap_config.read_platform_version(good) == "0.1.0"


def test_sp_version_independent_of_firmware_series(real_firmware_targets):
    payload = server._target_dbmap_config("ear-esp32-s3")
    sp_ver = payload["config"]["sp"]["version"]
    # sdkconfig series base remains under FIRMWARE_VERSION in flat parse, not nested.
    defaults_text = (REPO_FIRMWARE / "esp-ear" / "sdkconfig.defaults").read_text(
        encoding="utf-8"
    )
    assert 'CONFIG_DBMAP_FIRMWARE_VERSION="0.8.0"' in defaults_text
    assert sp_ver == "0.2.0"
    assert sp_ver != "0.8.0"


@pytest.mark.parametrize(
    ("profile", "capture", "ae", "serial", "decision", "csv"),
    [
        ("production", False, False, False, False, False),
        ("development", False, False, True, False, False),
        ("pcm-verification", True, True, True, False, False),
        ("acoustic-diagnosis", False, False, True, True, False),
        ("full-diagnostics", True, True, True, True, True),
        ("calibration", False, False, True, True, True),
    ],
)
def test_named_ear_profile(
    real_firmware_targets, profile, capture, ae, serial, decision, csv
):
    resolved = server._target_dbmap_config("ear-esp32-s3", profile)
    settings = {
        "CONFIG_SP_MIC_CAPTURE_DIAG": capture,
        "CONFIG_DBMAP_EAR_AUDIO_ENGINE": True,
        "CONFIG_DBMAP_EAR_AE_DIAG": ae,
        "CONFIG_DBMAP_EAR_AE_DIAG_EVERY_N": 10 if ae else None,
        "CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG": serial,
        "CONFIG_DBMAP_EAR_OBS_DECISION_DIAG": decision,
        "CONFIG_DBMAP_EAR_AE_SIG_CAPTURE": csv,
        "CONFIG_DBMAP_DIAG_ENABLED": True,
        "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS": False,
    }
    diag = resolved["diagnostics"]
    assert resolved["profile"] == profile
    assert diag["requested"] == settings
    assert {key: entry["value"] for key, entry in diag["preview"].items()} == settings
    assert diag["preview"]["CONFIG_SP_MIC_CAPTURE_DIAG"][
        "source"
    ] == dbmap_config.diagnostic_overlay(profile)
    assert "generated" not in diag
    catalog = dbmap_config.resolve_defaults_file_dbmap_config(
        node_type="ear",
        chip="esp32s3",
        project=REPO_FIRMWARE / "esp-ear",
        profile=profile,
    )
    assert catalog["product"]["dbmap"]["observation"]["EAR_AE_SIG_CAPTURE"] is csv
    assert "diagnostics" not in catalog


def test_preview_without_profile_preserves_ear_and_output(real_firmware_targets):
    legacy = server._target_dbmap_config("ear-esp32-s3")
    assert "profile" not in legacy
    assert legacy["diagnostics"]["requested"] == {}
    full = server._target_dbmap_config("ear-esp32-s3", "full-diagnostics")
    assert {
        key: entry["value"] for key, entry in legacy["diagnostics"]["preview"].items()
    } == full["diagnostics"]["requested"]
    assert "diagnostics" not in server._target_dbmap_config("output-esp32")


def test_preview_layers_match_idf_companion_order(tmp_path):
    overlay = dbmap_config.diagnostic_overlay("pcm-verification")
    for filename, value in (
        ("sdkconfig.defaults", 2),
        ("sdkconfig.defaults.esp32s3", 3),
        (overlay, 10),
        (f"{overlay}.esp32s3", 12),
    ):
        (tmp_path / filename).write_text(
            f"{dbmap_config.AE_DIAG}=y\n{dbmap_config.AE_CADENCE}={value}\n",
            encoding="utf-8",
        )
    layers = dbmap_config.defaults_files(tmp_path, "esp32s3", "pcm-verification")
    assert dbmap_config.diagnostic_preview(layers, {})[dbmap_config.AE_CADENCE] == {
        "value": 12,
        "source": f"{overlay}.esp32s3",
    }


def test_generated_diagnostic_parser_and_mismatch():
    diag = dbmap_config.generated_diagnostic_settings(
        "# CONFIG_SP_MIC_CAPTURE_DIAG is not set\n"
        "CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG=n\n"
        "CONFIG_DBMAP_EAR_AE_DIAG_EVERY_N=10\n"
        'CONFIG_DBMAP_WIFI_PASSWORD="never exposed"\n'
    )
    assert diag["CONFIG_SP_MIC_CAPTURE_DIAG"] is False
    assert diag["CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG"] is False
    assert diag[dbmap_config.AE_CADENCE] is None
    assert set(diag) == dbmap_config.DIAGNOSTIC_DEFAULTS.keys()
    assert dbmap_config.diagnostic_mismatches({dbmap_config.AE_DIAG: True}, diag) == [
        f"{dbmap_config.AE_DIAG}: requested y, generated n"
    ]
    active = dbmap_config.generated_diagnostic_settings(f"{dbmap_config.AE_DIAG}=y\n")
    assert dbmap_config.diagnostic_mismatches(
        {dbmap_config.AE_CADENCE: 10}, active
    ) == [f"{dbmap_config.AE_CADENCE}: requested 10, generated missing"]
