"""Owner configuration, native policy publication and verified runtime health."""
import copy
import json
import os
from pathlib import Path
import threading
import uuid
from . import model
from .runtime_control import RuntimeControl


class Workspace:
    def __init__(self, root, runtime_control=None):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.path = self.root / "reward-policy.json"
        self.lock = threading.RLock()
        self.runtime = runtime_control or RuntimeControl(self.root)
        if not self.path.exists():
            self._write(model.defaults())
        self.runtime.publish(self.read())

    def _write(self, value):
        temporary = self.path.with_name(".reward-" + uuid.uuid4().hex + ".tmp")
        try:
            data = (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
            with temporary.open("wb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, self.path)
        finally:
            if temporary.exists():
                temporary.unlink()

    def read(self):
        return model.validate(json.loads(self.path.read_text(encoding="utf-8-sig")))

    def public(self):
        with self.lock:
            policy = self.read()
            return self.runtime.decorate({
                "policy": policy,
                "revision": model.revision(policy),
                "delivery_connected": False,
                "status": "waiting_for_native",
                "human_rule": "1.1^(human_count-1)",
                "ai_match_tier": "same_as_pvp",
                "event_multiplier_source": "double_time_component",
            })

    def save(self, value, expected):
        candidate = model.validate(copy.deepcopy(value))
        with self.lock:
            current = self.read()
            old_revision = model.revision(current)
            if expected != old_revision:
                raise ValueError("配置已改变，请刷新后重试")
            history = self.root / "history" / (old_revision + ".json")
            if not history.exists():
                history.parent.mkdir(exist_ok=True)
                history.write_bytes(self.path.read_bytes())
            self._write(candidate)
            try:
                self.runtime.publish(candidate)
            except OSError as error:
                self._write(current)
                try:
                    self.runtime.publish(current)
                except OSError:
                    pass
                raise error
            return self.public()

    def multiplier(self, value, expected):
        with self.lock:
            return self.save(model.with_multiplier(self.read(), value), expected)

    def preview(self, value):
        with self.lock:
            policy = self.read()
            saved_revision = model.revision(policy)
            if "preview_multiplier" in value:
                policy = model.with_multiplier(policy, value["preview_multiplier"])
            result = model.preview(policy, value["context"], value["stats"])
            result["saved_policy_revision"] = saved_revision
            return result
