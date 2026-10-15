from datetime import datetime

from sqlalchemy.exc import IntegrityError
from sqlalchemy.orm import Session

from app.models import DeploymentAttempt, DeploymentAttemptEvent, utcnow

STAGE_ORDER = {
    "connecting": 0,
    "erasing": 1,
    "writing": 2,
    "verifying": 3,
}


class AttemptNotFound(Exception):
    pass


class AttemptAlreadyFinished(Exception):
    pass


class AttemptAlreadyInProgress(Exception):
    pass


class AttemptProgressRegressed(Exception):
    pass


def create_attempt(
    db: Session,
    node_id: str,
    firmware_id: str,
    firmware_version: str,
    artifact_sha256: str,
    initiated_by: str,
    flasher_version: str | None,
) -> DeploymentAttempt:
    active_attempt = (
        db.query(DeploymentAttempt)
        .filter(
            DeploymentAttempt.node_id == node_id,
            DeploymentAttempt.status == "in_progress",
        )
        .first()
    )
    if active_attempt is not None:
        raise AttemptAlreadyInProgress

    now = utcnow()
    attempt = DeploymentAttempt(
        node_id=node_id,
        firmware_id=firmware_id,
        firmware_version=firmware_version,
        artifact_sha256=artifact_sha256,
        status="in_progress",
        current_stage="connecting",
        progress_percent=0,
        initiated_by=initiated_by,
        flasher_version=flasher_version,
        started_at=now,
    )
    db.add(attempt)
    try:
        db.flush()
        db.add(
            DeploymentAttemptEvent(
                attempt_id=attempt.attempt_id,
                status=attempt.status,
                stage=attempt.current_stage,
                progress_percent=attempt.progress_percent,
                created_at=now,
            )
        )
        db.commit()
    except IntegrityError:
        db.rollback()
        active_attempt = (
            db.query(DeploymentAttempt)
            .filter(
                DeploymentAttempt.node_id == node_id,
                DeploymentAttempt.status == "in_progress",
            )
            .first()
        )
        if active_attempt is not None:
            raise AttemptAlreadyInProgress from None
        raise
    db.refresh(attempt)
    return attempt


def update_attempt(
    db: Session,
    node_id: str,
    attempt_id: str,
    status: str,
    stage: str,
    progress_percent: int | None,
    failure_code: str | None,
) -> DeploymentAttempt:
    attempt = db.get(DeploymentAttempt, attempt_id)
    if attempt is None or attempt.node_id != node_id:
        raise AttemptNotFound
    if attempt.status != "in_progress":
        raise AttemptAlreadyFinished

    next_progress = attempt.progress_percent if progress_percent is None else progress_percent
    if next_progress < attempt.progress_percent:
        raise AttemptProgressRegressed
    if (
        status == "in_progress"
        and stage in STAGE_ORDER
        and STAGE_ORDER[stage] < STAGE_ORDER[attempt.current_stage]
    ):
        raise AttemptProgressRegressed

    now = utcnow()
    attempt.status = status
    attempt.current_stage = stage
    attempt.progress_percent = next_progress
    attempt.failure_code = failure_code
    if status in {"succeeded", "failed", "unknown"}:
        attempt.completed_at = now
    db.add(
        DeploymentAttemptEvent(
            attempt_id=attempt.attempt_id,
            status=status,
            stage=stage,
            progress_percent=next_progress,
            failure_code=failure_code,
            created_at=now,
        )
    )
    db.commit()
    db.refresh(attempt)
    return attempt


def get_attempt(db: Session, node_id: str, attempt_id: str) -> DeploymentAttempt:
    attempt = db.get(DeploymentAttempt, attempt_id)
    if attempt is None or attempt.node_id != node_id:
        raise AttemptNotFound
    return attempt


def list_attempts(db: Session, node_id: str, limit: int) -> list[DeploymentAttempt]:
    return (
        db.query(DeploymentAttempt)
        .filter(DeploymentAttempt.node_id == node_id)
        .order_by(DeploymentAttempt.started_at.desc())
        .limit(limit)
        .all()
    )


def public_attempt(db: Session, attempt: DeploymentAttempt) -> dict:
    events = (
        db.query(DeploymentAttemptEvent)
        .filter(DeploymentAttemptEvent.attempt_id == attempt.attempt_id)
        .order_by(DeploymentAttemptEvent.created_at.asc(), DeploymentAttemptEvent.event_id.asc())
        .all()
    )
    return {
        "attempt_id": attempt.attempt_id,
        "node_id": attempt.node_id,
        "firmware_id": attempt.firmware_id,
        "firmware_version": attempt.firmware_version,
        "artifact_sha256": attempt.artifact_sha256,
        "status": attempt.status,
        "stage": attempt.current_stage,
        "progress_percent": attempt.progress_percent,
        "failure_code": attempt.failure_code,
        "initiated_by": attempt.initiated_by,
        "flasher_version": attempt.flasher_version,
        "started_at": _isoformat(attempt.started_at),
        "completed_at": _isoformat(attempt.completed_at),
        "events": [
            {
                "status": event.status,
                "stage": event.stage,
                "progress_percent": event.progress_percent,
                "failure_code": event.failure_code,
                "created_at": _isoformat(event.created_at),
            }
            for event in events
        ],
    }


def _isoformat(value: datetime | None) -> str | None:
    return value.isoformat() if value is not None else None
