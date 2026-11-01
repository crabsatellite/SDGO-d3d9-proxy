"""Single loopback API. All providers share its origin, session and mode gates."""
from __future__ import annotations
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import threading
from urllib.parse import parse_qs, urlsplit
from . import storage
from .providers import Binary


def make_server(workspace, port=0):
    token = secrets.token_urlsafe(32)
    static = Path(__file__).with_name('static')

    class Handler(BaseHTTPRequestHandler):
        server_version = 'SDGOModManager/1'
        def log_message(self, *_): pass

        def host_ok(self):
            return self.headers.get('Host') in (f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}')

        def response(self, status, data, mime='application/json; charset=utf-8', filename=None):
            self.send_response(status)
            for key, value in [('Content-Type', mime), ('Content-Length', str(len(data))), ('Cache-Control', 'no-store'),
                ('X-Content-Type-Options', 'nosniff'), ('Referrer-Policy', 'no-referrer'),
                ('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' blob: data:; connect-src 'self'; frame-ancestors 'none'; object-src 'none'; base-uri 'none'")]:
                self.send_header(key, value)
            if filename:
                self.send_header('Content-Disposition', 'attachment; filename="' + filename + '"')
            self.end_headers()
            self.wfile.write(data)

        def send_json(self, status, value): self.response(status, storage.json_bytes(value))

        def execute(self, method):
            try:
                if not self.host_ok(): raise PermissionError('访问地址无效')
                split = urlsplit(self.path); path = split.path; query = parse_qs(split.query)
                body = {}
                if method != 'GET':
                    origins = (f'http://127.0.0.1:{self.server.server_port}', f'http://localhost:{self.server.server_port}')
                    if self.headers.get('Origin') not in origins or not secrets.compare_digest(self.headers.get('X-ModManager-Token', ''), token):
                        raise PermissionError('请求不属于当前 Manager 会话')
                    if self.headers.get('Content-Type', '').split(';')[0] != 'application/json': raise ValueError('需要 JSON 请求')
                    length = int(self.headers.get('Content-Length', '0'))
                    limit = 64 * 1024 * 1024 if path.startswith('/api/providers/capsules/') else 16 * 1024 * 1024
                    if not 0 < length <= limit: raise ValueError('请求大小无效')
                    body = json.loads(self.rfile.read(length))
                    if not isinstance(body, dict): raise ValueError('请求必须是 JSON 对象')
                if method == 'GET' and path in ('/', '/ui-kit.html'):
                    return self.response(200, (static / ('index.html' if path == '/' else 'ui-kit.html')).read_bytes(), 'text/html; charset=utf-8')
                if method == 'GET' and path in ('/app.js', '/style.css', '/panel_helpers.js', '/ui.css', '/ui.js'):
                    return self.response(200, (static / path[1:]).read_bytes(), 'text/javascript; charset=utf-8' if path.endswith('.js') else 'text/css; charset=utf-8')
                if method == 'GET' and path.startswith('/api/art/'):
                    from .art import image
                    try:
                        return self.response(200, image(workspace, path[len('/api/art/'):], (query.get('host') or [None])[0]), 'image/jpeg')
                    except LookupError as error:
                        return self.send_json(404, {'error': str(error)})
                if method == 'GET' and path.startswith('/panels/'):
                    name = path[len('/panels/'):]
                    if name not in {key + '.js' for key in workspace.providers}: raise ValueError('面板未登记')
                    return self.response(200, (static / 'panels' / name).read_bytes(), 'text/javascript; charset=utf-8')
                if method == 'GET' and path == '/api/state': return self.send_json(200, {**workspace.public(), 'session_token': token})
                if method == 'GET' and path == '/api/identity': return self.send_json(200, {'application': 'sdgo_mod_manager', 'workspace_id': storage.revision(str(workspace.root).encode())})
                if method == 'GET' and path == '/api/developer':
                    workspace.require_developer()
                    host=workspace.host(query['host'][0]);identity=query['mod'][0]
                    root=storage.safe_child(Path(host['path'])/'sdgo/mods',identity)
                    manifest=storage.ini_values((root/'mod.ini').read_bytes())
                    return self.send_json(200, {'host_id':host['id'],'mod_id':identity,'directory':str(root),'manifest':dict(manifest['mod']),'contract':workspace.contracts.get(identity)})
                if method == 'POST' and path == '/api/ai-context':
                    from .ai_context import build
                    return self.send_json(200, build(workspace, 'http://127.0.0.1:'+str(self.server.server_port)+'/', host_id=body.get('host'),mod_id=body.get('mod'),provider_id=body.get('provider'),kind=body.get('kind','configure'),problem=body.get('problem','')))
                if method == 'POST' and path == '/api/mode': return self.send_json(200, workspace.set_mode(body['mode']))
                if path == '/api/services' and method == 'GET': return self.send_json(200, workspace.services.describe())
                if path.startswith('/api/services/'):
                    identity = path[len('/api/services/'):]
                    if method == 'GET': return self.send_json(200, workspace.services.describe(identity))
                    if method == 'POST' and body.get('action') == 'start': return self.send_json(200, workspace.services.start(identity))
                    if method == 'POST' and body.get('action') == 'stop': return self.send_json(200, workspace.services.stop(identity))
                    raise ValueError('后台任务操作无效')
                if method == 'POST' and path == '/api/exit':
                    self.send_json(200,{'stopping':'manager_only'})
                    threading.Thread(target=self.server.shutdown,daemon=True).start()
                    return
                if method == 'POST' and path == '/api/hosts': return self.send_json(200, workspace.add_host(body['path']))
                if method == 'POST' and path == '/api/pick':
                    from .picker import choose
                    return self.send_json(200,choose(body.get('kind','directory')))
                if method == 'POST' and path == '/api/import':
                    from .packages import import_client
                    return self.send_json(200, import_client(workspace, body['host'], body['package']))
                if method == 'POST' and path == '/api/toggle':
                    return self.send_json(200, workspace.toggle(body['host'], body['mod'], body['enabled'], body['revision']))
                if method == 'POST' and path == '/api/category':
                    return self.send_json(200,workspace.category(body['host'],body['category'],body['enabled'],body['revision']))
                if path == '/api/settings':
                    if method == 'GET':
                        return self.send_json(200, workspace.details(query['host'][0], query['mod'][0]))
                    if method == 'POST':
                        return self.send_json(200, workspace.configure(body['host'], body['mod'], body['values'], body['revisions']))
                if path.startswith('/api/providers/'):
                    rest = path[len('/api/providers/'):]; identity, _, local = rest.partition('/')
                    provider = workspace.providers.get(identity)
                    if provider is None: raise ValueError('模块未登记')
                    if local == 'config':
                        if method == 'GET': return self.send_json(200, provider.describe())
                        if method == 'POST': return self.send_json(200, provider.configure(body))
                    with provider.lock:
                        result = provider.request(method, '/' + local + ('?' + split.query if split.query else ''), body, self.headers)
                    if isinstance(result, Binary): return self.response(200, result.data, result.mime, result.filename)
                    def urls(value):
                        if isinstance(value, list): return [urls(item) for item in value]
                        if isinstance(value, dict):
                            return {key: ('/api/providers/' + identity + item if key in ('url','asset_url','package_url') and isinstance(item,str) and item.startswith(('/api/','/assets/')) else urls(item)) for key,item in value.items()}
                        return value
                    if isinstance(result, dict): result = {**result, 'session_token': token}
                    return self.send_json(200, urls(result))
                return self.send_json(404, {'error': '此入口不存在'})
            except (BrokenPipeError,ConnectionAbortedError,ConnectionResetError): return
            except PermissionError as error: self.send_json(403, {'error': str(error)})
            except storage.Conflict as error: self.send_json(409, {'error': str(error)})
            except (ValueError, KeyError, TypeError, FileNotFoundError) as error: self.send_json(400, {'error': str(error)})
            except Exception as error:
                # Capsule domain errors carry a bounded, owner-readable message.
                from .engines.capsules.capsule_ui import RequestError, ConflictError
                if isinstance(error, RequestError): return self.send_json(409 if isinstance(error, ConflictError) else 400, {'error': str(error)})
                import traceback
                workspace.root.mkdir(parents=True, exist_ok=True)
                with (workspace.root / 'manager-errors.log').open('a', encoding='utf-8') as stream: stream.write(traceback.format_exc() + '\n')
                self.send_json(500, {'error': '操作未完成，请查看本机 Manager 日志'})

        def do_GET(self): self.execute('GET')
        def do_POST(self): self.execute('POST')
        def do_PUT(self): self.execute('PUT')
        def do_DELETE(self): self.execute('DELETE')

    server = ThreadingHTTPServer(('127.0.0.1', port), Handler)
    server.daemon_threads = True
    server.workspace = workspace
    return server
