"""Legacy capsule HTTP contract adapter for isolated tests only; never packaged."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit
import json
import capsule_ui as domain
# Tests exercise the current Manager business module behind the historical routes.
globals().update({k:v for k,v in vars(domain).items() if not k.startswith('__')})

class CapsuleHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False


class CapsuleHandler(BaseHTTPRequestHandler):
    server_version = "CapsuleEditor/1"

    @property
    def app(self):
        return self.server.app

    def log_message(self, fmt, *args):
        # The UI can request hundreds of thumbnails. Keep the startup URL
        # readable and return actionable errors to the browser as JSON.
        return

    def _allowed_host(self):
        port = self.server.server_address[1]
        return {f"127.0.0.1:{port}", f"localhost:{port}"}

    def _check_host(self):
        host = self.headers.get("Host", "")
        if host not in self._allowed_host():
            raise RequestError("Invalid Host header")
        return host

    def _headers(self, status, content_type, length=0, download_name=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self'; "
            "img-src 'self' data:; connect-src 'self'; object-src 'none'; "
            "base-uri 'none'; frame-ancestors 'none'; form-action 'self'",
        )
        if download_name:
            self.send_header(
                "Content-Disposition", f'attachment; filename="{download_name}"'
            )
        self.end_headers()

    def _send_bytes(
        self, data, content_type, status=200, download_name=None, head=False
    ):
        self._headers(status, content_type, len(data), download_name)
        if not head:
            self.wfile.write(data)

    def _send_json(self, value, status=200, head=False):
        self._send_bytes(
            json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8"),
            "application/json; charset=utf-8",
            status,
            head=head,
        )

    def _error(self, error, head=False):
        status = getattr(error, "status", 400)
        self._send_json({"error": str(error), "status": status}, status, head=head)

    def _query_one(self, query, name, default=None):
        values = query.get(name)
        if not values:
            return default
        return values[-1]

    def _handle_get(self, head=False):
        self._check_host()
        split = urlsplit(self.path)
        path = split.path
        query = parse_qs(split.query)
        if path == "/":
            data, content_type = self.app.static("index.html")
            self._send_bytes(data, content_type, head=head)
            return
        if path in ("/app.js", "/style.css"):
            data, content_type = self.app.static(path[1:])
            self._send_bytes(data, content_type, head=head)
            return
        if path == "/favicon.ico":
            self._send_bytes(b"", "image/x-icon", status=204, head=head)
            return
        asset = ASSET_RE.fullmatch(path)
        if asset:
            file_path = self.app.store.asset(int(asset.group(1)))
            self._send_bytes(file_path.read_bytes(), "image/png", head=head)
            return
        if path == "/api/bootstrap":
            self._send_json(self.app.bootstrap(), head=head)
            return
        if path == "/api/machine":
            kind = self._query_one(query, "kind")
            raw_id = self._query_one(query, "id")
            try:
                machine_id = int(raw_id)
            except (TypeError, ValueError):
                raise RequestError("Machine ID must be an integer")
            self._send_json(self.app.machine(kind, machine_id), head=head)
            return
        if path == "/api/units":
            raw_limit = self._query_one(query, "limit", "100")
            try:
                limit = int(raw_limit)
            except ValueError:
                raise RequestError("Unit result limit must be an integer")
            self._send_json(
                self.app.units(self._query_one(query, "q", ""), limit), head=head
            )
            return
        if path == "/api/portraits":
            try:
                unit_id = int(self._query_one(query, "unit_id"))
                variant = int(self._query_one(query, "variant"))
            except (TypeError, ValueError):
                raise RequestError(
                    "Portrait unit_id and variant must be integers")
            self._send_json(
                self.app.store.portrait_get(unit_id, variant), head=head)
            return
        if path == "/api/catalog/download":
            catalog, _ = self.app.store.read()
            self._send_bytes(
                _json_bytes(catalog),
                "application/json; charset=utf-8",
                download_name="capsule-catalog.json",
                head=head,
            )
            return
        raise MissingError("Resource not found")

    def do_GET(self):
        try:
            self._handle_get()
        except (RequestError, ValueError, KeyError, OSError, json.JSONDecodeError) as error:
            self._error(error)

    def do_HEAD(self):
        try:
            self._handle_get(head=True)
        except (RequestError, ValueError, KeyError, OSError, json.JSONDecodeError) as error:
            self._error(error, head=True)

    def do_POST(self):
        try:
            host = self._check_host()
            origin = self.headers.get("Origin", "")
            if origin != f"http://{host}":
                raise RequestError("Mutation requires a same-origin request")
            content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip()
            if content_type != "application/json":
                raise RequestError("Mutation requires application/json")
            token = self.headers.get("X-Capsule-Token", "")
            if not hmac.compare_digest(token, self.app.token):
                raise RequestError("Invalid CSRF token")
            try:
                length = int(self.headers.get("Content-Length", ""))
            except ValueError:
                raise RequestError("Invalid Content-Length")
            if not 0 <= length <= MAX_REQUEST_BYTES:
                raise RequestError("Request body is too large")
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(payload, dict):
                raise RequestError("Request body must be a JSON object")
            result = self.app.dispatch_post(urlsplit(self.path).path, payload)
            self._send_json(result)
        except (RequestError, ValueError, KeyError, OSError, json.JSONDecodeError) as error:
            self._error(error)


def create_server(store, ui_path=None, port=0):
    ui_path = (
        Path(ui_path)
        if ui_path
        else Path(__file__).resolve().parents[1] / "ui" / "capsules"
    )
    app = CapsuleApplication(store, ui_path)
    server = CapsuleHTTPServer(("127.0.0.1", port), CapsuleHandler)
    server.app = app
    return server


domain.create_server = create_server
