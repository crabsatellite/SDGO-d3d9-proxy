"""Mount reward configuration and runtime health on an assistant HTTP port."""
import json
from pathlib import Path
import secrets
import traceback
from urllib.parse import urlsplit

MODULE = {"id": "rewards", "name": "战斗奖励", "url": "/rewards", "audience": "host"}


def attach(server, workspace):
    """Preserve existing routes; extend the assistant's json_response module list.

    Call before serve_forever. Existing modules keep their own session tokens;
    reward connectivity is derived from the workspace's verified native health.
    """
    parent = server.RequestHandlerClass
    if getattr(parent, "_reward_module_attached", False):
        raise ValueError("奖励模块已经挂载")
    token = secrets.token_urlsafe(32)
    workspace.public()

    class Handler(parent):
        _reward_module_attached = True

        def _reward_response(self, status, body, content_type="application/json; charset=utf-8"):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(body)

        def _reward_json(self, status, value):
            self._reward_response(status, json.dumps(value, ensure_ascii=False, allow_nan=False).encode("utf-8"))

        def _reward_host(self):
            return self.headers.get("Host") in (f"127.0.0.1:{self.server.server_port}", f"localhost:{self.server.server_port}")

        def json_response(self, status, value):
            if urlsplit(self.path).path == "/api/modules" and status == 200 and isinstance(value, dict):
                value = dict(value)
                modules = [dict(item) for item in value.get("modules", []) if isinstance(item, dict)]
                value["modules"] = [item for item in modules if item.get("id") != MODULE["id"]]
                value["modules"].append(dict(MODULE))
            if hasattr(parent, "json_response"):
                return super().json_response(status, value)
            return self._reward_json(status, value)

        def do_GET(self):
            path = urlsplit(self.path).path
            if path not in ("/rewards", "/api/rewards"):
                if path == "/api/modules" and not hasattr(parent, "do_GET"):
                    return self._reward_json(200, {"modules": [dict(MODULE)]}) if self._reward_host() else self._reward_json(403, {"error": "访问地址无效"})
                if hasattr(parent, "do_GET"):
                    return super().do_GET()
                return self._reward_json(404, {"error": "入口不存在"})
            if not self._reward_host():
                return self._reward_json(403, {"error": "访问地址无效"})
            if path == "/rewards":
                return self._reward_response(200, Path(__file__).with_name("ui.html").read_bytes(), "text/html; charset=utf-8")
            return self._reward_json(200, {**workspace.public(), "session_token": token})

        def do_POST(self):
            path = urlsplit(self.path).path
            if not path.startswith("/api/rewards/"):
                if hasattr(parent, "do_POST"):
                    return super().do_POST()
                return self._reward_json(404, {"error": "入口不存在"})
            origins = (f"http://127.0.0.1:{self.server.server_port}", f"http://localhost:{self.server.server_port}")
            if not self._reward_host() or self.headers.get("Origin") not in origins or not secrets.compare_digest(self.headers.get("X-Assistant-Token", ""), token):
                return self._reward_json(403, {"error": "请求不属于当前助手会话"})
            try:
                if self.headers.get("Content-Type", "").split(";", 1)[0] != "application/json":
                    raise ValueError("需要 JSON 请求")
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 65536:
                    raise ValueError("请求大小无效")
                value = json.loads(self.rfile.read(length))
                if path == "/api/rewards/preview":
                    result = workspace.preview(value)
                elif path == "/api/rewards/multiplier":
                    result = workspace.multiplier(value["base_multiplier"], self.headers.get("X-Reward-Revision", ""))
                elif path == "/api/rewards/policy":
                    result = workspace.save(value["policy"], self.headers.get("X-Reward-Revision", ""))
                else:
                    return self._reward_json(404, {"error": "入口不存在"})
                return self._reward_json(200, result)
            except (ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
                return self._reward_json(400, {"error": str(error)})
            except OSError:
                self._reward_log_error()
                return self._reward_json(500, {"error": "奖励运行时配置写入失败，请查看助手日志"})
            except Exception:
                self._reward_log_error()
                return self._reward_json(500, {"error": "奖励设置操作失败，请查看助手日志"})

        def _reward_log_error(self):
            try:
                with (workspace.root / "assistant-errors.log").open("a", encoding="utf-8") as stream:
                    stream.write(traceback.format_exc() + "\n")
            except OSError:
                pass

    server.RequestHandlerClass = Handler
    return server
