#!/usr/bin/env python3
"""Control Home Assistant media players for reproducible EAR microphone tests."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from lib.audio_scenario import (
    DEFAULT_SCENARIO_DIR,
    AudioScenarioError,
    format_dry_run_plan,
    format_execution_report,
    list_scenario_files,
    load_scenario_file,
    run_scenario,
)
from lib.homeassistant_client import (
    HomeAssistantClient,
    HomeAssistantConfig,
    HomeAssistantError,
    MediaPlayerInfo,
    clamp_volume,
    load_homeassistant_config,
    parse_volume,
    redact_secrets,
    validate_entity_id,
    validate_playable_url,
)
from lib.hub_cli import load_project_env, print_help_without_arguments


def add_ha_url_argument(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--ha-url",
        dest="ha_url",
        help="Home Assistant base URL (default: HOMEASSISTANT_URL from .env)",
    )


def add_connection_arguments(parser: argparse.ArgumentParser) -> None:
    add_ha_url_argument(parser)
    parser.add_argument(
        "--entity",
        help=(
            "media_player entity id "
            "(default: HOMEASSISTANT_MEDIA_PLAYER from .env)"
        ),
    )


def resolve_config(
    args: argparse.Namespace,
    *,
    require_media_player: bool = True,
    require_token: bool = True,
    require_url: bool = True,
) -> HomeAssistantConfig:
    return load_homeassistant_config(
        url=getattr(args, "ha_url", None),
        entity_id=getattr(args, "entity", None),
        require_media_player=require_media_player,
        require_token=require_token,
        require_url=require_url,
    )


def selected_entity(args: argparse.Namespace, config: HomeAssistantConfig) -> str:
    return validate_entity_id(args.entity or config.entity_id or "")


def _format_optional_float(value: float | None, *, digits: int = 2) -> str:
    if value is None:
        return "n/a"
    return f"{value:.{digits}f}"


def format_media_player_list_lines(
    players: list[MediaPlayerInfo],
    *,
    default_entity_id: str | None,
) -> list[str]:
    if not players:
        lines = ["No media_player entities found."]
        if default_entity_id:
            lines.append(f"Configured default: {default_entity_id} (not present)")
        else:
            lines.append("Configured default: unset")
        return lines

    lines = [
        "entity_id  default  state  volume  supported_features  friendly_name",
        "---------  -------  -----  ------  -------------------  -------------",
    ]
    for player in players:
        mark = "yes" if player.is_default else "no"
        name = player.friendly_name or "—"
        features = (
            str(player.supported_features)
            if player.supported_features is not None
            else "n/a"
        )
        lines.append(
            f"{player.entity_id}  {mark}  {player.state}  "
            f"{_format_optional_float(player.volume_level)}  "
            f"{features}  {name}"
        )
    if default_entity_id:
        if any(player.is_default for player in players):
            lines.append(f"Configured default: {default_entity_id}")
        else:
            lines.append(
                f"Configured default: {default_entity_id} (not present in discovery)"
            )
    else:
        lines.append("Configured default: unset")
    return lines


def format_media_player_status_lines(player: MediaPlayerInfo) -> list[str]:
    lines = [
        f"entity_id: {player.entity_id}",
        f"default: {'yes' if player.is_default else 'no'}",
        f"state: {player.state}",
        f"friendly_name: {player.friendly_name or 'n/a'}",
        f"volume_level: {_format_optional_float(player.volume_level)}",
        (
            "supported_features: "
            + (
                str(player.supported_features)
                if player.supported_features is not None
                else "n/a"
            )
        ),
        f"media_title: {player.media_title or 'n/a'}",
        f"media_position: {_format_optional_float(player.media_position, digits=3)}",
        f"media_duration: {_format_optional_float(player.media_duration, digits=3)}",
        "attributes:",
    ]
    if not player.attributes:
        lines.append("  (none)")
    else:
        for key in sorted(player.attributes):
            lines.append(f"  {key}: {player.attributes[key]!r}")
    return lines


def cmd_ping(args: argparse.Namespace) -> int:
    config = resolve_config(args, require_media_player=False)
    client = HomeAssistantClient(config)
    result = client.ping()
    status = (
        str(result.http_status) if result.http_status is not None else "n/a"
    )
    reach = "reachable" if result.reachable else "unreachable"
    message = redact_secrets(result.message, config.token)
    print(
        f"Home Assistant {reach}  http_status={status}  "
        f"elapsed_ms={result.elapsed_ms:.1f}  {message}"
    )
    print(f"url={config.base_url}")
    if result.reachable and result.authenticated:
        return 0
    return 1


def cmd_status(args: argparse.Namespace) -> int:
    config = resolve_config(args, require_media_player=False)
    client = HomeAssistantClient(config)
    entity = getattr(args, "entity", None)
    report = client.status(entity_id=entity)
    volume_default = (
        f"{report.default_volume:.2f}"
        if report.default_volume is not None
        else "unset"
    )
    elapsed = (
        f"{report.api_elapsed_ms:.1f}"
        if report.api_elapsed_ms is not None
        else "n/a"
    )
    print("Home Assistant status (read-only; no playback changes)")
    print(f"  ha_url: {report.ha_url}")
    print(f"  default_volume: {volume_default}")
    print("  api:")
    print(f"    reachable: {report.api_reachable}")
    print(f"    authenticated: {report.api_authenticated}")
    print(f"    http_status: {report.api_http_status}")
    print(f"    elapsed_ms: {elapsed}")
    print(f"    message: {redact_secrets(report.api_message, config.token)}")
    print("  media_player:")
    print(f"    configured_entity: {report.entity_id or 'unset'}")
    print(f"    exists: {report.player_exists}")
    print(f"    state: {report.player_state or 'n/a'}")
    if report.player_volume is None:
        print("    volume_level: n/a")
    else:
        print(f"    volume_level: {report.player_volume:.2f}")
    print(
        f"    message: {redact_secrets(report.player_message, config.token)}"
    )

    if not report.api_reachable or report.api_authenticated is not True:
        return 1
    if report.player_exists is False:
        return 1
    if report.entity_id is None:
        return 1
    return 0


def cmd_audio_list(args: argparse.Namespace) -> int:
    config = resolve_config(args, require_media_player=False)
    client = HomeAssistantClient(config)
    players = client.list_media_players(default_entity_id=config.entity_id)
    for line in format_media_player_list_lines(
        players, default_entity_id=config.entity_id
    ):
        print(line)
    return 0


def cmd_audio_status(args: argparse.Namespace) -> int:
    # Require an explicit --entity or configured default; never pick arbitrarily.
    config = resolve_config(args, require_media_player=True)
    client = HomeAssistantClient(config)
    entity = selected_entity(args, config)
    player = client.inspect_media_player(
        entity,
        default_entity_id=config.entity_id,
    )
    for line in format_media_player_status_lines(player):
        print(line)
    return 0


def cmd_audio_volume(args: argparse.Namespace) -> int:
    config = resolve_config(args)
    client = HomeAssistantClient(config)
    entity = selected_entity(args, config)

    modes = [
        name
        for name, value in (
            ("set", args.set_volume),
            ("up", args.up),
            ("down", args.down),
        )
        if value is not None
    ]
    if len(modes) > 1:
        raise HomeAssistantError("use only one of --set, --up, or --down")

    if args.set_volume is not None:
        level = client.set_volume_level(parse_volume(args.set_volume), entity_id=entity)
        print(f"{entity} volume_level={level:.2f} (set)")
        return 0

    current = client.get_volume_level(entity_id=entity)
    if args.up is not None:
        delta = parse_volume(args.up, label="--up")
        level = client.set_volume_level(
            clamp_volume(current + delta), entity_id=entity
        )
        print(f"{entity} volume_level={level:.2f} (was {current:.2f}, up {delta:.2f})")
        return 0
    if args.down is not None:
        delta = parse_volume(args.down, label="--down")
        level = client.set_volume_level(
            clamp_volume(current - delta), entity_id=entity
        )
        print(
            f"{entity} volume_level={level:.2f} (was {current:.2f}, down {delta:.2f})"
        )
        return 0

    print(f"{entity} volume_level={current:.2f}")
    return 0


def cmd_audio_play(args: argparse.Namespace) -> int:
    config = resolve_config(args)
    client = HomeAssistantClient(config)
    entity = selected_entity(args, config)
    url = validate_playable_url(args.url)

    volume: float | None
    if args.volume is not None:
        volume = parse_volume(args.volume)
    elif config.default_volume is not None:
        volume = config.default_volume
    else:
        volume = None

    result = client.play_media(
        url,
        entity_id=entity,
        media_content_type=args.media_content_type,
        volume=volume,
    )
    if not getattr(args, "verify", False):
        print(json.dumps(result, indent=2, sort_keys=True))
        print(
            "Note: HA accepted the request; playback completion is not confirmed.",
            file=sys.stderr,
        )
        return 0

    verification = client.verify_playback(
        result["media_content_id"],
        entity_id=entity,
    )
    result["verification"] = verification.as_dict()
    print(json.dumps(result, indent=2, sort_keys=True))
    print(
        f"play_media: accepted; verification={verification.result} "
        f"(HA player state only, not acoustic). {verification.detail}",
        file=sys.stderr,
    )
    if verification.result == "verification_failed":
        return 1
    return 0


def cmd_audio_pause(args: argparse.Namespace) -> int:
    config = resolve_config(args)
    client = HomeAssistantClient(config)
    entity = selected_entity(args, config)
    result = client.pause(entity_id=entity)
    print(json.dumps(result, indent=2, sort_keys=True))
    print(
        "Note: HA accepted media_pause; audible pause is not confirmed.",
        file=sys.stderr,
    )
    return 0


def cmd_audio_resume(args: argparse.Namespace) -> int:
    config = resolve_config(args)
    client = HomeAssistantClient(config)
    entity = selected_entity(args, config)
    result = client.resume(entity_id=entity)
    print(json.dumps(result, indent=2, sort_keys=True))
    print(
        "Note: HA accepted media_play (resume); audible playback is not confirmed.",
        file=sys.stderr,
    )
    return 0


def cmd_audio_stop(args: argparse.Namespace) -> int:
    config = resolve_config(args)
    client = HomeAssistantClient(config)
    entity = selected_entity(args, config)
    result = client.stop(entity_id=entity)
    print(json.dumps(result, indent=2, sort_keys=True))
    print(
        "Note: HA accepted media_stop; stop completion is not confirmed.",
        file=sys.stderr,
    )
    return 0


def cmd_scenario_list(_args: argparse.Namespace) -> int:
    directory = DEFAULT_SCENARIO_DIR
    files = list_scenario_files(directory)
    if not files:
        print(f"No scenario JSON files in {directory}")
        return 0
    for path in files:
        try:
            scenario = load_scenario_file(path)
            status = "ok"
            detail = scenario["name"]
        except AudioScenarioError as exc:
            status = "invalid"
            detail = str(exc)
        print(f"{path.relative_to(directory.parent.parent)}  [{status}]  {detail}")
    return 0


def cmd_scenario_validate(args: argparse.Namespace) -> int:
    path = Path(args.scenario_file)
    scenario = load_scenario_file(path)
    print(json.dumps(scenario, indent=2, sort_keys=True))
    print(f"Scenario valid: {path} ({len(scenario['steps'])} steps)", file=sys.stderr)
    return 0


def cmd_scenario_run(args: argparse.Namespace) -> int:
    path = Path(args.scenario_file)
    scenario = load_scenario_file(path)
    dry_run = bool(args.dry_run)
    restore_volume = not bool(getattr(args, "no_restore", False))
    client: HomeAssistantClient | None = None

    # Dry-run needs the effective entity for the plan, not live HA credentials.
    config = resolve_config(
        args,
        require_media_player=True,
        require_token=not dry_run,
        require_url=not dry_run,
    )
    entity = selected_entity(args, config)
    if not dry_run:
        client = HomeAssistantClient(config)

    if dry_run:
        for line in format_dry_run_plan(scenario, entity_id=entity):
            print(line)
    else:
        print(
            f"Scenario {scenario['name']!r} "
            f"({len(scenario['steps'])} steps, live, entity={entity})",
            file=sys.stderr,
        )

    report = run_scenario(
        scenario,
        client,
        entity_id=entity,
        dry_run=dry_run,
        restore_volume=restore_volume,
    )
    for result in report.steps:
        tag = "dry-run" if result.dry_run else ("ok" if result.ok else "FAIL")
        print(
            f"[{tag}] step {result.index}/{len(scenario['steps'])} "
            f"action={result.action} "
            f"start={result.started_at_utc} end={result.ended_at_utc} "
            f"duration_s={result.duration_s:.3f} {result.detail}"
        )
    for line in format_execution_report(report):
        print(line)

    if report.error:
        # Preserve original error; cleanup issues already listed in the report.
        message = report.error
        if report.cleanup_errors:
            message = (
                f"{message} "
                f"(cleanup issues: {len(report.cleanup_errors)}; see report)"
            )
        raise AudioScenarioError(message)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Control a Home Assistant media player for controlled EAR "
            "microphone stimulus tests."
        )
    )
    top = parser.add_subparsers(dest="command", required=True)

    ping = top.add_parser(
        "ping",
        help="Minimal authenticated GET /api/ (reachable / HTTP status / latency)",
    )
    add_ha_url_argument(ping)
    ping.set_defaults(func=cmd_ping)

    status = top.add_parser(
        "status",
        help=(
            "Read-only API + configured media-player probe "
            "(does not play/pause/stop)"
        ),
    )
    add_connection_arguments(status)
    status.set_defaults(func=cmd_status)

    audio = top.add_parser("audio", help="Media-player volume and playback controls")
    audio_cmds = audio.add_subparsers(dest="audio_command", required=True)

    audio_list = audio_cmds.add_parser(
        "list",
        help=(
            "Discover media_player entities (entity id, name, state, features, "
            "volume; marks HOMEASSISTANT_MEDIA_PLAYER default)"
        ),
    )
    add_ha_url_argument(audio_list)
    audio_list.set_defaults(func=cmd_audio_list)

    audio_status = audio_cmds.add_parser(
        "status",
        help=(
            "Inspect one media_player (default: HOMEASSISTANT_MEDIA_PLAYER; "
            "requires configured or --entity; does not pick arbitrarily)"
        ),
    )
    add_connection_arguments(audio_status)
    audio_status.set_defaults(func=cmd_audio_status)

    volume = audio_cmds.add_parser(
        "volume",
        help="Read or change media-player volume (0.0–1.0 via HA volume_set)",
    )
    add_connection_arguments(volume)
    volume.add_argument(
        "--set",
        dest="set_volume",
        type=float,
        help="Set absolute volume_level in [0.0, 1.0]",
    )
    volume.add_argument(
        "--up",
        type=float,
        help="Increase volume_level by this amount (clamped to 1.0)",
    )
    volume.add_argument(
        "--down",
        type=float,
        help="Decrease volume_level by this amount (clamped to 0.0)",
    )
    volume.set_defaults(func=cmd_audio_volume)

    play = audio_cmds.add_parser(
        "play",
        help="Request play_media for an http(s) URL accessible to HA / the player",
    )
    add_connection_arguments(play)
    play.add_argument("--url", required=True, help="http(s) media URL")
    play.add_argument(
        "--volume",
        type=float,
        help=(
            "Optional volume_level before play "
            "(overrides HOMEASSISTANT_VOLUME default)"
        ),
    )
    play.add_argument(
        "--media-content-type",
        default="music",
        help="HA media_content_type (default: music)",
    )
    play.add_argument(
        "--verify",
        action="store_true",
        help=(
            "After HA accepts play_media, poll player state up to 3 times "
            "(1s apart) for playing + matching media_content_id. "
            "Does not retry play_media; does not claim acoustic playback."
        ),
    )
    play.set_defaults(func=cmd_audio_play)

    pause = audio_cmds.add_parser(
        "pause",
        help=(
            "Request media_pause (intended to preserve playback position; "
            "audible pause not confirmed)"
        ),
    )
    add_connection_arguments(pause)
    pause.set_defaults(func=cmd_audio_pause)

    resume = audio_cmds.add_parser(
        "resume",
        help=(
            "Request media_play to resume (intended to preserve playback position; "
            "audible playback not confirmed)"
        ),
    )
    add_connection_arguments(resume)
    resume.set_defaults(func=cmd_audio_resume)

    stop = audio_cmds.add_parser(
        "stop",
        help=(
            "Request media_stop (may lose playback position; unlike pause/resume). "
            "Stop completion is not confirmed."
        ),
    )
    add_connection_arguments(stop)
    stop.set_defaults(func=cmd_audio_stop)

    scenario = audio_cmds.add_parser(
        "scenario",
        help="Validate and sequentially run versioned JSON audio scenarios",
    )
    scenario_cmds = scenario.add_subparsers(dest="scenario_command", required=True)

    scenario_list = scenario_cmds.add_parser(
        "list", help=f"List scenario files under {DEFAULT_SCENARIO_DIR}"
    )
    scenario_list.set_defaults(func=cmd_scenario_list)

    scenario_validate = scenario_cmds.add_parser(
        "validate", help="Validate a scenario JSON file against schema_version 1"
    )
    scenario_validate.add_argument("scenario_file")
    scenario_validate.set_defaults(func=cmd_scenario_validate)

    scenario_run = scenario_cmds.add_parser(
        "run",
        help=(
            "Validate then execute steps sequentially (fail-stop). "
            "Use --dry-run to print steps without HA calls."
        ),
    )
    add_connection_arguments(scenario_run)
    scenario_run.add_argument("scenario_file")
    scenario_run.add_argument(
        "--dry-run",
        action="store_true",
        help=(
            "Validate, resolve media sources, and display the execution plan "
            "without Home Assistant calls or waits"
        ),
    )
    scenario_run.add_argument(
        "--no-restore",
        action="store_true",
        help=(
            "Do not restore the previous media-player volume after the run. "
            "Required to proceed when the initial volume cannot be read and the "
            "scenario changes volume."
        ),
    )
    scenario_run.set_defaults(func=cmd_scenario_run)

    return parser


def main() -> int:
    try:
        load_project_env()
    except ValueError as exc:
        print(f"configuration failed: {exc}", file=sys.stderr)
        return 1

    parser = build_parser()
    if print_help_without_arguments(parser):
        return 0
    args = parser.parse_args()
    token = None
    try:
        # Best-effort token for redaction when config loaded inside command.
        from os import environ

        token = environ.get("HOMEASSISTANT_TOKEN")
        return int(args.func(args))
    except (HomeAssistantError, AudioScenarioError, OSError, ValueError) as exc:
        message = redact_secrets(str(exc), token)
        print(f"homeassistant: {message}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
