from datetime import datetime, timezone

from sqlalchemy import func, select
from sqlalchemy.orm import Session

from app.models import Group, GroupMembership, User
from app.schemas import GroupMemberView, GroupView


class GroupNotFound(Exception):
    pass


class InvalidGroupMember(Exception):
    pass


class LastGroupOwner(Exception):
    pass


class GroupCreationLimit(Exception):
    pass


def list_groups(db: Session, user_id: str | None = None) -> list[GroupView]:
    statement = select(Group).order_by(Group.name, Group.group_id)
    if user_id is not None:
        member_group_ids = select(GroupMembership.group_id).where(
            GroupMembership.user_id == user_id,
            GroupMembership.valid_to.is_(None),
        )
        statement = statement.where(Group.group_id.in_(member_group_ids))
    return [_group_view(db, group) for group in db.scalars(statement)]


def get_group(db: Session, group_id: str) -> GroupView:
    group = db.get(Group, group_id)
    if group is None:
        raise GroupNotFound
    return _group_view(db, group)


def create_group(
    db: Session,
    name: str,
    owner_user_id: str,
    creation_limit: int | None = None,
) -> GroupView:
    owner = db.get(User, owner_user_id)
    if owner is None or owner.status != "active":
        raise InvalidGroupMember
    if creation_limit is not None:
        owned_count = db.scalar(
            select(func.count()).select_from(Group).where(Group.created_by_user_id == owner_user_id)
        ) or 0
        if owned_count >= creation_limit:
            raise GroupCreationLimit
    group = Group(name=name.strip(), created_by_user_id=owner_user_id)
    db.add(group)
    db.flush()
    db.add(GroupMembership(group_id=group.group_id, user_id=owner_user_id, role="owner"))
    db.commit()
    return _group_view(db, group)


def set_member_role(db: Session, group_id: str, user_id: str, role: str) -> GroupView:
    group = db.get(Group, group_id)
    user = db.get(User, user_id)
    if group is None:
        raise GroupNotFound
    if user is None or user.status != "active":
        raise InvalidGroupMember
    current = _current_membership(db, group_id, user_id)
    if current is not None and current.role == role:
        return _group_view(db, group)
    if current is not None:
        _ensure_not_last_owner(db, group_id, current)
        current.valid_to = datetime.now(timezone.utc)
        db.flush()
    db.add(GroupMembership(group_id=group_id, user_id=user_id, role=role))
    db.commit()
    return _group_view(db, group)


def remove_member(db: Session, group_id: str, user_id: str) -> GroupView:
    group = db.get(Group, group_id)
    if group is None:
        raise GroupNotFound
    current = _current_membership(db, group_id, user_id)
    if current is None:
        return _group_view(db, group)
    _ensure_not_last_owner(db, group_id, current)
    current.valid_to = datetime.now(timezone.utc)
    db.commit()
    return _group_view(db, group)


def set_group_status(db: Session, group_id: str, status: str) -> GroupView:
    group = db.get(Group, group_id)
    if group is None:
        raise GroupNotFound
    group.status = status
    db.commit()
    return _group_view(db, group)


def get_member_role(db: Session, group_id: str, user_id: str) -> str | None:
    membership = _current_membership(db, group_id, user_id)
    return membership.role if membership else None


def _current_membership(db: Session, group_id: str, user_id: str) -> GroupMembership | None:
    return db.scalar(
        select(GroupMembership).where(
            GroupMembership.group_id == group_id,
            GroupMembership.user_id == user_id,
            GroupMembership.valid_to.is_(None),
        )
    )


def _ensure_not_last_owner(
    db: Session,
    group_id: str,
    membership: GroupMembership,
) -> None:
    if membership.role != "owner":
        return
    owner_count = db.scalar(
        select(func.count())
        .select_from(GroupMembership)
        .where(
            GroupMembership.group_id == group_id,
            GroupMembership.role == "owner",
            GroupMembership.valid_to.is_(None),
        )
    ) or 0
    if owner_count <= 1:
        raise LastGroupOwner


def _group_view(db: Session, group: Group) -> GroupView:
    memberships = db.scalars(
        select(GroupMembership)
        .where(GroupMembership.group_id == group.group_id, GroupMembership.valid_to.is_(None))
        .order_by(GroupMembership.role, GroupMembership.user_id)
    ).all()
    members = []
    for membership in memberships:
        user = db.get(User, membership.user_id)
        if user is not None:
            members.append(
                GroupMemberView(
                    user_id=user.user_id,
                    display_name=user.display_name,
                    role=membership.role,
                )
            )
    return GroupView(
        group_id=group.group_id,
        name=group.name,
        status=group.status,
        created_by_user_id=group.created_by_user_id,
        created_at=_utc(group.created_at),
        updated_at=_utc(group.updated_at),
        members=members,
    )


def _utc(value: datetime) -> datetime:
    return value if value.tzinfo is not None else value.replace(tzinfo=timezone.utc)
