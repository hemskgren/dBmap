import importlib.util
import ssl
import threading
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

SERVE_PATH = Path(__file__).resolve().parents[2] / "web" / "serve.py"
spec = importlib.util.spec_from_file_location("dbmap_web_serve", SERVE_PATH)
serve = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(serve)


def test_access_log_uses_the_forwarded_client(capsys) -> None:
    handler = serve.StaticHandler.__new__(serve.StaticHandler)
    handler.client_address = ("172.19.0.8", 45864)
    handler.headers = {"X-Forwarded-For": "203.0.113.9"}
    handler.log_message('"%s" %s %s', "GET / HTTP/1.1", "200", "-")

    captured = capsys.readouterr()
    assert "203.0.113.9" in captured.err
    assert "172.19.0.8" not in captured.err


def test_access_log_ignores_a_spoofed_forwarded_header(capsys) -> None:
    handler = serve.StaticHandler.__new__(serve.StaticHandler)
    handler.client_address = ("172.19.0.8", 45864)
    handler.headers = {
        "X-Forwarded-For": "203.0.113.9\ninjected",
        "X-Real-IP": "not-an-ip",
    }
    handler.log_message("%s", "GET / HTTP/1.1")

    captured = capsys.readouterr()
    assert "172.19.0.8" in captured.err
    assert "injected" not in captured.err


def test_health_returns_a_short_body() -> None:
    server = ThreadingHTTPServer(("127.0.0.1", 0), serve.StaticHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        port = server.server_address[1]
        with urllib.request.urlopen(
            f"http://127.0.0.1:{port}/health", timeout=2
        ) as response:
            assert response.status == 200
            assert response.read() == b"ok\n"
    finally:
        server.shutdown()
        server.server_close()


def test_client_disconnect_is_not_a_server_error() -> None:
    assert serve._is_client_disconnect(
        ConnectionResetError(104, "Connection reset by peer")
    )
    assert serve._is_client_disconnect(BrokenPipeError(32, "Broken pipe"))
    assert serve._is_client_disconnect(
        ssl.SSLEOFError("EOF occurred in violation of protocol")
    )
    assert not serve._is_client_disconnect(RuntimeError("disk failed"))
    assert not serve._is_client_disconnect(None)


def test_handler_returns_when_the_peer_resets(monkeypatch) -> None:
    def fail(self) -> None:
        raise ConnectionResetError(104, "Connection reset by peer")

    monkeypatch.setattr(serve.SimpleHTTPRequestHandler, "handle", fail)
    handler = serve.StaticHandler.__new__(serve.StaticHandler)

    serve.StaticHandler.handle(handler)


def test_handler_reports_other_failures(monkeypatch) -> None:
    def fail(self) -> None:
        raise RuntimeError("disk failed")

    monkeypatch.setattr(serve.SimpleHTTPRequestHandler, "handle", fail)
    handler = serve.StaticHandler.__new__(serve.StaticHandler)

    try:
        serve.StaticHandler.handle(handler)
    except RuntimeError as exc:
        assert str(exc) == "disk failed"
    else:
        raise AssertionError("unexpected handler errors must propagate")


def test_server_prints_nothing_for_a_connection_reset(capsys) -> None:
    server = serve.ThreadingHTTPSServer(("127.0.0.1", 0), serve.StaticHandler)
    try:
        try:
            raise ConnectionResetError(104, "Connection reset by peer")
        except ConnectionResetError:
            server.handle_error(None, ("172.19.0.4", 45864))
    finally:
        server.server_close()

    captured = capsys.readouterr()
    assert captured.out == ""
    assert captured.err == ""


def test_server_still_prints_unexpected_errors(capsys) -> None:
    server = serve.ThreadingHTTPSServer(("127.0.0.1", 0), serve.StaticHandler)
    try:
        try:
            raise RuntimeError("disk failed")
        except RuntimeError:
            server.handle_error(None, ("127.0.0.1", 9))
    finally:
        server.server_close()

    captured = capsys.readouterr()
    combined = captured.out + captured.err
    assert "Exception occurred" in combined
    assert "Traceback" in combined
    assert "disk failed" in combined
