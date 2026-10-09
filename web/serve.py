"""Serve the static dBmap UI over HTTPS inside the Compose network."""

import os
import ssl
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class ThreadingHTTPSServer(ThreadingHTTPServer):
    daemon_threads = True


def main() -> None:
    os.chdir(Path(__file__).parent)
    server = ThreadingHTTPSServer(("0.0.0.0", 443), SimpleHTTPRequestHandler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain("/certs/web.crt", "/certs/web.key")
    server.socket = context.wrap_socket(server.socket, server_side=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
