"""Serve the static dBmap UI over HTTPS inside the Compose network."""

import ipaddress
import os
import ssl
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def _is_client_disconnect(exc: BaseException | None) -> bool:
    """A peer that closes mid-response is not a server failure."""
    if isinstance(exc, ConnectionError):
        return True
    if isinstance(exc, (ssl.SSLEOFError, ssl.SSLZeroReturnError)):
        return True
    if isinstance(exc, ssl.SSLError):
        reason = getattr(exc, "reason", "")
        text = reason if isinstance(reason, str) else str(exc)
        lowered = text.lower()
        return "reset" in lowered or "broken pipe" in lowered or "eof" in lowered
    return False


def _logged_client_ip(headers, fallback: str) -> str:
    """Use the client address nginx recorded when it is a single IP."""
    forwarded = ""
    if headers is not None:
        raw = headers.get("X-Forwarded-For") or headers.get("X-Real-IP") or ""
        forwarded = raw.split(",", 1)[0].strip()
    if forwarded:
        try:
            return str(ipaddress.ip_address(forwarded))
        except ValueError:
            return fallback
    return fallback


class StaticHandler(SimpleHTTPRequestHandler):
    def address_string(self) -> str:
        return _logged_client_ip(getattr(self, "headers", None), self.client_address[0])

    def handle(self) -> None:
        try:
            super().handle()
        except Exception as exc:
            if not _is_client_disconnect(exc):
                raise

    def do_GET(self) -> None:
        if self.path.split("?", 1)[0] != "/health":
            super().do_GET()
            return
        body = b"ok\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


class ThreadingHTTPSServer(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address) -> None:
        if _is_client_disconnect(sys.exception()):
            return
        super().handle_error(request, client_address)


def main() -> None:
    os.chdir(Path(__file__).parent)
    server = ThreadingHTTPSServer(("0.0.0.0", 443), StaticHandler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain("/certs/web.crt", "/certs/web.key")
    server.socket = context.wrap_socket(server.socket, server_side=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
