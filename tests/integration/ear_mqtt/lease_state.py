"""Lease authority shared by the test Hub ingress and the broker ACL plugin."""

import fcntl
import os
import secrets
import time
from contextlib import contextmanager
from pathlib import Path

from app.db import SessionLocal
from app.models import Node

TEST_NODE = "TST-EAR-001"
MAX_TTL_SECONDS = 10
NS_PER_SECOND = 1_000_000_000


class LeaseBusy(RuntimeError):
    pass


class TestLease:
    __test__ = False

    def __init__(self, root: Path):
        self.root = root
        self.state = root / "active"

    @contextmanager
    def _lock(self):
        with (self.root / "lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            yield

    def _current(self):
        try:
            issued, deadline, token = self.state.read_text().split()
            issued, deadline = int(issued), int(deadline)
            if not 0 < deadline - issued <= MAX_TTL_SECONDS * NS_PER_SECOND:
                return 0, token
            if issued > time.monotonic_ns():
                return 0, token
            return deadline, token
        except FileNotFoundError:
            return 0, None

    @staticmethod
    def _set_node(active: bool):
        with SessionLocal() as db:
            node = db.get(Node, TEST_NODE)
            node.lifecycle_state = "active" if active else "deactivated"
            db.commit()

    def _disable(self):
        # Revoke broker access first. The broker never accepts a missing lease.
        self.state.unlink(missing_ok=True)
        self._set_node(False)

    def acquire(self, ttl_seconds: int):
        if type(ttl_seconds) is not int or not 1 <= ttl_seconds <= MAX_TTL_SECONDS:
            raise ValueError("test lease TTL out of range")
        with self._lock():
            deadline, _ = self._current()
            if time.monotonic_ns() < deadline:
                raise LeaseBusy("test identity already leased")
            self._disable()
            token = secrets.token_hex(32)
            issued = time.monotonic_ns()
            deadline = issued + ttl_seconds * NS_PER_SECOND
            pending = self.root / "pending"
            pending.unlink(missing_ok=True)
            try:
                # Never publish a partial lease to the broker.
                fd = os.open(pending, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, "w") as stream:
                    stream.write(f"{issued} {deadline} {token}\n")
                self._set_node(True)
                pending.replace(self.state)
            except BaseException:
                pending.unlink(missing_ok=True)
                self._disable()
                raise
            return deadline, token

    def release(self, token: str):
        with self._lock():
            _, current = self._current()
            if current == token:
                self._disable()

    def reap_expired(self):
        with self._lock():
            deadline, token = self._current()
            if token and time.monotonic_ns() >= deadline:
                self._disable()

    @contextmanager
    def admit(self):
        # Keep revocation and Hub delivery ordered. The broker checks independently.
        with self._lock():
            deadline, token = self._current()
            with SessionLocal() as db:
                node = db.get(Node, TEST_NODE)
                active = node is not None and node.lifecycle_state == "active"
            yield bool(active and token and time.monotonic_ns() < deadline)
