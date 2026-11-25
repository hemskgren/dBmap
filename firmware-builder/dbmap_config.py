"""Resolve effective CONFIG_DBMAP_* values for pre-build review and metadata."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

EAR_DIAGNOSTIC_PROFILES = {
    "production": "Production",
    "development": "Development",
    "pcm-verification": "PCM verification",
    "acoustic-diagnosis": "Acoustic diagnosis",
    "full-diagnostics": "Full diagnostics",
    "calibration": "Calibration",
}

AE_DIAG = "CONFIG_DBMAP_EAR_AE_DIAG"
AE_CADENCE = "CONFIG_DBMAP_EAR_AE_DIAG_EVERY_N"
# Symbols outside the DBMAP parser use their Platform or pinned ESP-IDF defaults.
DIAGNOSTIC_DEFAULTS = {
    "CONFIG_SP_MIC_CAPTURE_DIAG": False,
    "CONFIG_DBMAP_EAR_AUDIO_ENGINE": True,
    AE_DIAG: False,
    AE_CADENCE: 10,
    "CONFIG_DBMAP_EAR_OBS_SERIAL_DIAG": False,
    "CONFIG_DBMAP_EAR_OBS_DECISION_DIAG": False,
    "CONFIG_DBMAP_EAR_AE_SIG_CAPTURE": False,
    "CONFIG_DBMAP_DIAG_ENABLED": True,
    "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS": False,
}


def diagnostic_overlay(profile: str) -> str:
    if profile not in EAR_DIAGNOSTIC_PROFILES:
        raise ValueError("Select a supported EAR diagnostic profile.")
    return f"sdkconfig.defaults.log-{profile}"


def parse_diagnostic_settings(text: str) -> dict[str, Any]:
    values: dict[str, Any] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("# ") and line.endswith(" is not set"):
            key = line.removeprefix("# ").removesuffix(" is not set")
            if key in DIAGNOSTIC_DEFAULTS and key != AE_CADENCE:
                values[key] = False
            continue
        key, sep, value = line.partition("=")
        if not sep or key not in DIAGNOSTIC_DEFAULTS:
            continue
        value = value.strip()
        if key == AE_CADENCE and value.isascii() and value.isdecimal():
            values[key] = int(value)
        elif key != AE_CADENCE and value in ("y", "n"):
            values[key] = value == "y"
        else:
            raise ValueError(f"Invalid value for diagnostic setting {key}.")
    return values


def requested_diagnostic_settings(project: Path, profile: str | None) -> dict[str, Any]:
    if profile is None:
        return {}
    values = parse_diagnostic_settings(
        (project / diagnostic_overlay(profile)).read_text(encoding="utf-8")
    )
    required = DIAGNOSTIC_DEFAULTS.keys() - {AE_CADENCE}
    if values.get(AE_DIAG):
        required.add(AE_CADENCE)
    missing = required - values.keys()
    if missing:
        raise ValueError(
            f"Incomplete diagnostic profile {profile}: missing {', '.join(sorted(missing))}."
        )
    if not values[AE_DIAG]:
        values[AE_CADENCE] = None
    return values


def diagnostic_preview(
    layers: list[Path], kconfig_defaults: dict[str, Any]
) -> dict[str, dict[str, Any]]:
    rows = {}
    for key, fallback in DIAGNOSTIC_DEFAULTS.items():
        value = (
            kconfig_defaults.get(short_name(key), fallback)
            if key.startswith("CONFIG_DBMAP_")
            else fallback
        )
        origin = (
            "ESP-IDF Kconfig default"
            if key.startswith("CONFIG_FREERTOS_")
            else "Kconfig default"
        )
        rows[key] = {"value": value, "source": origin}
    for path in layers:
        for key, value in parse_diagnostic_settings(
            path.read_text(encoding="utf-8")
        ).items():
            rows[key] = {"value": value, "source": path.name}
    if not rows[AE_DIAG]["value"]:
        rows[AE_CADENCE] = {
            "value": None,
            "source": f"inactive because {AE_DIAG} is off",
        }
    return rows


def generated_diagnostic_settings(text: str) -> dict[str, Any]:
    values = parse_diagnostic_settings(text)
    result = {
        key: values.get(key, False) for key in DIAGNOSTIC_DEFAULTS if key != AE_CADENCE
    }
    result[AE_CADENCE] = values.get(AE_CADENCE) if result[AE_DIAG] else None
    return result


def diagnostic_mismatches(
    requested: dict[str, Any], generated: dict[str, Any]
) -> list[str]:
    def display(key: str, value: Any) -> str:
        if value is None:
            return (
                "inactive"
                if key == AE_CADENCE and not generated[AE_DIAG]
                else "missing"
            )
        if isinstance(value, bool):
            return "y" if value else "n"
        return str(value)

    return [
        f"{key}: requested {('inactive' if value is None else display(key, value))}, "
        f"generated {display(key, generated[key])}"
        for key, value in requested.items()
        if generated[key] != value
    ]


# Hierarchy used for list/metadata. Keys are CONFIG_DBMAP_* without the prefix.
GROUP_KEYS: dict[str, tuple[str, ...]] = {
    "node": (
        # FIRMWARE_VERSION is omitted on purpose: catalog/metadata and List use the
        # top-level firmware_version (builder-bumped). Nested CONFIG base 0.7.0 was
        # misleading next to e.g. firmware_version 0.7.10.
        "NODE_TYPE_EAR",
        "NODE_TYPE_OUTPUT",
        "HARDWARE_REVISION",
        "HUB_HTTP_URL",
        "NTP_SERVER",
        "WIFI_SSID",
        "WIFI_PASSWORD",
        "BOOTSTRAP_TOKEN",
    ),
    "diag": (
        "DIAG_ENABLED",
        "DIAG_RSSI_DEGRADED_DBM",
        "DIAG_COUNTER_SATURATE",
    ),
    "backoff": (
        "BACKOFF_MQTT_INITIAL_MS",
        "BACKOFF_MQTT_MAX_MS",
        "BACKOFF_MQTT_STABLE_MS",
        "BACKOFF_WIFI_INITIAL_MS",
        "BACKOFF_WIFI_MAX_MS",
        "BACKOFF_HTTPS_INITIAL_MS",
        "BACKOFF_HTTPS_MAX_MS",
    ),
    # Platform microphone hardware/background options are CONFIG_SP_MIC_* (not
    # CONFIG_DBMAP_*). They are intentionally absent from this DBMAP catalog.
    # Product analysis enable that selects Platform PCM + L0 lives under acoustic.
    "acoustic": (
        "EAR_MIC_L0",  # product select → SP_MIC_PCM_RETAIN + SP_AUDIO_L0
    ),
    "observation": (
        "EAR_OBS_SEQ_BLOCK_SIZE",
        "EAR_OBS_RMS_ACTIVITY",
        "EAR_OBS_ENTER_THRESHOLD_DBFS_X1000",
        "EAR_OBS_EXIT_THRESHOLD_DBFS_X1000",
        "EAR_OBS_MIN_ACTIVITY_MS",
        "EAR_OBS_EXIT_HOLD_MS",
        "EAR_OBS_BOOT_SETTLE_MS",
        "EAR_OBS_INTERVAL_MS",
        "EAR_OBS_MAX_INVALID_HOLD_MS",
        "EAR_OBS_RELATIVE_END",
        "EAR_OBS_REL_RETURN_MARGIN_DB_X1000",
        "EAR_OBS_REL_EXIT_HOLD_MS",
        "EAR_OBS_REL_CANCEL_HYST_DB_X1000",
        "EAR_OBS_REL_MIN_CONTRAST_DB_X1000",
        "EAR_OBS_REL_BASELINE_STALE_MS",
        "EAR_OBS_REL_LATE_ARM",
        "EAR_OBS_TEST_PUBLISH_ONCE",
        "EAR_OBS_TEST_SIGNAL_LEVEL_DBFS",
        "EAR_OBS_TEST_DURATION_MS",
        "EAR_OBS_L0",
        "EAR_OBS_L0_MAX_WINDOWS",
        "EAR_OBS_PUBLISH_L0",
        "EAR_OBS_L1",
        "EAR_OBS_L2",
        "EAR_OBS_L2_PUBLISH",
        "EAR_OBS_SERIAL_DIAG",
        "EAR_OBS_DECISION_DIAG",
        # AE signature CSV capture (AS-001 calibration); ear sdkconfig.defaults on.
        "EAR_AE_SIG_CAPTURE",
    ),
    # Platform Output hardware profile pins (defaults today; future per-board overlays).
    "output": (
        "AUDIO_I2S_BCLK_GPIO",
        "AUDIO_I2S_WS_GPIO",
        "AUDIO_I2S_DOUT_GPIO",
        "AUDIO_SD_MODE_GPIO",
    ),
}

# Flat groups collected per node kind, then nested as config.sp / config.product.dbmap.
SP_GROUPS_BY_NODE_TYPE: dict[str, tuple[str, ...]] = {
    "ear": ("node", "diag", "backoff"),
    "output": ("node", "diag", "backoff", "output"),
}
PRODUCT_DBMAP_GROUPS_BY_NODE_TYPE: dict[str, tuple[str, ...]] = {
    "ear": ("acoustic", "observation"),
    "output": (),
}

GROUPS_BY_NODE_TYPE: dict[str, tuple[str, ...]] = {
    node_type: SP_GROUPS_BY_NODE_TYPE.get(node_type, ())
    + PRODUCT_DBMAP_GROUPS_BY_NODE_TYPE.get(node_type, ())
    for node_type in sorted(
        set(SP_GROUPS_BY_NODE_TYPE) | set(PRODUCT_DBMAP_GROUPS_BY_NODE_TYPE)
    )
}

_CONFIG_LINE = re.compile(r"^(?P<name>CONFIG_DBMAP_[A-Z0-9_]+)=(?P<value>.*)$")
_NOT_SET_LINE = re.compile(r"^# (?P<name>CONFIG_DBMAP_[A-Z0-9_]+) is not set$")
_SHORT = re.compile(r"^DBMAP_[A-Z0-9_]+$")
# Firmware mic reporting option (CONFIG_SP_*; included in build metadata only).
_SP_MIC_SHOW_ZERO = re.compile(
    r"^CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS=(?P<value>.*)$"
)
_SP_MIC_SHOW_ZERO_NOT_SET = re.compile(
    r"^# CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS is not set$"
)
# Strict SemVer core (MAJOR.MINOR.PATCH). No pre-release / build metadata.
_SEMVER_CORE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")

# Matches Kconfig default for SP_MIC_SHOW_ZERO_ERROR_COUNTERS (default n).
_DEFAULT_SHOW_ZERO_ERROR_COUNTERS = False


def is_valid_sp_version(value: str) -> bool:
    """True if value is a SemVer core version string (MAJOR.MINOR.PATCH)."""
    return bool(value) and _SEMVER_CORE.fullmatch(value) is not None


def read_platform_version(platform_dir: Path) -> str:
    """
    Read the authoritative Sensor Platform version from platform/VERSION.

    Raises ValueError if the file is missing or the first non-empty line is not
    a valid SemVer core version. Callers must not invent a fallback version.
    """
    path = platform_dir / "VERSION"
    if not path.is_file():
        raise ValueError(f"Missing Sensor Platform VERSION file: {path}")
    raw = path.read_text(encoding="utf-8")
    line = ""
    for candidate in raw.splitlines():
        stripped = candidate.strip()
        if stripped and not stripped.startswith("#"):
            line = stripped
            break
    if not is_valid_sp_version(line):
        raise ValueError(
            f"Malformed Sensor Platform version in {path}: {raw!r} "
            "(expected MAJOR.MINOR.PATCH SemVer core)"
        )
    return line


def inject_sp_version(config: dict[str, Any], version: str) -> dict[str, Any]:
    """Set config.sp.version without inventing Kconfig keys."""
    if not is_valid_sp_version(version):
        raise ValueError(f"Invalid Sensor Platform version: {version!r}")
    if not isinstance(config, dict):
        raise TypeError("config must be an object")
    sp = config.get("sp")
    if sp is None:
        sp = {}
        config["sp"] = sp
    if not isinstance(sp, dict):
        raise TypeError("config.sp must be an object")
    sp["version"] = version
    return config


def parse_show_zero_error_counters(text: str) -> bool | None:
    """
    Parse CONFIG_SP_MIC_SHOW_ZERO_ERROR_COUNTERS from sdkconfig / defaults text.

    Returns True/False when the symbol appears, or None when absent.
    """
    found: bool | None = None
    for raw in text.splitlines():
        line = raw.strip()
        if _SP_MIC_SHOW_ZERO_NOT_SET.match(line):
            found = False
            continue
        match = _SP_MIC_SHOW_ZERO.match(line)
        if match:
            raw_value = match.group("value").strip()
            found = raw_value == "y"
    return found


def inject_show_zero_error_counters(
    config: dict[str, Any], value: bool
) -> dict[str, Any]:
    """
    Record firmware show_zero_error_counters in build metadata.

    Nested as config.sp.mic.show_zero_error_counters — not a Reported State
    field and not a Hub/map setting.
    """
    if not isinstance(config, dict):
        raise TypeError("config must be an object")
    sp = config.get("sp")
    if sp is None:
        sp = {}
        config["sp"] = sp
    if not isinstance(sp, dict):
        raise TypeError("config.sp must be an object")
    mic = sp.get("mic")
    if mic is None:
        mic = {}
        sp["mic"] = mic
    if not isinstance(mic, dict):
        raise TypeError("config.sp.mic must be an object")
    mic["show_zero_error_counters"] = bool(value)
    return config


def resolve_show_zero_error_counters(
    *,
    project: Path,
    chip: str,
    platform_dir: Path | None = None,
) -> bool:
    """Effective firmware option from Kconfig default + sdkconfig.defaults."""
    value = _DEFAULT_SHOW_ZERO_ERROR_COUNTERS
    if platform_dir is not None:
        kconfig = platform_dir / "mic" / "Kconfig"
        if kconfig.is_file():
            text = kconfig.read_text(encoding="utf-8")
            # Prefer an explicit `default n` / `default y` under the symbol.
            in_symbol = False
            for raw in text.splitlines():
                line = raw.strip()
                if line.startswith("config SP_MIC_SHOW_ZERO_ERROR_COUNTERS"):
                    in_symbol = True
                    continue
                if in_symbol:
                    if line.startswith(("config ", "endmenu")):
                        break
                    if line == "default y":
                        value = True
                        break
                    if line == "default n":
                        value = False
                        break
    for path in defaults_files(project, chip):
        parsed = parse_show_zero_error_counters(path.read_text(encoding="utf-8"))
        if parsed is not None:
            value = parsed
    return value


def short_name(config_name: str) -> str:
    if config_name.startswith("CONFIG_DBMAP_"):
        return config_name[len("CONFIG_DBMAP_") :]
    if config_name.startswith("DBMAP_"):
        return config_name[len("DBMAP_") :]
    return config_name


def full_name(short: str) -> str:
    return short if short.startswith("CONFIG_") else f"CONFIG_DBMAP_{short}"


def parse_sdkconfig_dbmap(text: str) -> dict[str, Any]:
    """Parse CONFIG_DBMAP_* assignments from sdkconfig / defaults text."""
    values: dict[str, Any] = {}
    for raw in text.splitlines():
        line = raw.strip()
        not_set = _NOT_SET_LINE.match(line)
        if not_set:
            values[short_name(not_set.group("name"))] = False
            continue
        match = _CONFIG_LINE.match(line)
        if not match:
            continue
        name = short_name(match.group("name"))
        raw_value = match.group("value").strip()
        if raw_value == "y":
            values[name] = True
        elif raw_value == "n":
            values[name] = False
        else:
            try:
                values[name] = json.loads(raw_value)
            except json.JSONDecodeError:
                values[name] = raw_value
    return values


def parse_kconfig_dbmap_defaults(text: str) -> dict[str, Any]:
    """Extract default values for DBMAP_* symbols from a Kconfig fragment."""
    defaults: dict[str, Any] = {}
    choice_members: list[str] = []
    choice_default: str | None = None
    in_choice = False
    current: str | None = None
    current_type: str | None = None

    def close_choice() -> None:
        nonlocal choice_members, choice_default, in_choice
        if in_choice and choice_members:
            selected = short_name(choice_default) if choice_default else None
            for member in choice_members:
                key = short_name(member)
                defaults[key] = selected == key
        choice_members = []
        choice_default = None
        in_choice = False

    for raw in text.splitlines():
        stripped = raw.strip()
        if not stripped or stripped.startswith(("help", "#")):
            continue
        if stripped.startswith("choice "):
            close_choice()
            in_choice = True
            current = None
            current_type = None
            continue
        if stripped == "endchoice":
            close_choice()
            current = None
            current_type = None
            continue
        if stripped.startswith(("menu ", "endmenu", "menuconfig ")):
            current = None
            current_type = None
            continue
        if stripped.startswith("config "):
            symbol = stripped[len("config ") :].strip()
            if not symbol.startswith("DBMAP_"):
                current = None
                current_type = None
                continue
            current = symbol
            current_type = None
            if in_choice:
                choice_members.append(symbol)
            continue
        if current is None:
            if in_choice and stripped.startswith("default "):
                token = stripped[len("default ") :].split()[0]
                if _SHORT.fullmatch(token):
                    choice_default = token
            continue
        if stripped.startswith("bool"):
            current_type = "bool"
            if current not in defaults and not in_choice:
                defaults[short_name(current)] = False
            continue
        if stripped.startswith("int"):
            current_type = "int"
            continue
        if stripped.startswith("string"):
            current_type = "string"
            continue
        if not stripped.startswith("default "):
            continue
        token = stripped[len("default ") :].strip()
        if in_choice and _SHORT.fullmatch(token.split()[0]):
            choice_default = token.split()[0]
            continue
        default_token = token.split("#", 1)[0].strip()
        if current_type == "bool" or default_token in {"y", "n"}:
            defaults[short_name(current)] = default_token == "y"
        elif current_type == "int" or re.fullmatch(r"-?\d+", default_token):
            defaults[short_name(current)] = int(default_token)
        elif default_token.startswith('"'):
            defaults[short_name(current)] = json.loads(default_token)
        elif _SHORT.fullmatch(default_token):
            # Non-choice symbol defaulting to another symbol name — treat as unset bool.
            defaults[short_name(current)] = False
    close_choice()
    return defaults


def defaults_files(project: Path, chip: str, profile: str | None = None) -> list[Path]:
    """ESP-IDF inserts each chip-specific companion after its defaults layer."""
    names = ["sdkconfig.defaults"]
    if profile is not None:
        names.append(diagnostic_overlay(profile))
    return [
        path
        for name in names
        for path in (project / name, project / f"{name}.{chip}")
        if path.is_file()
    ]


def kconfig_files(components: Path, extra_trees: list[Path]) -> list[Path]:
    paths = [components / "node_common" / "Kconfig.projbuild"]
    # Platform also hosts mic/observation Kconfig beside node_common.
    paths.extend(sorted(components.glob("*/Kconfig")))
    paths.extend(sorted(components.glob("*/Kconfig.projbuild")))
    for tree in extra_trees:
        paths.extend(sorted(tree.glob("*/Kconfig")))
        paths.extend(sorted(tree.glob("*/Kconfig.projbuild")))
    # De-dupe while preserving order (node_common projbuild may match the glob).
    seen: set[Path] = set()
    unique: list[Path] = []
    for path in paths:
        if path.is_file() and path not in seen:
            seen.add(path)
            unique.append(path)
    return unique


def _group_subset(
    flat: dict[str, Any], group_names: tuple[str, ...]
) -> dict[str, dict[str, Any]]:
    grouped: dict[str, dict[str, Any]] = {}
    for group in group_names:
        entries: dict[str, Any] = {}
        for key in GROUP_KEYS[group]:
            if key in flat:
                entries[key] = flat[key]
        if entries:
            grouped[group] = entries
    return grouped


def group_values(
    flat: dict[str, Any],
    node_type: str,
    *,
    platform_version: str | None = None,
) -> dict[str, Any]:
    """Nest CONFIG short-names under config.sp / config.product.dbmap ownership."""
    sp = _group_subset(flat, SP_GROUPS_BY_NODE_TYPE.get(node_type, ("node",)))
    product_dbmap = _group_subset(
        flat, PRODUCT_DBMAP_GROUPS_BY_NODE_TYPE.get(node_type, ())
    )
    config: dict[str, Any] = {"sp": sp}
    if product_dbmap:
        config["product"] = {"dbmap": product_dbmap}
    else:
        config["product"] = {"dbmap": {}}
    if platform_version is not None:
        inject_sp_version(config, platform_version)
    return config


def attach_mic_report_metadata(
    config: dict[str, Any],
    *,
    project: Path,
    chip: str,
    platform_dir: Path | None,
) -> dict[str, Any]:
    """Always attach config.sp.mic.show_zero_error_counters for Ear builds."""
    value = resolve_show_zero_error_counters(
        project=project, chip=chip, platform_dir=platform_dir
    )
    return inject_show_zero_error_counters(config, value)


def highlights(
    effective: dict[str, Any], kconfig_defaults: dict[str, Any]
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for key, value in sorted(effective.items()):
        if key not in kconfig_defaults:
            continue
        default = kconfig_defaults[key]
        if value != default:
            rows.append(
                {
                    "key": key,
                    "value": value,
                    "vs_kconfig_default": default,
                }
            )
    return rows


def resolve_effective_dbmap_config(
    *,
    node_type: str,
    chip: str,
    project: Path,
    components: Path,
    extra_component_trees: list[Path],
    ntp_server: str,
    profile: str | None = None,
) -> dict[str, Any]:
    """Merge Kconfig defaults, sdkconfig.defaults, and builder overlays."""
    kconfig_defaults: dict[str, Any] = {}
    for path in kconfig_files(components, extra_component_trees):
        kconfig_defaults.update(
            parse_kconfig_dbmap_defaults(path.read_text(encoding="utf-8"))
        )

    file_overrides: dict[str, Any] = {}
    for path in defaults_files(project, chip, profile):
        file_overrides.update(parse_sdkconfig_dbmap(path.read_text(encoding="utf-8")))

    effective = dict(kconfig_defaults)
    effective.update(file_overrides)

    # Builder always clears device secrets and injects NTP for reusable images.
    effective["WIFI_SSID"] = ""
    effective["WIFI_PASSWORD"] = ""
    effective["BOOTSTRAP_TOKEN"] = ""
    effective["NTP_SERVER"] = ntp_server

    # Normalize node-kind choice so both booleans are present and exclusive.
    if node_type == "ear":
        effective["NODE_TYPE_EAR"] = True
        effective["NODE_TYPE_OUTPUT"] = False
    elif node_type == "output":
        effective["NODE_TYPE_EAR"] = False
        effective["NODE_TYPE_OUTPUT"] = True

    sp_version = read_platform_version(components)
    nested = group_values(effective, node_type, platform_version=sp_version)
    if node_type == "ear":
        attach_mic_report_metadata(
            nested,
            project=project,
            chip=chip,
            platform_dir=components,
        )
    return {
        "source": "effective",
        "config": nested,
        "highlights": highlights(effective, kconfig_defaults),
        **(
            {
                "diagnostics": {
                    "requested": requested_diagnostic_settings(project, profile),
                    "preview": diagnostic_preview(
                        defaults_files(project, chip, profile), kconfig_defaults
                    ),
                },
            }
            if node_type == "ear"
            else {}
        ),
        "notes": [
            "Release firmware_version is assigned at build time (sdkconfig.defaults series base + retained patch history) and is not duplicated under config.sp.node.",
            "Builder overlay sets NTP_SERVER from Hub env NTP_SERVER (default se.pool.ntp.org if unset); overrides sdkconfig.defaults.",
            "WIFI_SSID, WIFI_PASSWORD, and BOOTSTRAP_TOKEN are cleared for reusable images.",
            "config.sp is Sensor Platform; config.product.dbmap is dBmap product (Ear acoustic + observation). Most Platform mic flags remain CONFIG_SP_* outside the DBMAP catalog; config.sp.mic.show_zero_error_counters is included as build metadata for the firmware reporting option.",
            "config.sp.version is the independent Sensor Platform SemVer from firmware/platform/VERSION (not firmware_version).",
        ],
    }


def resolve_defaults_file_dbmap_config(
    *,
    node_type: str,
    chip: str,
    project: Path,
    platform_dir: Path | None = None,
    profile: str | None = None,
) -> dict[str, Any]:
    """Only CONFIG_DBMAP_* lines from sdkconfig.defaults / sdkconfig.defaults.<chip>."""
    file_overrides: dict[str, Any] = {}
    for path in defaults_files(project, chip, profile):
        file_overrides.update(parse_sdkconfig_dbmap(path.read_text(encoding="utf-8")))
    sp_version = None
    if platform_dir is not None:
        sp_version = read_platform_version(platform_dir)
    nested = group_values(file_overrides, node_type, platform_version=sp_version)
    if node_type == "ear":
        attach_mic_report_metadata(
            nested,
            project=project,
            chip=chip,
            platform_dir=platform_dir,
        )
    return nested


def format_log_block(payload: dict[str, Any]) -> str:
    body = {
        "source": payload.get("source", "effective"),
        "config": payload.get("config", {}),
        "highlights": payload.get("highlights", []),
    }
    if "diagnostics" in payload:
        body["diagnostics"] = payload["diagnostics"]
    rendered = json.dumps(body, indent=2, sort_keys=True)
    return (
        "\nResolved firmware config preview (pre-build; Kconfig defaults + "
        "sdkconfig.defaults + builder overlays; CONFIG_DBMAP_* keys):\n"
        f"{rendered}\n"
    )
