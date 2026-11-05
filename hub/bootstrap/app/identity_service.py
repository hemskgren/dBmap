from sqlalchemy import select
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.models import ExternalIdentity, User
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
from app.schemas import UserIdentityView, UserView


class UserNotFound(Exception):
    pass


class IdentityConflict(Exception):
    pass


def _user_view(user: User) -> UserView:
    identities = sorted(
        user.external_identities, key=lambda item: (item.provider, item.identity_id)
    )
    return UserView(
        user_id=user.user_id,
        display_name=user.display_name,
        status=user.status,
        created_at=user.created_at,
        updated_at=user.updated_at,
        external_identities=[
            UserIdentityView(
                identity_id=identity.identity_id,
                provider=identity.provider,
                provider_subject=identity.provider_subject,
                status=identity.status,
                created_at=identity.created_at,
                last_authenticated_at=identity.last_authenticated_at,
            )
            for identity in identities
        ],
    )


def list_users_page(
    db: Session,
    *,
    page: int = 1,
    page_size: int = DEFAULT_PAGE_SIZE,
    order: ListOrder = "newest",
    snapshot_id: int | None = None,
) -> PageResult[UserView]:
    """
    Return one page of users.

    Ordering is ``created_at`` with ``user_id`` as tie-breaker. Snapshot freezes
    on SQLite ``rowid`` because ``user_id`` is a UUID and is not monotonic.
    """
    order = validate_page_params(page, page_size, order)
    current_max = max_sqlite_rowid(db, User)
    effective_snapshot = resolve_snapshot_id(snapshot_id, current_max)
    filters = []
    if effective_snapshot is not None:
        filters.append(SQLITE_ROWID <= effective_snapshot)

    total = count_rows(db, User, filters)
    statement = select(User)
    for clause in filters:
        statement = statement.where(clause)
    if order == "newest":
        statement = statement.order_by(User.created_at.desc(), User.user_id.desc())
    else:
        statement = statement.order_by(User.created_at.asc(), User.user_id.asc())
    rows = db.scalars(
        statement.offset(page_offset(page, page_size)).limit(page_size)
    ).all()
    return PageResult(
        items=[_user_view(user) for user in rows],
        total=total,
        page=page,
        page_size=page_size,
        total_pages=compute_total_pages(total, page_size),
        order=order,
        snapshot_id=effective_snapshot,
    )


def list_users(db: Session) -> list[UserView]:
    """Backward-compatible helper: first page capped at the maximum page size."""
    return list_users_page(db, page=1, page_size=MAX_PAGE_SIZE).items


def get_user(db: Session, user_id: str) -> UserView:
    user = db.get(User, user_id)
    if user is None:
        raise UserNotFound
    return _user_view(user)


def create_user(db: Session, display_name: str) -> UserView:
    user = User(display_name=display_name.strip())
    db.add(user)
    db.commit()
    return _user_view(user)


def set_user_status(db: Session, user_id: str, status: str) -> UserView:
    user = db.get(User, user_id)
    if user is None:
        raise UserNotFound
    user.status = status
    db.commit()
    return _user_view(user)


def link_github_identity(db: Session, user_id: str, provider_subject: str) -> UserView:
    user = db.get(User, user_id)
    if user is None:
        raise UserNotFound

    existing = db.scalar(
        select(ExternalIdentity).where(
            ExternalIdentity.provider == "github",
            ExternalIdentity.provider_subject == provider_subject,
        )
    )
    if existing is not None:
        if existing.user_id == user_id and existing.status == "active":
            return _user_view(user)
        raise IdentityConflict

    db.add(
        ExternalIdentity(
            user_id=user_id,
            provider="github",
            provider_subject=provider_subject,
        )
    )
    try:
        db.commit()
    except IntegrityError as exc:
        db.rollback()
        raise IdentityConflict from exc
    return _user_view(user)


def set_identity_status(
    db: Session, user_id: str, identity_id: str, status: str
) -> UserView:
    user = db.get(User, user_id)
    if user is None:
        raise UserNotFound
    identity = db.get(ExternalIdentity, identity_id)
    if identity is None or identity.user_id != user_id:
        raise UserNotFound
    identity.status = status
    db.commit()
    return _user_view(user)
