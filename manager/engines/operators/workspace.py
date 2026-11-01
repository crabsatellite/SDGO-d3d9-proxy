import copy
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile
from . import assets, model, server, presets


class Workspace:
    def __init__(self, root, client, snapshot, module_binary=None, server_ini=None):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.assets = assets.Assets(client)
        self.snapshot = Path(snapshot)
        self.module_binary = Path(module_binary) if module_binary else None
        self.server_ini = Path(server_ini) if server_ini else None
        self.path = self.root / "operators.json"
        self.baseline = model.import_catalog(snapshot, self.assets)
        if not self.path.exists():
            if presets.available(snapshot):
                self.baseline=presets.initialize(self.baseline,snapshot,self.assets)
            for op in self.baseline["operators"]:
                avatar=op["avatar"]
                if avatar["kind"]=="upload":
                    raw=(self.assets.root/'sdgo/mods/operator_catalog/assets'/f"{op['id']}.txr").read_bytes()
                    if hashlib.sha256(raw).hexdigest()!=avatar["sha256"]:
                        raise ValueError("已安装副官头像与配置不匹配")
                    assets.validate_png(raw)
                    target=self.root/'uploads'/(avatar['sha256']+'.png')
                    target.parent.mkdir(exist_ok=True)
                    if not target.exists():target.write_bytes(raw)
            model.save(self.path, self.baseline)
        self.read()

    def read(self):
        value = model.load(self.path)
        model.validate(value)
        if value["client_index_sha256"] != self.assets.index_hash:
            raise ValueError("客户端数据包已变化，请在新的助手工作目录重新导入")
        return value

    def check(self, expected):
        value = self.read()
        if expected != model.revision(value):
            raise ValueError("配置已在另一处改变，请刷新后重试")
        return value

    def commit(self, catalog):
        model.validate(catalog)
        old = self.path.read_bytes()
        backup = self.root / "history" / (hashlib.sha256(old).hexdigest() + ".json")
        if not backup.exists():
            backup.parent.mkdir(exist_ok=True)
            backup.write_bytes(old)
        model.save(self.path, catalog)
        model.save(self.root / "product-registrations.json", model.registrations(catalog))
        return self.public(catalog)

    def public(self, value=None):
        value = value or self.read()
        return {"schema_version": 1, "revision": model.revision(value), "operators": value["operators"],
                "drop_pools":value.get('drop_pools',{}),
                "items": value["items"], "native_operator_ids": value["native_operator_ids"],
                "gallery": [{"id": o["id"], "name": o["name"], "url": f"/api/operators/portrait/{o['id']}"}
                            for o in self.baseline["operators"] if o['id'] in self.baseline['native_operator_ids']],
                "capabilities": {"server_apply": self.server_ini is not None,
                                 "client_package": bool(self.module_binary and self.module_binary.exists()),
                                 "storefront_changes": False}}

    def upload(self, data, expected):
        catalog = self.check(expected)
        width, height = assets.validate_png(data)
        sha = hashlib.sha256(data).hexdigest()
        path = self.root / "uploads" / (sha + ".png")
        path.parent.mkdir(exist_ok=True)
        if not path.exists():
            path.write_bytes(data)
        catalog["uploaded_assets"][sha] = {"width": width, "height": height}
        result = self.commit(catalog)
        result["uploaded_avatar"] = {"kind": "upload", "sha256": sha}
        return result

    def package(self):
        catalog = self.read()
        if not self.module_binary or not self.module_binary.is_file():
            raise ValueError("当前助手包缺少已编译的副官客户端模块")
        rows = self.assets.records("gdrs/0255.gdt", assets.OP_SIZE)
        files = {}
        prefix = "client/sdgo/mods/operator_catalog/"
        files[prefix + "operator_catalog.dll"] = self.module_binary.read_bytes()
        files[prefix + "mod.ini"] = (server.MODULE_DIR / "mod.ini").read_bytes()
        ini = ["[catalog]", "Version=1", "Revision=" + model.revision(catalog)]
        for op in catalog["operators"]:
            if not op["managed"]:
                continue
            avatar = op["avatar"]
            profile = avatar["operator_id"] if avatar["kind"] == "native" else op["template_id"]
            sprite = ""
            if avatar["kind"] == "upload":
                raw = (self.root / "uploads" / (avatar["sha256"] + ".png")).read_bytes()
                if hashlib.sha256(raw).hexdigest() != avatar["sha256"]:
                    raise ValueError("头像文件校验失败")
                assets.validate_png(raw)
                texture = f"sdgo\\mods\\operator_catalog\\assets\\{op['id']}.txr"
                files[prefix + f"assets/{op['id']}.txr"] = raw
                sprite = f"sdgo\\mods\\operator_catalog\\assets\\{op['id']}.spr"
                for index, (offset,capacity) in enumerate(assets.SPRITE_FIELDS):
                    original = assets.wchar(rows[profile],offset,capacity)
                    if original:
                        spr = self.assets.read(original)
                        height, width = struct.unpack_from("<II",spr,12)
                    else:
                        width=height=128
                    files[prefix+f"assets/{op['id']}_{index}.spr"] = assets.sprite_binary(texture,width,height)
            ini += ["", f"[operator:{op['id']}]", f"Category={op['category_id']}",
                    f"Template={op['template_id']}", "Name=" + op["name"],
                    "NameHex=" + op["name"].encode("utf-16-le").hex(),
                    f"AvatarProfile={profile}", "AvatarSprite=" + sprite]
        files[prefix + "display.ini"] = b"\xff\xfe" + ("\r\n".join(ini)+"\r\n").encode("utf-16-le")
        public_definitions={"schema_version":1,"revision":model.revision(catalog),
                            "client_index_sha256":catalog['client_index_sha256'],
                            "operators":[o for o in catalog['operators'] if o['managed']],
                            "uploaded_assets":catalog['uploaded_assets']}
        for field in ('drop_pools','default_pack_version'):
            if field in catalog:public_definitions[field]=copy.deepcopy(catalog[field])
        files[prefix+'definitions.json']=(json.dumps(public_definitions,ensure_ascii=False,indent=2)+'\n').encode()
        import configparser
        metadata=configparser.ConfigParser(interpolation=None)
        metadata.read_string(files[prefix+"mod.ini"].decode("utf-8-sig"))
        for name in metadata["mod"].get("files","").split(","):
            name=name.strip()
            if name and prefix+name not in files:
                resource=server.MODULE_DIR/name
                if not resource.resolve().is_relative_to(server.MODULE_DIR.resolve()):raise ValueError("模块资源路径越界")
                files[prefix+name]=resource.read_bytes()
        fragment = server.MODULE_DIR / "symbols.fragment.ini"
        if not fragment.exists():
            raise ValueError("客户端模块缺少符号签名片段")
        files["symbols.fragment.ini"] = fragment.read_bytes()
        installer = Path(__file__).with_name("Install-Operator.ps1")
        files["Install-Operator.ps1"] = installer.read_bytes()
        files["registrations.json"] = (json.dumps(model.registrations(catalog),ensure_ascii=False,indent=2)+"\n").encode()
        manifest = {"module": "operator_catalog", "schema_version": 1,
                    "revision": model.revision(catalog), "storefront_changes": False,
                    "host": "GOnline-54BE1568-00C97000",
                    "files": {name:hashlib.sha256(data).hexdigest() for name,data in files.items()}}
        files["manifest.json"] = json.dumps(manifest,indent=2).encode()
        output = io.BytesIO()
        with zipfile.ZipFile(output,"w",zipfile.ZIP_DEFLATED) as z:
            for name,data in files.items():
                z.writestr(name,data)
        return output.getvalue()

    def apply(self, expected):
        catalog = self.check(expected)
        if self.server_ini is None:
            raise ValueError("此助手实例为离线配置模式，尚未连接服务端")
        return server.apply_catalog(catalog,self.server_ini)
