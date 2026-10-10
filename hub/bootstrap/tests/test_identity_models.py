from uuid import UUID

import pytest
from app.models import Base, ExternalIdentity, Node, User
from sqlalchemy import create_engine
from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session


def test_users_have_independent_stable_ids_and_allow_multiple_identities() -> None:
    engine = create_engine("sqlite://")
    Base.metadata.create_all(bind=engine)

    with Session(engine) as db:
        first = User(display_name="Alex")
        second = User(display_name="Alex")
        db.add_all([first, second])
        db.commit()

        assert first.user_id != second.user_id
        UUID(first.user_id)
        UUID(second.user_id)

        github_identity = ExternalIdentity(
            user_id=first.user_id,
            provider="github",
            provider_subject="12345678",
        )
        second_identity = ExternalIdentity(
            user_id=first.user_id,
            provider="oidc-example",
            provider_subject="subject-1",
        )
        db.add_all([github_identity, second_identity])
        db.commit()

        first.display_name = "Alex Example"
        db.commit()
        assert db.get(User, first.user_id).display_name == "Alex Example"
        assert len(db.get(User, first.user_id).external_identities) == 2


def test_provider_subject_is_unique_and_disabled_identity_is_retained() -> None:
    engine = create_engine("sqlite://")
    Base.metadata.create_all(bind=engine)

    with Session(engine) as db:
        user = User(display_name="Alex")
        other_user = User(display_name="Alex")
        db.add_all([user, other_user])
        db.commit()

        identity = ExternalIdentity(
            user_id=user.user_id,
            provider="github",
            provider_subject="12345678",
        )
        db.add(identity)
        db.commit()

        duplicate = ExternalIdentity(
            user_id=other_user.user_id,
            provider="github",
            provider_subject="12345678",
        )
        db.add(duplicate)
        with pytest.raises(IntegrityError):
            db.commit()
        db.rollback()

        identity.status = "disabled"
        db.commit()
        assert db.get(ExternalIdentity, identity.identity_id).status == "disabled"
        assert db.get(User, user.user_id) is not None


def test_identity_tables_are_additive_to_existing_device_data() -> None:
    engine = create_engine("sqlite://")
    Node.__table__.create(bind=engine)

    with Session(engine) as db:
        db.add(Node(node_id="EAR-007", node_type="ear"))
        db.commit()

    Base.metadata.create_all(bind=engine)

    with Session(engine) as db:
        assert db.get(Node, "EAR-007").node_id == "EAR-007"
        assert db.get(Node, "EAR-007").node_type == "ear"
