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


def get_db() -> Generator[Session, None, None]:
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()
