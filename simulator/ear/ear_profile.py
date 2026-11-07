"""Load reusable EAR selection profiles (alias → slot → credentials path).

Profiles select installed simulator devices only. They do not define source
motion, detection windows, cadence, association, or localization.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

from sim_ear_slots import credentials_file_for_ear, validate_ear_slot

_PROFILE_FIELDS = frozenset({"name", "ears"})
_ALIAS_PATTERN = re.compile(r"[A-Za-z0-9_-]{1,64}")


class EarProfileError(ValueError):
    """Invalid EAR profile content or resolution failure."""


@dataclass(frozen=True)
class EarProfile:
    name: str
    # Deterministic alias→slot order matching JSON object insertion order.
    ears: dict[str, str]
    path: Path

    @property
    def slots(self) -> tuple[str, ...]:
        """Unique slots in first-seen profile order."""
        seen: list[str] = []
        for slot in self.ears.values():
            if slot not in seen:
                seen.append(slot)
        return tuple(seen)


def _require_alias(alias: str, *, context: str) -> str:
    if not isinstance(alias, str) or not alias:
        raise EarProfileError(f"{context}: Ear alias must be a non-empty string")
    if not _ALIAS_PATTERN.fullmatch(alias):
        raise EarProfileError(f"{context}: invalid Ear alias {alias!r}")
    return alias


def load_ear_profile(path: Path | str) -> EarProfile:
    profile_path = Path(path)
    try:
        raw = json.loads(profile_path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise EarProfileError(f"EAR profile not found: {profile_path}") from exc
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise EarProfileError(
            f"could not read EAR profile {profile_path}: {exc}"
        ) from exc

    if not isinstance(raw, dict):
        raise EarProfileError(f"EAR profile {profile_path} must be a JSON object")
    unexpected = set(raw) - _PROFILE_FIELDS
    missing = _PROFILE_FIELDS - set(raw)
    if unexpected or missing:
        raise EarProfileError(
            f"EAR profile {profile_path} fields mismatch; "
            f"missing={sorted(missing)}, unexpected={sorted(unexpected)}"
        )

    name = raw["name"]
    if not isinstance(name, str) or not name.strip():
        raise EarProfileError(
            f"EAR profile {profile_path}: name must be a non-empty string"
        )

    ears_raw = raw["ears"]
    if not isinstance(ears_raw, dict) or not ears_raw:
        raise EarProfileError(
            f"EAR profile {profile_path}: ears must be a non-empty object"
        )

    ears: dict[str, str] = {}
    slots_seen: set[str] = set()
    for alias_key, slot_value in ears_raw.items():
        alias = _require_alias(str(alias_key), context=str(profile_path))
        if alias in ears:
            raise EarProfileError(
                f"EAR profile {profile_path}: duplicate Ear alias {alias!r}"
            )
        if not isinstance(slot_value, str):
            raise EarProfileError(
                f"EAR profile {profile_path}: ears.{alias} must be a slot letter string"
            )
        try:
            slot = validate_ear_slot(slot_value)
        except ValueError as exc:
            raise EarProfileError(
                f"EAR profile {profile_path}: ears.{alias}: {exc}"
            ) from exc
        if slot in slots_seen:
            raise EarProfileError(
                f"EAR profile {profile_path}: duplicate slot {slot!r} "
                f"(alias {alias!r})"
            )
        slots_seen.add(slot)
        ears[alias] = slot

    return EarProfile(name=name.strip(), ears=ears, path=profile_path)


def resolve_profile_credential_paths(
    profile: EarProfile,
    *,
    require_files: bool = True,
) -> dict[str, Path]:
    """Map scenario aliases to credential Paths via ``sim_ear_slots``.

    Order follows the profile's alias declaration order.
    """
    resolved: dict[str, Path] = {}
    missing: list[str] = []
    for alias, slot in profile.ears.items():
        path = Path(credentials_file_for_ear(slot))
        if require_files and not path.is_file():
            missing.append(f"{alias}→slot {slot} ({path})")
        resolved[alias] = path
    if missing:
        raise EarProfileError(
            "EAR profile credential file(s) missing: " + "; ".join(missing)
        )
    return resolved


def resolve_profile_keepalive_paths(
    profile: EarProfile,
    *,
    require_files: bool = True,
) -> list[str]:
    """Credential paths for keepalive: unique slots in profile order."""
    paths: list[str] = []
    missing: list[str] = []
    for slot in profile.slots:
        path = credentials_file_for_ear(slot)
        if require_files and not Path(path).is_file():
            missing.append(f"slot {slot} ({path})")
        paths.append(path)
    if missing:
        raise EarProfileError(
            "EAR profile credential file(s) missing: " + "; ".join(missing)
        )
    return paths


def profile_summary(profile: EarProfile) -> str:
    pairs = ", ".join(f"{alias}={slot}" for alias, slot in profile.ears.items())
    return f"{profile.name} ({pairs})"
