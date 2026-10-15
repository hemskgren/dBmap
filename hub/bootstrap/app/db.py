from collections.abc import Generator

from sqlalchemy import create_engine, inspect, text
from sqlalchemy.orm import Session, sessionmaker

from app.config import settings
from app.models import Base

connect_args = {}
if settings.database_url.startswith("sqlite"):
    connect_args = {"check_same_thread": False}

engine = create_engine(settings.database_url, connect_args=connect_args)
SessionLocal = sessionmaker(bind=engine, autoflush=False, autocommit=False)


def init_db() -> None:
    Base.metadata.create_all(bind=engine)
    _migrate_bootstrap_token_active_index()
    _migrate_device_ownership_to_user_or_group()
    _migrate_desired_state_firmware_id()
    node_columns = {column["name"] for column in inspect(engine).get_columns("nodes")}
    if "lifecycle_state" not in node_columns:
        with engine.begin() as connection:
            connection.execute(
                text(
                    "ALTER TABLE nodes ADD COLUMN lifecycle_state "
                    "VARCHAR(16) NOT NULL DEFAULT 'active'"
                )
            )
    columns = {column["name"] for column in inspect(engine).get_columns("node_credentials")}
    if "mqtt_password_plain_once" in columns:
        with engine.begin() as connection:
            connection.execute(text("ALTER TABLE node_credentials DROP COLUMN mqtt_password_plain_once"))


def _migrate_desired_state_firmware_id() -> None:
    columns = {column["name"] for column in inspect(engine).get_columns("desired_state")}
    if "firmware_id" not in columns:
        with engine.begin() as connection:
            connection.execute(
                text("ALTER TABLE desired_state ADD COLUMN firmware_id VARCHAR(64) NOT NULL DEFAULT ''")
            )


def _migrate_bootstrap_token_active_index() -> None:
    with engine.begin() as connection:
        connection.exec_driver_sql(
            "CREATE UNIQUE INDEX IF NOT EXISTS uq_bootstrap_tokens_one_unconsumed_per_node "
            "ON bootstrap_tokens (node_id) WHERE consumed_at IS NULL"
        )


def _migrate_device_ownership_to_user_or_group() -> None:
    columns = {column["name"] for column in inspect(engine).get_columns("device_ownership")}
    if "owner_type" in columns:
        return
    with engine.begin() as connection:
        connection.exec_driver_sql(
            "ALTER TABLE device_ownership RENAME TO device_ownership_pre_groups"
        )
        connection.exec_driver_sql(
            """
            CREATE TABLE device_ownership (
                assignment_id VARCHAR(36) NOT NULL PRIMARY KEY,
                node_id VARCHAR(64) NOT NULL REFERENCES nodes(node_id) ON DELETE RESTRICT,
                owner_type VARCHAR(8) NOT NULL CHECK (owner_type IN ('user', 'group')),
                owner_user_id VARCHAR(36) REFERENCES users(user_id) ON DELETE RESTRICT,
                owner_group_id VARCHAR(36) REFERENCES groups(group_id) ON DELETE RESTRICT,
                valid_from DATETIME NOT NULL,
                valid_to DATETIME,
                CHECK (
                    (owner_type = 'user' AND owner_user_id IS NOT NULL AND owner_group_id IS NULL)
                    OR
                    (owner_type = 'group' AND owner_group_id IS NOT NULL AND owner_user_id IS NULL)
                ),
                CHECK (valid_to IS NULL OR valid_to >= valid_from)
            )
            """
        )
        connection.exec_driver_sql(
            """
            INSERT INTO device_ownership
                (assignment_id, node_id, owner_type, owner_user_id, owner_group_id, valid_from, valid_to)
            SELECT assignment_id, node_id, 'user', owner_user_id, NULL, valid_from, valid_to
            FROM device_ownership_pre_groups
            """
        )
        connection.exec_driver_sql("DROP TABLE device_ownership_pre_groups")
        connection.exec_driver_sql(
            "CREATE UNIQUE INDEX uq_device_ownership_current "
            "ON device_ownership(node_id) WHERE valid_to IS NULL"
        )
        connection.exec_driver_sql(
            "CREATE INDEX ix_device_ownership_node_id ON device_ownership(node_id)"
        )
        connection.exec_driver_sql(
            "CREATE INDEX ix_device_ownership_owner_user_id ON device_ownership(owner_user_id)"
        )
        connection.exec_driver_sql(
            "CREATE INDEX ix_device_ownership_owner_group_id ON device_ownership(owner_group_id)"
        )


def get_db() -> Generator[Session, None, None]:
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()
