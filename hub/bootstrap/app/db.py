import logging
from collections.abc import Generator
from datetime import UTC, datetime

from sqlalchemy import create_engine, inspect, text
from sqlalchemy.engine import Connection, Engine
from sqlalchemy.orm import Session, sessionmaker

from app.config import settings
from app.models import Base

log = logging.getLogger("hub-bootstrap.db")

LEGACY_MONITOR_CHECKPOINT_TABLE = "hub_monitor_checkpoints"
LEGACY_MONITOR_TABLES = (
    "hub_monitor_samples",
    "hub_monitor_events",
    LEGACY_MONITOR_CHECKPOINT_TABLE,
    "public_monitor_samples",
    "public_monitor_events",
    "public_monitor_checkpoints",
    "public_monitor_transitions",
)

connect_args = {}
if settings.database_url.startswith("sqlite"):
    connect_args = {"check_same_thread": False}

engine = create_engine(settings.database_url, connect_args=connect_args)
SessionLocal = sessionmaker(bind=engine, autoflush=False, autocommit=False)
_legacy_monitor_cleanup_pending = False


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
    columns = {
        column["name"] for column in inspect(engine).get_columns("node_credentials")
    }
    if "mqtt_password_plain_once" in columns:
        with engine.begin() as connection:
            connection.execute(
                text(
                    "ALTER TABLE node_credentials DROP COLUMN mqtt_password_plain_once"
                )
            )
    _migrate_device_https_credentials()
    _migrate_device_diag_pending_ota()
    _migrate_node_chip_identity()
    _migrate_observations_report_identity()
    cleanup_legacy_monitor_tables(force=True)


def _valid_monitor_checkpoint(value: object, now: datetime) -> bool:
    if not isinstance(value, str):
        return False
    try:
        checkpoint_at = datetime.fromisoformat(value)
    except (ValueError, OverflowError):
        return False
    return checkpoint_at.utcoffset() is not None and checkpoint_at <= now


def _migrate_legacy_monitor_checkpoint(
    connection: Connection, tables: set[str], now: datetime
) -> bool:
    if LEGACY_MONITOR_CHECKPOINT_TABLE not in tables:
        return True
    if "device_monitor_checkpoints" not in tables:
        log.warning(
            "retaining %s: device_monitor_checkpoints table is missing",
            LEGACY_MONITOR_CHECKPOINT_TABLE,
        )
        return False

    device_value = connection.execute(
        text(
            "SELECT last_check_at FROM device_monitor_checkpoints "
            "WHERE hub_id = :hub_id"
        ),
        {"hub_id": settings.hub_id},
    ).scalar_one_or_none()
    if _valid_monitor_checkpoint(device_value, now):
        return True

    legacy_columns = {
        column["name"]
        for column in inspect(connection).get_columns(LEGACY_MONITOR_CHECKPOINT_TABLE)
    }
    if not {"hub_id", "last_check_at"} <= legacy_columns:
        log.warning(
            "retaining %s: expected hub_id and last_check_at columns",
            LEGACY_MONITOR_CHECKPOINT_TABLE,
        )
        return False

    legacy_value = connection.execute(
        text(
            "SELECT last_check_at FROM hub_monitor_checkpoints "
            "WHERE hub_id = :hub_id"
        ),
        {"hub_id": settings.hub_id},
    ).scalar_one_or_none()
    if not _valid_monitor_checkpoint(legacy_value, now):
        log.info(
            "retaining %s until Device Monitor commits a valid checkpoint for hub_id=%s",
            LEGACY_MONITOR_CHECKPOINT_TABLE,
            settings.hub_id,
        )
        return False

    connection.execute(
        text(
            "INSERT INTO device_monitor_checkpoints (hub_id, last_check_at) "
            "VALUES (:hub_id, :last_check_at) "
            "ON CONFLICT(hub_id) DO UPDATE "
            "SET last_check_at = excluded.last_check_at"
        ),
        {"hub_id": settings.hub_id, "last_check_at": legacy_value},
    )
    return True


def cleanup_legacy_monitor_tables(
    bind: Engine | None = None, *, force: bool = False
) -> tuple[str, ...]:
    """Drop only allowlisted retired monitoring tables after checkpoint handoff."""
    global _legacy_monitor_cleanup_pending
    target = bind if bind is not None else engine
    is_runtime_engine = target is engine
    if is_runtime_engine and not force and not _legacy_monitor_cleanup_pending:
        return ()
    if target.dialect.name != "sqlite":
        raise RuntimeError("legacy monitoring cleanup supports SQLite only")

    with target.connect() as connection:
        connection.exec_driver_sql("BEGIN IMMEDIATE")
        try:
            tables = set(inspect(connection).get_table_names())
            existing = [name for name in LEGACY_MONITOR_TABLES if name in tables]
            if not existing:
                connection.commit()
                if is_runtime_engine:
                    _legacy_monitor_cleanup_pending = False
                return ()

            checkpoint_ready = _migrate_legacy_monitor_checkpoint(
                connection, tables, datetime.now(UTC)
            )
            dropped = tuple(
                name
                for name in existing
                if name != LEGACY_MONITOR_CHECKPOINT_TABLE or checkpoint_ready
            )
            quote = connection.dialect.identifier_preparer.quote
            for name in dropped:
                connection.exec_driver_sql(f"DROP TABLE {quote(name)}")
            connection.commit()

            if is_runtime_engine:
                _legacy_monitor_cleanup_pending = (
                    LEGACY_MONITOR_CHECKPOINT_TABLE in existing
                    and not checkpoint_ready
                )
            if dropped:
                log.info("removed legacy monitoring tables: %s", ", ".join(dropped))
            return dropped
        except Exception:
            connection.rollback()
            raise


def legacy_monitor_cleanup_pending() -> bool:
    return _legacy_monitor_cleanup_pending


def _migrate_observations_report_identity() -> None:
    """
    Breaking Stage 1 cutover: report PK becomes (node_id, sequence_number).

    Development databases with the old observation_id primary key are rebuilt
    empty (payload shape is also hierarchical-only). Clear incompatible rows.
    """
    tables = set(inspect(engine).get_table_names())
    if "observations" not in tables:
        return
    columns = {column["name"] for column in inspect(engine).get_columns("observations")}
    if "id" in columns:
        return
    with engine.begin() as connection:
        connection.execute(text("DROP TABLE observations"))
    Base.metadata.create_all(bind=engine, tables=[Base.metadata.tables["observations"]])


def _migrate_device_diag_pending_ota() -> None:
    tables = set(inspect(engine).get_table_names())
    if "device_diag_state" not in tables:
        return
    columns = {
        column["name"] for column in inspect(engine).get_columns("device_diag_state")
    }
    if "pending_ota_json" not in columns:
        with engine.begin() as connection:
            connection.execute(
                text(
                    "ALTER TABLE device_diag_state ADD COLUMN pending_ota_json "
                    "TEXT NOT NULL DEFAULT '{}'"
                )
            )


def _migrate_node_chip_identity() -> None:
    tables = set(inspect(engine).get_table_names())
    if "nodes" not in tables:
        return
    columns = {column["name"] for column in inspect(engine).get_columns("nodes")}
    with engine.begin() as connection:
        if "chip_mac" not in columns:
            connection.execute(
                text("ALTER TABLE nodes ADD COLUMN chip_mac VARCHAR(17)")
            )
        if "chip_variant" not in columns:
            connection.execute(
                text("ALTER TABLE nodes ADD COLUMN chip_variant VARCHAR(128)")
            )
        connection.exec_driver_sql(
            "CREATE UNIQUE INDEX IF NOT EXISTS uq_nodes_chip_mac "
            "ON nodes (chip_mac) WHERE chip_mac IS NOT NULL"
        )


def _migrate_desired_state_firmware_id() -> None:
    columns = {
        column["name"] for column in inspect(engine).get_columns("desired_state")
    }
    if "firmware_id" not in columns:
        with engine.begin() as connection:
            connection.execute(
                text(
                    "ALTER TABLE desired_state ADD COLUMN firmware_id VARCHAR(64) NOT NULL DEFAULT ''"
                )
            )


def _migrate_device_https_credentials() -> None:
    node_columns = {column["name"] for column in inspect(engine).get_columns("nodes")}
    credential_columns = {
        column["name"] for column in inspect(engine).get_columns("node_credentials")
    }
    with engine.begin() as connection:
        if "recovery_opened_at" not in node_columns:
            connection.execute(
                text("ALTER TABLE nodes ADD COLUMN recovery_opened_at DATETIME")
            )
        if "device_api_token_hash" not in credential_columns:
            connection.execute(
                text(
                    "ALTER TABLE node_credentials ADD COLUMN device_api_token_hash VARCHAR(64)"
                )
            )
        if "device_api_issued_at" not in credential_columns:
            connection.execute(
                text(
                    "ALTER TABLE node_credentials ADD COLUMN device_api_issued_at DATETIME"
                )
            )
        if "device_api_revoked_at" not in credential_columns:
            connection.execute(
                text(
                    "ALTER TABLE node_credentials ADD COLUMN device_api_revoked_at DATETIME"
                )
            )
        connection.exec_driver_sql(
            "CREATE UNIQUE INDEX IF NOT EXISTS uq_node_credentials_device_api_token_hash "
            "ON node_credentials (device_api_token_hash) WHERE device_api_token_hash IS NOT NULL"
        )


def _migrate_bootstrap_token_active_index() -> None:
    with engine.begin() as connection:
        connection.exec_driver_sql(
            "CREATE UNIQUE INDEX IF NOT EXISTS uq_bootstrap_tokens_one_unconsumed_per_node "
            "ON bootstrap_tokens (node_id) WHERE consumed_at IS NULL"
        )


def _migrate_device_ownership_to_user_or_group() -> None:
    columns = {
        column["name"] for column in inspect(engine).get_columns("device_ownership")
    }
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
