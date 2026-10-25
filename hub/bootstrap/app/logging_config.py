"""Log lines shaped like the web and firmware-builder HTTP servers."""

import logging
import sys
import time

_MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
_LOG_FORMAT = "%(asctime)s %(levelname)s %(name)s %(message)s"


def access_timestamp(created: float) -> str:
    """English month stamp used by http.server.BaseHTTPRequestHandler."""
    when = time.localtime(created)
    month = _MONTHS[when.tm_mon - 1]
    return f"{when.tm_mday:02d}/{month}/{when.tm_year:04d} {when.tm_hour:02d}:{when.tm_min:02d}:{when.tm_sec:02d}"


def client_host(client_addr: str) -> str:
    """Drop the port uvicorn appends so the host matches the other HTTP servers."""
    host, separator, port = client_addr.rpartition(":")
    if separator and port.isdigit():
        return host
    return client_addr


class HubFormatter(logging.Formatter):
    def __init__(self, fmt: str = _LOG_FORMAT) -> None:
        super().__init__(fmt=fmt)

    def formatTime(self, record: logging.LogRecord, datefmt: str | None = None) -> str:
        return access_timestamp(record.created)


class HubAccessFormatter(logging.Formatter):
    """Common log line: host - - [date time] \"request\" status."""

    def format(self, record: logging.LogRecord) -> str:
        try:
            client_addr, method, full_path, http_version, status_code = record.args  # type: ignore[misc]
        except (TypeError, ValueError):
            return super().format(record)
        timestamp = access_timestamp(record.created)
        host = client_host(str(client_addr))
        return f'{host} - - [{timestamp}] "{method} {full_path} HTTP/{http_version}" {status_code} -'


def _restyle(logger_name: str, formatter: logging.Formatter, stream) -> None:
    logger = logging.getLogger(logger_name)
    logger.setLevel(logging.INFO)
    for handler in logger.handlers:
        handler.setFormatter(formatter)
        if isinstance(handler, logging.StreamHandler):
            handler.setStream(stream)


def configure_logging() -> None:
    """Stamp Hub logs with the same date and time as web and firmware-builder."""
    formatter = HubFormatter()
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    if not root.handlers:
        handler = logging.StreamHandler(sys.stderr)
        handler.setFormatter(formatter)
        root.addHandler(handler)

    _restyle("uvicorn", formatter, sys.stderr)
    _restyle("uvicorn.error", formatter, sys.stderr)
    access = logging.getLogger("uvicorn.access")
    access.setLevel(logging.INFO)
    access.propagate = False
    if not access.handlers:
        handler = logging.StreamHandler(sys.stderr)
        access.addHandler(handler)
    _restyle("uvicorn.access", HubAccessFormatter(), sys.stderr)
