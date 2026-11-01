"""Local SDGO mod assistant. The operator module does not manage storefronts."""
import argparse
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import re
import threading
import traceback
from urllib.parse import urlparse

from assistant_modules.operators import model
from assistant_modules.operators.workspace import Workspace


def make_server(workspace, port=0):
    token = secrets.token_urlsafe(32)
    ui = Path(__file__).parent / "assistant_modules" / "operators" / "ui.html"

    class Handler(BaseHTTPRequestHandler):
        server_version = "SDGOModAssistant/0.1"

        def log_message(self, fmt, *args):
            pass

        def allowed_host(self):
            return self.headers.get("Host") in {f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}"}

        def response(self, status, body, content_type="application/json; charset=utf-8", download=None):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self'; frame-ancestors 'none'")
            if download:
                self.send_header("Content-Disposition", f'attachment; filename="{download}"')
            self.end_headers()
            self.wfile.write(body)

        def json_response(self, status, value):
            self.response(status, json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8"))

        def do_GET(self):
            if not self.allowed_host():
                self.json_response(403,{"error":"访问地址不受信任"}); return
            path = urlparse(self.path).path
            try:
                if path in ("/", "/operators"):
                    self.response(200,ui.read_bytes(),"text/html; charset=utf-8")
                elif path == "/api/modules":
                    self.json_response(200,{"modules":[{"id":"operators","name":"副官","url":"/operators","audience":"host"}]})
                elif path == "/api/operators":
                    with model.LOCK:
                        self.json_response(200,{**workspace.public(),"session_token":token})
                elif path.startswith("/api/operators/portrait/"):
                    identity = int(path.rsplit("/",1)[1])
                    if identity not in workspace.baseline["native_operator_ids"]:
                        raise ValueError("头像不存在")
                    self.response(200,workspace.assets.portrait(identity),"image/png")
                elif path.startswith("/api/operators/upload/"):
                    sha = path.rsplit("/",1)[1]
                    catalog = workspace.read()
                    if sha not in catalog["uploaded_assets"] or not re.fullmatch('[0-9a-f]{64}',sha):
                        raise ValueError("上传头像不存在")
                    self.response(200,(workspace.root/"uploads"/(sha+".png")).read_bytes(),"image/png")
                elif path == "/api/operators/package":
                    with model.LOCK:
                        self.response(200,workspace.package(),"application/zip","SDGO-operator-client.zip")
                elif path == "/api/operators/registrations":
                    self.response(200,json.dumps(model.registrations(workspace.read()),ensure_ascii=False,indent=2).encode(),
                                  "application/json; charset=utf-8","operator-registrations.json")
                else:
                    self.json_response(404,{"error":"入口不存在"})
            except (ValueError,KeyError,OSError) as error:
                self.json_response(400,{"error":str(error)})
            except Exception:
                with (workspace.root/'assistant-errors.log').open('a',encoding='utf-8') as f:
                    f.write(traceback.format_exc()+'\n')
                self.json_response(500,{"error":"读取副官数据失败"})

        def write_request(self):
            origin = self.headers.get("Origin")
            if (not self.allowed_host() or origin not in {f"http://127.0.0.1:{self.server.server_port}", f"http://localhost:{self.server.server_port}"}
                or not secrets.compare_digest(self.headers.get("X-Assistant-Token", ""),token)):
                self.json_response(403,{"error":"此写入不属于当前助手会话"}); return
            if self.headers.get("Content-Type", "").split(";")[0] != "application/json":
                self.json_response(400,{"error":"需要 JSON 请求"}); return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 12582912:
                    raise ValueError("请求内容过大或为空")
                body = json.loads(self.rfile.read(length))
                expected = self.headers.get("X-Catalog-Revision", "")
                path = urlparse(self.path).path
                with model.LOCK:
                    catalog = workspace.check(expected)
                    if path == "/api/operators/new" and self.command == "POST":
                        result, identity = model.create_operator(catalog,int(body["template_id"]))
                        response = workspace.commit(result)
                        response["selected_id"] = identity
                    elif path == "/api/operators/avatar" and self.command == "POST":
                        response = workspace.upload(base64.b64decode(body["png"],validate=True),expected)
                    elif path == "/api/operators/apply" and self.command == "POST":
                        response = {"applied":workspace.apply(expected)}
                    elif path.startswith("/api/operators/") and self.command in ("PUT","DELETE"):
                        identity = int(path.rsplit("/",1)[1])
                        value = body if self.command == "PUT" else {"enabled":False}
                        response = workspace.commit(model.update_operator(catalog,identity,value))
                    else:
                        self.json_response(404,{"error":"入口不存在"}); return
                self.json_response(200,response)
            except (ValueError,KeyError,TypeError,OSError) as error:
                self.json_response(400,{"error":str(error)})
            except Exception:
                with (workspace.root/'assistant-errors.log').open('a',encoding='utf-8') as f:
                    f.write(traceback.format_exc()+'\n')
                self.json_response(500,{"error":"操作未完成；服务端写入若已开始则会回滚。请查看宿主日志。"})

        do_POST = write_request
        do_PUT = write_request
        do_DELETE = write_request

    server = ThreadingHTTPServer(("127.0.0.1",port),Handler)
    server.daemon_threads = True
    return server


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace",type=Path,required=True)
    parser.add_argument("--client",type=Path,required=True)
    parser.add_argument("--snapshot",type=Path,required=True)
    parser.add_argument("--module-binary",type=Path)
    parser.add_argument("--server-ini",type=Path)
    parser.add_argument("--port",type=int,default=0)
    args = parser.parse_args()
    if args.module_binary is None:
        root=Path(__file__).resolve().parent.parent
        for candidate in (root/'client/sdgo/mods/operator_catalog/operator_catalog.dll',
                          root/'build/native/Release/operator_catalog.dll'):
            if candidate.is_file():
                args.module_binary=candidate
                break
    workspace = Workspace(args.workspace,args.client,args.snapshot,args.module_binary,args.server_ini)
    server = make_server(workspace,args.port)
    model.save(args.workspace/"assistant-session.json",{"url":f"http://127.0.0.1:{server.server_port}/operators"})
    print(f"SDGO assistant: http://127.0.0.1:{server.server_port}/operators",flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
