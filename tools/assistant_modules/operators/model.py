"""Portable operator definitions; storefronts consume registrations separately."""
import copy
from decimal import Decimal, InvalidOperation
import hashlib
import json
from pathlib import Path
import re
import threading
from .assets import OP_SIZE, wchar

LOCK = threading.RLock()


def load(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def revision(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    raw = json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    if path.exists() and path.read_text(encoding="utf-8") == raw:
        return
    tmp = path.with_suffix(path.suffix + ".pending")
    with tmp.open("x", encoding="utf-8", newline="\n") as f:
        f.write(raw)
    tmp.replace(path)


def percent_bps(value):
    if isinstance(value, bool):
        raise ValueError("掉落概率必须是百分比数值")
    try:
        bps = Decimal(str(value)) * 100
    except InvalidOperation:
        raise ValueError("掉落概率格式不正确") from None
    if not bps.is_finite() or bps != bps.to_integral_value() or not 0 <= bps <= 10000:
        raise ValueError("概率范围为 0–100%，最多两位小数")
    return int(bps)


def native_category(operator_id):
    if type(operator_id) is not int or not 60001 <= operator_id <= 60999:
        raise ValueError("副官编号必须处于 60001–60999")
    return 1219000 + operator_id - 60000


def validate(catalog):
    if catalog.get("schema_version") != 1:
        raise ValueError("不支持的副官配置版本")
    operators = catalog["operators"]
    ids = set()
    items = {int(x["id"]) for x in catalog["items"]}
    templates = {int(x) for x in catalog["native_operator_ids"]}
    assets = catalog.get("uploaded_assets", {})
    item_defs = {int(x["id"]): x for x in catalog["items"]}
    if sum(op["managed"] for op in operators) > 256:
        raise ValueError("当前客户端模块最多支持 256 个自定义副官配置")
    for op in operators:
        identity = op["id"]
        own_category = native_category(identity)
        category = item_defs.get(op["category_id"])
        valid_category = (category is not None and category["ref_index"] == identity
                          and 1200000 <= op["category_id"] < 1300000)
        if (not valid_category and (own_category != op["category_id"] or category is not None)) or identity in ids:
            raise ValueError("副官编号重复或商品登记编号不一致")
        ids.add(identity)
        if op["template_id"] not in templates:
            raise ValueError("技能/动作模板副官不存在")
        name = op["name"]
        if not isinstance(name, str) or not name.strip() or len(name.encode("utf-16-le")) > 96 or any(ord(ch) < 32 for ch in name):
            raise ValueError("副官名称需为 1–48 个 UTF16 字符且不能包含控制字符")
        if type(op["managed"]) is not bool or type(op["enabled"]) is not bool:
            raise ValueError("启用状态必须是布尔值")
        if op["delivery"] not in ("all_players", "product"):
            raise ValueError("发放方式只能是全员免费或注册商品")
        avatar = op["avatar"]
        if avatar.get("kind") == "native":
            if avatar.get("operator_id") not in templates:
                raise ValueError("头像来源副官不存在")
        elif avatar.get("kind") == "upload":
            if not re.fullmatch(r"[0-9a-f]{64}", avatar.get("sha256", "")) or avatar["sha256"] not in assets:
                raise ValueError("上传头像不存在")
        else:
            raise ValueError("无效头像类型")
        total = 0
        if len(op["drops"]) > 128:
            raise ValueError("单个副官最多配置 128 条掉落规则")
        for drop in op["drops"]:
            if type(drop["category_id"]) is not int or drop["category_id"] not in items or drop["category_id"] == 1070001:
                raise ValueError("掉落物品未注册；不掉落请留出概率余量")
            chance = drop["chance_bps"]
            if type(chance) is not int or not 0 <= chance <= 10000:
                raise ValueError("掉落概率无效")
            total += chance
            lo, hi = drop["min_amount"], drop["max_amount"]
            if type(lo) is not int or type(hi) is not int or not 1 <= lo <= hi <= 65535:
                raise ValueError("掉落数量必须为 1–65535，且最大值不能小于最小值")
        if total > 10000:
            raise ValueError("掉落概率合计不能超过 100%；余量代表不掉落")
    if sum(op["managed"] and op["enabled"] and op["delivery"] == "all_players" for op in operators) > 100:
        raise ValueError("全员免费副官不能超过原版 100 个仓位")


def import_catalog(snapshot, assets):
    root = Path(snapshot) / "GOnlineGame"
    source = load(root / "dbo.tbl_OperatorData.json")
    drops = load(root / "dbo.tbl_OperatorDropDice.json")
    items = load(root / "dbo.tbl_ItemCategory.json")
    records = assets.records("gdrs/0255.gdt", OP_SIZE)
    sidecar=assets.root/'sdgo/mods/operator_catalog/definitions.json'
    saved=load(sidecar) if sidecar.exists() else None
    saved_ops={}
    if saved:
        if saved.get('schema_version')!=1 or saved.get('client_index_sha256')!=assets.index_hash:
            raise ValueError('已安装副官配置与客户端资源包不匹配')
        saved_ops={o['id']:o for o in saved['operators']}
        if len(saved_ops)!=len(saved['operators']):raise ValueError('已安装副官配置编号重复')
    server_ids={int(r['OperatorID']) for r in source}
    if not set(records)<=server_ids or server_ids-set(records)-set(saved_ops):
        raise ValueError("客户端和服务端副官列表不一致")
    published=root/'dbo.sdgo_OperatorProfiles.json'
    if published.exists() and saved:
        for row in load(published):
            if row['OperatorID'] in saved_ops and row['Enabled'] and row['Revision']!=saved['revision']:
                raise ValueError('已安装副官配置不是当前服务端版本，请先更新客户端配置')
    taken = {int(r["CatalogueID"]): int(r["RefIndex"]) for r in items}
    catalog = {"schema_version": 1, "native_operator_ids": sorted(records),
               "client_index_sha256": assets.index_hash, "uploaded_assets": saved.get('uploaded_assets',{}) if saved else {},
               "items": [{"id": int(r["CatalogueID"]), "name": str(r["Name"]).split("\0")[0],
                          "ref_index": int(r["RefIndex"]), "time_limit": int(r["TimeLimit"])} for r in items],
               "native_server_data": {str(r["OperatorID"]): r for r in source}, "operators": []}
    for r in source:
        identity = int(r["OperatorID"])
        if identity not in records:continue
        category = native_category(identity)
        own=saved_ops.get(identity)
        if category in taken and not (own and own['category_id']==category and taken[category]==identity):
            raise ValueError(f"商品登记编号 {category} 已被其他内容占用")
        existing = [item for item in items if int(item["RefIndex"]) == identity
                    and 1200000 <= int(item["CatalogueID"]) < 1300000]
        if existing:
            category = int(min(existing, key=lambda x: (int(x["TimeLimit"]), int(x["CatalogueID"])))['CatalogueID'])
        rules = []
        for drop in sorted((x for x in drops if x["OperatorNumber"] == identity), key=lambda x: x["Idx"]):
            if drop["CategoryID"] == 1070001:
                continue
            rules.append({"category_id": int(drop["CategoryID"]), "chance_bps": int(drop["Prob"]) * 10,
                          "min_amount": int(drop["Amount"]),
                          "max_amount": int(drop["Amount"]) + max(0, int(drop["Opt"]) - 1)})
        catalog["operators"].append({"id": identity, "category_id": category, "template_id": identity,
            "name": wchar(records[identity], 5, 100), "managed": False, "enabled": True,
            "delivery": "all_players" if identity == 60001 else "product",
            "avatar": {"kind": "native", "operator_id": identity}, "drops": rules})
    for identity,op in saved_ops.items():
        existing=next((x for x in catalog['operators'] if x['id']==identity),None)
        if existing is not None:
            existing.update(copy.deepcopy(op))
        else:
            catalog['operators'].append(copy.deepcopy(op))
    validate(catalog)
    return catalog


def update_operator(catalog, identity, value):
    result = copy.deepcopy(catalog)
    current = next((op for op in result["operators"] if op["id"] == identity), None)
    if current is None:
        raise ValueError("副官不存在")
    allowed = {"name", "enabled", "delivery", "avatar", "drops", "template_id"}
    if set(value) - allowed:
        raise ValueError("包含不允许修改的字段")
    current.update(copy.deepcopy(value))
    current["managed"] = True
    validate(result)
    return result


def create_operator(catalog, template_id):
    result = copy.deepcopy(catalog)
    model = next((op for op in catalog["operators"] if op["id"] == template_id), None)
    if model is None:
        raise ValueError("副官模板不存在")
    identity = max(op["id"] for op in result["operators"]) + 1
    item = copy.deepcopy(model)
    item.update(id=identity, category_id=native_category(identity), name="新副官", managed=True,
                delivery="product", enabled=True)
    result["operators"].append(item)
    validate(result)
    return result, identity


def choose_drop(profile, roll, quantity_roll=0):
    """Reference semantics: mutually exclusive outcomes per native trigger."""
    if not 0 <= roll < 10000:
        raise ValueError("概率采样值越界")
    total = 0
    for rule in profile["drops"]:
        total += rule["chance_bps"]
        if roll < total:
            span = rule["max_amount"] - rule["min_amount"] + 1
            if not 0 <= quantity_roll < span:
                raise ValueError("数量采样值越界")
            return rule["category_id"], rule["min_amount"] + quantity_roll
    return None


def registrations(catalog):
    return {"schema_version": 1, "provider": "operator_catalog", "revision": revision(catalog),
            "items": [{"key": f"operator:{o['id']}", "kind": "operator", "operator_id": o["id"],
                       "category_id": o["category_id"], "display_name": o["name"], "avatar": o["avatar"],
                       "drop_rules": copy.deepcopy(o["drops"]),
                       "registration": "product" if o["delivery"] == "product" else "free_for_all",
                       "enabled": o["enabled"]} for o in catalog["operators"] if o["managed"]],
            "storefront_changes": False}
