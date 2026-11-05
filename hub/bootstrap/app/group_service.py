from datetime import datetime, timezone

from sqlalchemy import func, select
from sqlalchemy.orm import Session

from app.models import Group, GroupMembership, User
from app.pagination import (
    DEFAULT_PAGE_SIZE,
    MAX_PAGE_SIZE,
    SQLITE_ROWID,
    ListOrder,
    PageResult,
    compute_total_pages,
    count_rows,
    max_sqlite_rowid,
    page_offset,
    resolve_snapshot_id,
    validate_page_params,
)
from app.schemas import GroupMemberView, GroupView


class GroupNotFound(Exception):
    pass


class InvalidGroupMember(Exception):
    pass


class LastGroupOwner(Exception):
    pass


class GroupCreationLimit(Exception):
    pass


def _member_filter(user_id: str | None):
    if user_id is None:
        return None
    return Group.group_id.in_(
        select(GroupMembership.group_id).where(
            GroupMembership.user_id == user_id,
            GroupMembership.valid_to.is_(None),
        )
    )


def list_groups_page(
    db: Session,
    user_id: str | None = None,
    *,
    page: int = 1,
    page_size: int = DEFAULT_PAGE_SIZE,
    order: ListOrder = "newest",
    snapshot_id: int | None = None,
) -> PageResult[GroupView]:
    """
    Return one page of groups.

    Ordering is ``created_at`` with ``group_id`` as tie-breaker. Snapshot freezes
    on SQLite ``rowid`` because ``group_id`` is a UUID and is not monotonic.
    """
    order = validate_page_params(page, page_size, order)
    base_filters = []
    member_clause = _member_filter(user_id)
    if member_clause is not None:
        base_filters.append(member_clause)

    current_max = max_sqlite_rowid(db, Group, base_filters)
    effective_snapshot = resolve_snapshot_id(snapshot_id, current_max)
    filters = list(base_filters)
    if effective_snapshot is not None:
        filters.append(SQLITE_ROWID <= effective_snapshot)

    total = count_rows(db, Group, filters)
    statement = select(Group)
    for clause in filters:
        statement = statement.where(clause)
    if order == "newest":
        statement = statement.order_by(Group.created_at.desc(), Group.group_id.desc())
    else:
        statement = statement.order_by(Group.created_at.asc(), Group.group_id.asc())
    rows = db.scalars(
        statement.offset(page_offset(page, page_size)).limit(page_size)
    ).all()
    return PageResult(
        items=[_group_view(db, group) for group in rows],
        total=total,
        page=page,
        page_size=page_size,
        total_pages=compute_total_pages(total, page_size),
        order=order,
        snapshot_id=effective_snapshot,
    )


def list_groups(db: Session, user_id: str | None = None) -> list[GroupView]:
    """Backward-compatible helper: first page capped at the maximum page size."""
    return list_groups_page(
        db, user_id, page=1, page_size=MAX_PAGE_SIZE
    ).items


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
        owned_count = (
            db.scalar(
                select(func.count())
                .select_from(Group)
                .where(Group.created_by_user_id == owner_user_id)
            )
            or 0
        )
        if owned_count >= creation_limit:
            raise GroupCreationLimit
    group = Group(name=name.strip(), created_by_user_id=owner_user_id)
    db.add(group)
    db.flush()
    db.add(
        GroupMembership(group_id=group.group_id, user_id=owner_user_id, role="owner")
    )
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


def _current_membership(
    db: Session, group_id: str, user_id: str
) -> GroupMembership | None:
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
    owner_count = (
        db.scalar(
            select(func.count())
            .select_from(GroupMembership)
            .where(
                GroupMembership.group_id == group_id,
                GroupMembership.role == "owner",
                GroupMembership.valid_to.is_(None),
            )
        )
        or 0
    )
    if owner_count <= 1:
        raise LastGroupOwner


def _group_view(db: Session, group: Group) -> GroupView:
    memberships = db.scalars(
        select(GroupMembership)
        .where(
            GroupMembership.group_id == group.group_id,
            GroupMembership.valid_to.is_(None),
        )
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
