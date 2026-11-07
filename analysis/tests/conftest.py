from pathlib import Path

import pytest
from app import policy
from app.config import settings

settings.database_url = "sqlite:///" + str(Path("/tmp/dbmap-analysis-test.db"))
settings.admin_token = "test-admin-token-analysis-0001"
settings.viewer_token = "test-viewer-token-analysis-0001"


def _test_policy(input_data: dict) -> bool:
    subject = input_data.get("subject", {})
    if not subject.get("authenticated"):
        return False
    if subject.get("role") == "admin":
        return True
    return subject.get("role") == "viewer" and input_data.get("action") in {
        "analysis.scratch.read",
    }


@pytest.fixture(autouse=True)
def use_test_policy(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(policy, "evaluate", _test_policy)


@pytest.fixture(autouse=True)
def reset_database() -> None:
    from app.db import SessionLocal, engine, init_db
    from app.models import Base

    Base.metadata.drop_all(bind=engine)
    init_db()
    SessionLocal().close()
