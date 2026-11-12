"""Resolve effective CONFIG_DBMAP_* values for pre-build review and metadata."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

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
    "mic": (
        "EAR_MIC_MODEL_INMP441",
        "EAR_MIC_SAMPLE_RATE_HZ",
        "EAR_MIC_WINDOW_MS",
        "EAR_MIC_I2S_BCLK_GPIO",
        "EAR_MIC_I2S_WS_GPIO",
        "EAR_MIC_I2S_DIN_GPIO",
        "EAR_MIC_I2S_DIN1_GPIO",
        "EAR_MIC_I2S_MCLK_GPIO",
        "EAR_MIC_L0",
        "EAR_MIC_BACKGROUND",
        "EAR_MIC_BACKGROUND_WINDOW_MS",
        "EAR_MIC_BACKGROUND_BOOT_SETTLE_MS",
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
    "ear": ("node", "diag", "backoff", "mic"),
    "output": ("node", "diag", "backoff", "output"),
}
PRODUCT_DBMAP_GROUPS_BY_NODE_TYPE: dict[str, tuple[str, ...]] = {
    "ear": ("observation",),
    "output": (),
}

GROUPS_BY_NODE_TYPE: dict[str, tuple[str, ...]] = {
    node_type: SP_GROUPS_BY_NODE_TYPE.get(node_type, ())
    + PRODUCT_DBMAP_GROUPS_BY_NODE_TYPE.get(node_type, ())
    for node_type in sorted(
        set(SP_GROUPS_BY_NODE_TYPE) | set(PRODUCT_DBMAP_GROUPS_BY_NODE_TYPE)
    )
}

_CONFIG_LINE = re.compile(
    r"^(?P<name>CONFIG_DBMAP_[A-Z0-9_]+)=(?P<value>.*)$"
)
_NOT_SET_LINE = re.compile(r"^# (?P<name>CONFIG_DBMAP_[A-Z0-9_]+) is not set$")
_SHORT = re.compile(r"^DBMAP_[A-Z0-9_]+$")
# Strict SemVer core (MAJOR.MINOR.PATCH). No pre-release / build metadata.
_SEMVER_CORE = re.compile(r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$")


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


def defaults_files(project: Path, chip: str) -> list[Path]:
    """ESP-IDF auto-loads sdkconfig.defaults and sdkconfig.defaults.<chip> only."""
    files: list[Path] = []
    base = project / "sdkconfig.defaults"
    if base.is_file():
        files.append(base)
    target_specific = project / f"sdkconfig.defaults.{chip}"
    if target_specific.is_file():
        files.append(target_specific)
    return files


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
) -> dict[str, Any]:
    """Merge Kconfig defaults, sdkconfig.defaults, and builder overlays."""
    kconfig_defaults: dict[str, Any] = {}
    for path in kconfig_files(components, extra_component_trees):
        kconfig_defaults.update(parse_kconfig_dbmap_defaults(path.read_text(encoding="utf-8")))

    file_overrides: dict[str, Any] = {}
    for path in defaults_files(project, chip):
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
    return {
        "source": "effective",
        "config": nested,
        "highlights": highlights(effective, kconfig_defaults),
        "notes": [
            "Release firmware_version is assigned at build time (sdkconfig.defaults series base + retained patch history) and is not duplicated under config.sp.node.",
            "Builder overlay sets NTP_SERVER from Hub env NTP_SERVER (default se.pool.ntp.org if unset); overrides sdkconfig.defaults.",
            "WIFI_SSID, WIFI_PASSWORD, and BOOTSTRAP_TOKEN are cleared for reusable images.",
            "config.sp is Sensor Platform; config.product.dbmap is dBmap product (Ear observation today).",
            "config.sp.version is the independent Sensor Platform SemVer from firmware/platform/VERSION (not firmware_version).",
        ],
    }


def resolve_defaults_file_dbmap_config(
    *,
    node_type: str,
    chip: str,
    project: Path,
    platform_dir: Path | None = None,
) -> dict[str, Any]:
    """Only CONFIG_DBMAP_* lines from sdkconfig.defaults / sdkconfig.defaults.<chip>."""
    file_overrides: dict[str, Any] = {}
    for path in defaults_files(project, chip):
        file_overrides.update(parse_sdkconfig_dbmap(path.read_text(encoding="utf-8")))
    sp_version = None
    if platform_dir is not None:
        sp_version = read_platform_version(platform_dir)
    return group_values(file_overrides, node_type, platform_version=sp_version)


def format_log_block(payload: dict[str, Any]) -> str:
    body = {
        "source": payload.get("source", "effective"),
        "config": payload.get("config", {}),
        "highlights": payload.get("highlights", []),
    }
    rendered = json.dumps(body, indent=2, sort_keys=True)
    return (
        "\nEffective firmware config (pre-build; Kconfig defaults + "
        "sdkconfig.defaults + builder overlays; CONFIG_DBMAP_* keys):\n"
        f"{rendered}\n"
    )
