from datetime import datetime, timezone

from sqlalchemy import select
from sqlalchemy.orm import Session

from app.models import DeviceOwnership, Group, Node, User
from app.schemas import DeviceOwnershipView, OwnershipAssignmentView


class OwnershipTargetNotFound(Exception):
    pass


class InactiveOwner(Exception):
    pass


def get_ownership(db: Session, node_id: str) -> DeviceOwnershipView:
    normalized_id = node_id.upper()
    if db.get(Node, normalized_id) is None:
        raise OwnershipTargetNotFound

    rows = db.scalars(
        select(DeviceOwnership)
        .where(DeviceOwnership.node_id == normalized_id)
        .order_by(DeviceOwnership.valid_from, DeviceOwnership.assignment_id)
    ).all()
    assignments = []
    for row in rows:
        owner = db.get(User if row.owner_type == "user" else Group, row.owner_user_id or row.owner_group_id)
        if owner is None:
            # The FK should prevent this; fail closed if a legacy database is inconsistent.
            raise OwnershipTargetNotFound
        assignments.append(
            OwnershipAssignmentView(
                assignment_id=row.assignment_id,
                owner_type=row.owner_type,
                owner_id=owner.user_id if row.owner_type == "user" else owner.group_id,
                owner_display_name=owner.display_name if row.owner_type == "user" else owner.name,
                valid_from=_utc(row.valid_from),
                valid_to=_utc(row.valid_to) if row.valid_to else None,
            )
        )
    current = next((item for item in reversed(assignments) if item.valid_to is None), None)
    return DeviceOwnershipView(
        node_id=normalized_id,
        current_owner_type=current.owner_type if current else None,
        current_owner_id=current.owner_id if current else None,
        current_owner_display_name=current.owner_display_name if current else None,
        assignments=assignments,
    )


def get_owner_context(db: Session, node_id: str) -> dict | None:
    assignment = db.scalar(
        select(DeviceOwnership).where(
            DeviceOwnership.node_id == node_id.upper(),
            DeviceOwnership.valid_to.is_(None),
        )
    )
    if assignment is None:
        return None
    owner_type = assignment.owner_type
    owner_id = assignment.owner_user_id if owner_type == "user" else assignment.owner_group_id
    owner = db.get(User if owner_type == "user" else Group, owner_id)
    if owner is None:
        return None
    return {
        "owner_type": owner_type,
        "owner_id": owner_id,
        "owner_status": owner.status,
        "owner_display_name": owner.display_name if owner_type == "user" else owner.name,
    }


def get_current_owner(db: Session, node_id: str) -> tuple[str, str, str] | None:
    owner = get_owner_context(db, node_id)
    if owner is None:
        return None
    return owner["owner_type"], owner["owner_id"], owner["owner_display_name"]


def set_owner(
    db: Session,
    node_id: str,
    owner_type: str | None,
    owner_id: str | None,
) -> DeviceOwnershipView:
    normalized_id = node_id.upper()
    node = db.get(Node, normalized_id)
    if node is None:
        raise OwnershipTargetNotFound

    current = db.scalar(
        select(DeviceOwnership).where(
            DeviceOwnership.node_id == normalized_id,
            DeviceOwnership.valid_to.is_(None),
        )
    )
    if owner_type is not None and owner_id is not None:
        owner = db.get(User if owner_type == "user" else Group, owner_id)
        if owner is None:
            raise OwnershipTargetNotFound
        current_owner_id = current.owner_user_id if current and current.owner_type == "user" else (
            current.owner_group_id if current else None
        )
        if current is not None and current.owner_type == owner_type and current_owner_id == owner_id:
            return get_ownership(db, normalized_id)
        if owner.status != "active":
            raise InactiveOwner

    now = datetime.now(timezone.utc)
    if current is not None:
        current.valid_to = now
        db.flush()
    if owner_type is not None and owner_id is not None:
        db.add(
            DeviceOwnership(
                node_id=normalized_id,
                owner_type=owner_type,
                owner_user_id=owner_id if owner_type == "user" else None,
                owner_group_id=owner_id if owner_type == "group" else None,
                valid_from=now,
            )
        )
    db.commit()
    return get_ownership(db, normalized_id)


def add_initial_owner(
    db: Session,
    node_id: str,
    owner_type: str | None,
    owner_id: str | None,
) -> None:
    if owner_type is None and owner_id is None:
        return
    if owner_type is None or owner_id is None:
        raise ValueError("owner type and owner ID must be provided together")

    owner = db.get(User if owner_type == "user" else Group, owner_id)
    if owner is None:
        raise OwnershipTargetNotFound
    if owner.status != "active":
        raise InactiveOwner

    db.add(
        DeviceOwnership(
            node_id=node_id,
            owner_type=owner_type,
            owner_user_id=owner_id if owner_type == "user" else None,
            owner_group_id=owner_id if owner_type == "group" else None,
            valid_from=datetime.now(timezone.utc),
        )
    )


def _utc(value: datetime) -> datetime:
    return value if value.tzinfo is not None else value.replace(tzinfo=timezone.utc)
