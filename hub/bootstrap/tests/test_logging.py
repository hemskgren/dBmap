import logging
import sys
import time

from app.logging_config import (
    HubAccessFormatter,
    HubFormatter,
    client_host,
    configure_logging,
)


def test_client_host_drops_the_uvicorn_port() -> None:
    assert client_host("203.0.113.9:54432") == "203.0.113.9"
    assert client_host("2001:db8::10:0") == "2001:db8::10"
    assert client_host("203.0.113.9") == "203.0.113.9"


def test_access_log_includes_the_http_server_date_and_time() -> None:
    record = logging.LogRecord(
        name="uvicorn.access",
        level=logging.INFO,
        pathname=__file__,
        lineno=1,
        msg='%s - "%s %s HTTP/%s" %d',
        args=("203.0.113.9:54432", "GET", "/health", "1.1", 200),
        exc_info=None,
    )
    record.created = time.mktime((2026, 10, 25, 13, 53, 0, 0, 0, -1))

    line = HubAccessFormatter().format(record)

    assert line == '203.0.113.9 - - [25/Oct/2026 13:53:00] "GET /health HTTP/1.1" 200 -'


def test_service_logs_use_the_same_date_and_time() -> None:
    record = logging.LogRecord(
        name="hub-bootstrap.mqtt",
        level=logging.INFO,
        pathname=__file__,
        lineno=1,
        msg="mqtt connected rc=%s",
        args=(0,),
        exc_info=None,
    )
    record.created = time.mktime((2026, 10, 25, 13, 53, 0, 0, 0, -1))

    line = HubFormatter().format(record)

    assert line == "25/Oct/2026 13:53:00 INFO hub-bootstrap.mqtt mqtt connected rc=0"


def test_configure_logging_stamps_uvicorn_access(capsys) -> None:
    access = logging.getLogger("uvicorn.access")
    root = logging.getLogger()
    previous_handlers = list(access.handlers)
    previous_propagate = access.propagate
    previous_level = access.level
    previous_root_handlers = list(root.handlers)
    access.handlers.clear()
    access.addHandler(logging.StreamHandler(sys.stdout))
    try:
        configure_logging()
        access.info('%s - "%s %s HTTP/%s" %d', "203.0.113.9:54432", "GET", "/health", "1.1", 200)
        captured = capsys.readouterr()
        assert captured.out == ""
        assert captured.err.startswith("203.0.113.9 - - [")
        assert '"GET /health HTTP/1.1" 200 -' in captured.err
    finally:
        access.handlers.clear()
        access.handlers.extend(previous_handlers)
        access.propagate = previous_propagate
        access.setLevel(previous_level)
        root.handlers.clear()
        root.handlers.extend(previous_root_handlers)
