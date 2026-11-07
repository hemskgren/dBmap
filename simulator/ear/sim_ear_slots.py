"""Shared simulated Ear slot definitions (a–g).

Slot letters are local credential/file aliases. Hub assigns ``SIM-EAR-*`` node
IDs at registration time. The expected mapping below is the conventional
dev-layout target (A→001 … G→007); always verify the printed node id after
``add`` before calling ``set-installation``.
"""

from __future__ import annotations

from typing import Any

EAR_SLOTS: tuple[str, ...] = ("a", "b", "c", "d", "e", "f", "g")
DEFAULT_EAR = "a"

# Conventional target mapping for the shared SIM installation estate.
# Hub still allocates the next free SIM-EAR-* id; callers must verify.
EXPECTED_NODE_ID_BY_SLOT: dict[str, str] = {
    "a": "SIM-EAR-001",
    "b": "SIM-EAR-002",
    "c": "SIM-EAR-003",
    "d": "SIM-EAR-004",
    "e": "SIM-EAR-005",
    "f": "SIM-EAR-006",
    "g": "SIM-EAR-007",
}

# Authoritative proposed Hub installation table for this estate.
# A–C match the documented set-installation values (not helicopter_east_west.json).
# D–G are simulated placement proposals — easy to edit after map review.
# Scenario JSON must match Hub installs when a scenario uses these devices.
INSTALLATION_PROPOSALS: dict[str, dict[str, Any]] = {
    "a": {
        "node_id": "SIM-EAR-001",
        "latitude": 59.35100847144696,
        "longitude": 18.04842390753845,
        "height_m": 74.0,
        "orientation_deg": 0.0,
        "note": (
            "Documented Hub install for slot a. "
            "helicopter_east_west.json ear_a differs by ~20 m "
            "(59.3511652, 18.0487074) — do not silently merge."
        ),
    },
    "b": {
        "node_id": "SIM-EAR-002",
        "latitude": 59.335118371024905,
        "longitude": 18.12680373585177,
        "height_m": 155.0,
        "orientation_deg": 270.0,
        "note": "Matches helicopter_east_west.json ear_b.",
    },
    "c": {
        "node_id": "SIM-EAR-003",
        "latitude": 59.32765051899676,
        "longitude": 18.05362024876985,
        "height_m": 72.0,
        "orientation_deg": 0.0,
        "note": "Matches helicopter_east_west.json ear_c.",
    },
    "d": {
        "node_id": "SIM-EAR-004",
        "latitude": 59.3655000,
        "longitude": 18.0900000,
        "height_m": 95.0,
        "orientation_deg": 180.0,
        "note": "Simulated proposal — north of E–W corridor; adjust after map review.",
    },
    "e": {
        "node_id": "SIM-EAR-005",
        "latitude": 59.3180000,
        "longitude": 18.1000000,
        "height_m": 88.0,
        "orientation_deg": 0.0,
        "note": "Simulated proposal — south-east of corridor; adjust after map review.",
    },
    "f": {
        "node_id": "SIM-EAR-006",
        "latitude": 59.3550000,
        "longitude": 17.9950000,
        "height_m": 80.0,
        "orientation_deg": 135.0,
        "note": "Simulated proposal — north-west; adjust after map review.",
    },
    "g": {
        "node_id": "SIM-EAR-007",
        "latitude": 59.3120000,
        "longitude": 17.9800000,
        "height_m": 110.0,
        "orientation_deg": 45.0,
        "note": "Simulated proposal — south-west outlier; adjust after map review.",
    },
}


class SimEarSlotError(ValueError):
    """Invalid simulated Ear slot or unexpected Hub node id for a slot."""


def validate_ear_slot(ear: str) -> str:
    if ear not in EAR_SLOTS:
        known = ", ".join(EAR_SLOTS)
        raise SimEarSlotError(f"unsupported sim Ear slot {ear!r}; expected one of: {known}")
    return ear


def credentials_file_for_ear(ear: str) -> str:
    validate_ear_slot(ear)
    return f"shared/simulator/sim-ear-{ear}.json"


def expected_node_id_for_slot(ear: str) -> str:
    validate_ear_slot(ear)
    return EXPECTED_NODE_ID_BY_SLOT[ear]


def installation_proposal_for_slot(ear: str) -> dict[str, Any]:
    validate_ear_slot(ear)
    return dict(INSTALLATION_PROPOSALS[ear])


def assert_node_id_matches_slot(ear: str, node_id: str) -> None:
    """Refuse to bind a slot credentials file to an unexpected SIM-EAR id."""
    expected = expected_node_id_for_slot(ear)
    if node_id != expected:
        raise SimEarSlotError(
            f"slot {ear!r} expects {expected}, but Hub assigned {node_id!r}. "
            "Do not overwrite or relocate another device. "
            "Inspect Hub inventory (`hub_admin.py list` / `show`), free or "
            "align the expected id, then retry this slot. "
            "If a new node was created under the unexpected id, resolve it "
            "explicitly in Hub before retrying."
        )


def slot_help_text(*, include_all: bool = False) -> str:
    letters = ",".join(EAR_SLOTS)
    base = f"Sim Ear slot → shared/simulator/sim-ear-{{{letters}}}.json (default: {DEFAULT_EAR}"
    if include_all:
        return base + "; all = every slot that has a credentials file)"
    return base + ")"
