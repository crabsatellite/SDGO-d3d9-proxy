"""Serve battle reward controls alone or on an installed assistant's HTTP port."""
from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import hashlib
import importlib.util
import inspect
import json
import os
from pathlib import Path
import re
import socket
import sys
import uuid


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

# Load our owner API before any installed assistant can add packages with the
# same top-level names to sys.path.
from owner.rewards.runtime_control import RuntimeControl
from owner.rewards.workspace import Workspace


_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_REWARD_MODULE = Path(__file__).resolve().parent / "assistant_modules" / "rewards" / "__init__.py"


def _private_module(path: Path, prefix: str):
    name = prefix + hashlib.sha256(str(path).encode("utf-8")).hexdigest()[:16]
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ValueError(f"无法加载 Python 模块：{path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except Exception:
        sys.modules.pop(name, None)
        raise
    return module


def _reward_attach():
    module = _private_module(_REWARD_MODULE, "_sdgo_reward_mount_")
    attach = getattr(module, "attach", None)
    if not callable(attach):
        raise ValueError("奖励助手挂载模块缺少 attach(server, workspace)")
    return attach


def _inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _installed_module(root_value: Path, entry_value: Path):
    root = root_value.resolve()
    if not root.is_dir():
        raise ValueError(f"已安装助手目录不存在：{root}")
    entry = entry_value if entry_value.is_absolute() else root / entry_value
    entry = entry.resolve()
    if not _inside(entry, root):
        raise ValueError("助手入口必须位于 --assistant-root 内")
    if not entry.is_file():
        raise ValueError(f"助手入口不存在：{entry}")

    existing = sys.modules.get("assistant_modules")
    if existing is not None:
        locations = []
        if getattr(existing, "__file__", None):
            locations.append(Path(existing.__file__).resolve())
        locations.extend(Path(item).resolve() for item in getattr(existing, "__path__", ()))
        if locations and not any(_inside(item, root) for item in locations):
            raise ValueError("当前进程已加载另一套 assistant_modules；请用 reward_assistant.py 单独启动")

    original_path = list(sys.path)
    try:
        sys.path[:0] = [str(entry.parent), str(root)]
        module = _private_module(entry, "_sdgo_installed_assistant_")
    finally:
        sys.path[:] = original_path
    return module


def _attribute(value, dotted_name: str, role: str):
    current = value
    for name in dotted_name.split("."):
        if not _IDENTIFIER.fullmatch(name):
            raise ValueError(f"{role} 名称无效：{dotted_name}")
        if not hasattr(current, name):
            raise ValueError(f"已安装助手未导出 {role}：{dotted_name}")
        current = getattr(current, name)
    if not callable(current):
        raise ValueError(f"{role} 不是可调用对象：{dotted_name}")
    return current


def _call(target, values, role: str):
    try:
        signature = inspect.signature(target)
    except (TypeError, ValueError) as error:
        raise ValueError(f"无法读取{role}签名：{error}") from error

    remaining = dict(values)
    positional = []
    keywords = {}
    accepts_keywords = False
    missing = []
    for parameter in signature.parameters.values():
        if parameter.kind == inspect.Parameter.VAR_POSITIONAL:
            continue
        if parameter.kind == inspect.Parameter.VAR_KEYWORD:
            accepts_keywords = True
            continue
        if parameter.name in remaining:
            supplied = remaining.pop(parameter.name)
            if parameter.kind == inspect.Parameter.POSITIONAL_ONLY:
                positional.append(supplied)
            else:
                keywords[parameter.name] = supplied
        elif parameter.default is inspect.Parameter.empty:
            missing.append(parameter.name)
    if missing:
        raise ValueError(f"{role}缺少参数：{', '.join(missing)}")
    if remaining and not accepts_keywords:
        raise ValueError(f"{role}不接受参数：{', '.join(sorted(remaining))}")
    if accepts_keywords:
        keywords.update(remaining)
    return target(*positional, **keywords)


def _options(rows):
    result = {}
    for row in rows:
        name, separator, value = row.partition("=")
        if not separator or not _IDENTIFIER.fullmatch(name) or not value:
            raise ValueError("--assistant-option 必须使用 NAME=VALUE")
        if name in result:
            raise ValueError(f"重复的助手参数：{name}")
        result[name] = value
    return result


def _base_workspace(module, args):
    constructor = _attribute(module, args.assistant_workspace_factory, "Workspace 工厂")
    values = _options(args.assistant_option)
    common = {
        "root": args.assistant_workspace,
        "client": args.assistant_client,
        "snapshot": args.assistant_snapshot,
        "module_binary": args.assistant_module_binary,
        "server_ini": args.assistant_server_ini,
    }
    for name, value in common.items():
        if value is None:
            continue
        if name in values:
            raise ValueError(f"助手参数 {name} 同时使用了专用选项和 --assistant-option")
        values[name] = value.resolve()
    return _call(constructor, values, "Workspace 工厂")


def _check_port(port: int):
    if not 0 <= port <= 65535:
        raise ValueError("--port 必须在 0..65535")
    if port == 0:
        return
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        probe.bind(("127.0.0.1", port))
    except OSError as error:
        raise ValueError(f"本机端口 {port} 已被占用") from error
    finally:
        probe.close()


def _standalone_server(port: int):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True
    return server


def _validate_server(server):
    required = ("RequestHandlerClass", "server_address", "server_port", "serve_forever", "server_close")
    missing = [name for name in required if not hasattr(server, name)]
    if missing:
        raise ValueError("助手工厂没有返回兼容 HTTPServer 的对象：" + ", ".join(missing))
    if server.server_address[0] != "127.0.0.1":
        raise ValueError("共享助手必须只监听 127.0.0.1")


def build_server(args):
    """Build and mount the server without starting its serve_forever loop."""
    _check_port(args.port)
    attach = _reward_attach()
    server = None
    try:
        if args.assistant_root is None:
            server = _standalone_server(args.port)
            mode = "standalone"
        else:
            module = _installed_module(args.assistant_root, args.assistant_entry)
            base_workspace = _base_workspace(module, args)
            factory = _attribute(module, args.assistant_factory, "HTTP server 工厂")
            server = _call(
                factory,
                {"workspace": base_workspace, "port": args.port},
                "HTTP server 工厂",
            )
            mode = "shared"
        _validate_server(server)
        server.daemon_threads = True
        control = RuntimeControl(args.workspace, args.runtime, args.heartbeat_timeout)
        reward_workspace = Workspace(args.workspace, control)
        attach(server, reward_workspace)
        return server, reward_workspace, mode
    except Exception:
        if server is not None:
            server.server_close()
        raise


def _atomic_json(path: Path, value):
    data = (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name("." + path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def parser():
    value = argparse.ArgumentParser(description=__doc__)
    value.add_argument("--workspace", type=Path, required=True, help="Reward policy owner directory")
    value.add_argument("--runtime", type=Path, help="Native policy/status directory; defaults to WORKSPACE/runtime")
    value.add_argument("--heartbeat-timeout", type=int, default=15)
    value.add_argument("--port", type=int, default=0)
    value.add_argument("--assistant-root", type=Path, help="Installed assistant package root; omit for reward-only mode")
    value.add_argument("--assistant-entry", type=Path, default=Path("tools/mod_assistant.py"))
    value.add_argument("--assistant-factory", default="make_server")
    value.add_argument("--assistant-workspace-factory", default="Workspace")
    value.add_argument("--assistant-workspace", type=Path)
    value.add_argument("--assistant-client", type=Path)
    value.add_argument("--assistant-snapshot", type=Path)
    value.add_argument("--assistant-module-binary", type=Path)
    value.add_argument("--assistant-server-ini", type=Path)
    value.add_argument(
        "--assistant-option",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="Additional installed Workspace constructor argument",
    )
    return value


def main(argv=None):
    argument_parser = parser()
    args = argument_parser.parse_args(argv)
    installed_values = (
        args.assistant_workspace,
        args.assistant_client,
        args.assistant_snapshot,
        args.assistant_module_binary,
        args.assistant_server_ini,
        *args.assistant_option,
    )
    if args.assistant_root is None and any(value is not None and value != "" for value in installed_values):
        argument_parser.error("助手构造参数需要同时提供 --assistant-root")
    if args.assistant_root is not None and args.assistant_workspace is None:
        argument_parser.error("共享入口需要 --assistant-workspace")

    server = None
    try:
        server, workspace, mode = build_server(args)
        state = workspace.public()
        url = f"http://127.0.0.1:{server.server_port}/rewards"
        _atomic_json(
            workspace.root / "reward-assistant-session.json",
            {
                "schema_version": 1,
                "pid": os.getpid(),
                "url": url,
                "mode": mode,
                "status": state["status"],
                "delivery_connected": state["delivery_connected"],
            },
        )
    except Exception as error:
        if server is not None:
            server.server_close()
        argument_parser.error(f"助手启动失败：{error}")

    connection = "native settlement connected" if state["delivery_connected"] else "waiting for native settlement heartbeat"
    print(f"SDGO reward assistant: {url} ({mode}; {connection})", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
