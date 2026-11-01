"""Offline capsule authoring and acquisition analysis; never connects to SQL.

The JSON produced here is a proposed deployment format. Runtime consumers and
the native client/server adapter are separate from this configuration tool.
"""
from __future__ import annotations

import collections
import copy
import functools
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    data = json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    if path.exists() and path.read_text(encoding="utf-8") == data:
        return False
    temp = path.with_name(path.name + ".pending")
    with temp.open("x", encoding="utf-8", newline="\n") as f:
        f.write(data)
    temp.replace(path)
    return True


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                     separators=(",", ":"), allow_nan=False).encode()).hexdigest()


@functools.lru_cache(maxsize=48)
def table(root, database, name, optional=False):
    base = Path(root) / database
    files = sorted(base.glob(f"dbo.{name}.part*.json"))
    single = base / f"dbo.{name}.json"
    if single.exists():
        if files:
            raise ValueError(f"Both complete and split exports exist: {name}")
        files = [single]
    if not files and not optional:
        raise ValueError(f"Required configuration table missing: {database}/{name}")
    rows = []
    for path in files:
        raw = path.read_bytes()
        meta_path = Path(root) / "_meta.json"
        if meta_path.exists():
            expected = read_json(meta_path).get("files", {}).get(path.relative_to(root).as_posix())
            if expected and hashlib.sha256(raw).hexdigest() != expected["sha256"]:
                raise ValueError(f"Snapshot configuration changed since export: {name}")
        data = json.loads(raw.decode("utf-8-sig"))
        if not isinstance(data, list):
            raise ValueError(f"Expected a table array: {path}")
        rows.extend(data)
    return rows


def grade(unit):
    return {1: "C", 2: "B", 3: "A", 4: "S"}[int(unit["UnitGradeNumber"])] + {
        1: "-training", 2: "", 3: "S", 4: "R", 5: "U"}[int(unit["UnitGradeNumber2"])]


def eligible(unit):
    return (int(unit.get("SallyReady", 0)) == 1
            and int(unit["UnitGradeNumber"]) in (1, 2, 3, 4)
            and int(unit["UnitGradeNumber2"]) in (2, 3, 4, 5)
            and not (unit.get("UnitModelName", "").strip().lower() == "name"
                     and int(unit.get("UnitGraph", 0)) == 1010001))


class ClientData:
    """Bounded read-only ZDX/ZPK reader; only selected GDT entries are read."""

    def __init__(self, client):
        self.root = Path(client)
        raw = (self.root / "GPackData.zdx").read_bytes()
        version, self.chunk_size, chunks, _, count, _ = struct.unpack_from("<6I", raw)
        if version != 1 or not 0 < self.chunk_size <= 1048576 or count > 1000000:
            raise ValueError("Unsupported ZDX header")
        self.index_hash = hashlib.sha256(raw).hexdigest()
        self.entries = {}
        pos = 24
        for _ in range(count):
            chars = []
            while True:
                c = struct.unpack_from("<H", raw, pos)[0] ^ 0xAA
                pos += 2
                if c == 0:
                    break
                chars.append(chr(c))
                if len(chars) > 4096:
                    raise ValueError("Oversized archive path")
            size, start, length = struct.unpack_from("<III", raw, pos)
            pos += 36
            key = "".join(chars).replace("\\", "/").lower()
            if key in self.entries:
                raise ValueError("Duplicate archive entry")
            self.entries[key] = (size, start, length)

    def read(self, name):
        # Exact known relative paths only; no archive filenames are materialized.
        key = name.replace("\\", "/").lower()
        if key not in {"gdrs/0033.gdt", "gdrs/0154.gdt", "gdrs/0155.gdt"}:
            raise ValueError("Entry outside capsule inspection scope")
        return self._read(key)

    def read_resource(self, name):
        """Read a numeric game resource; no supplied paths are extracted."""
        import re
        key = name.replace("\\", "/").lower()
        if key != "uirs/1161.uir" and not re.fullmatch(r"(?:gdrs/[0-9]+\.gdt|sprs/[0-9a-z_-]+\.spr|txrs/[0-9a-z_-]+\.txr)", key):
            raise ValueError("Invalid game resource name")
        return self._read(key)

    def _read(self, key):
        loose = self.root.joinpath(*key.split("/"))
        if loose.exists():
            return loose.read_bytes()
        size, cursor, length = self.entries[key]
        if size > 16777216 or length > 100000:
            raise ValueError("Capsule entry exceeds inspection bound")
        data, seen = bytearray(), set()
        with (self.root / "GPackData.zpk").open("rb") as f:
            for _ in range(length):
                if cursor in seen:
                    raise ValueError("Cyclic archive chunk chain")
                seen.add(cursor)
                f.seek(cursor * (self.chunk_size + 8))
                block = f.read(self.chunk_size + 8)
                if len(block) != self.chunk_size + 8:
                    raise ValueError("Truncated archive chunk")
                cursor = struct.unpack_from("<I", block)[0]
                data.extend(block[8:])
        data = bytes(data[:size])
        if data.startswith(b"ZOAGZIP1"):
            expected = struct.unpack_from("<I", data, 8)[0]
            if expected > 16777216:
                raise ValueError("Oversized decoded capsule entry")
            stream = zlib.decompressobj(31)
            result = stream.decompress(data[12:], expected + 1)
            if len(result) != expected or not stream.eof:
                raise ValueError("Invalid capsule gzip payload")
            return result
        return data


def gdt_records(data):
    if data[:2] != b"\xf9\xf8":
        raise ValueError("Capsule adapter currently supports UTF16LE encrypted GDT only")
    size, count, extra = struct.unpack_from("<III", data, 2)
    if not 0 < size <= 4096 or count > 100000:
        raise ValueError("Unsupported GDT dimensions")
    key_size = (size + count + extra) % 65 + 192
    key = data[14:14 + key_size]
    if len(key) != key_size or max(key) >= key_size:
        raise ValueError("Invalid GDT permutation key")
    stride = math.ceil(size / key_size) * key_size
    if len(data) != 14 + key_size + stride * count:
        raise ValueError("Truncated or trailing GDT data")
    rows = []
    for index in range(count):
        start = 14 + key_size + index * stride
        decoded = bytearray()
        for offset in range(0, stride, key_size):
            block = data[start + offset:start + offset + key_size]
            decoded.extend(block[key[i]] ^ key[(768 + 3 * i) % key_size]
                           for i in range(key_size))
        rows.append(bytes(decoded[:size]))
    return size, rows


def capsule_display(client):
    reader = ClientData(client)
    raw = reader.read("gdrs/0033.gdt")
    size, rows = gdt_records(raw)
    if size != 1019:
        raise ValueError("Unknown fixed capsule GDT record layout")
    machines = []
    for row in rows:
        count = row[11]
        if count != 6:
            raise ValueError("This fixed-capsule importer expects six slots")
        slots = []
        for i in range(count):
            sprites = [row[59 + (2 * i + j) * 60:59 + (2 * i + j + 1) * 60]
                       .decode("utf-16-le").split("\0")[0] for j in range(2)]
            slots.append({"unit_id": struct.unpack_from("<I", row, 27 + 4 * i)[0],
                          "sprites": sprites})
        machines.append({"id": struct.unpack_from("<I", row, 5)[0],
                         "grid_id": struct.unpack_from("<I", row)[0], "slots": slots})
    return machines, {"zdx_sha256": reader.index_hash,
                      "fixed_gdt_sha256": hashlib.sha256(raw).hexdigest(),
                      "custom_gdt_sha256": hashlib.sha256(reader.read("gdrs/0154.gdt")).hexdigest(),
                      "custom_candidates_gdt_sha256": hashlib.sha256(reader.read("gdrs/0155.gdt")).hexdigest()}


GRADE_SETS = [[2], [5], [4], [3], [2, 5], [2, 4], [2, 3], [5, 4],
              [5, 3], [4, 3], [2, 5, 4], [2, 5, 3], [2, 4, 3], [5, 4, 3], [2, 5, 4, 3]]


def import_catalog(snapshot, client):
    units_raw = table(snapshot, "GOnlineGame", "tbl_Units")
    units = {int(u["UnitNumber"]): u for u in units_raw}
    recipes = table(snapshot, "GOnlineGame", "tbl_MixTreeFile")
    recipe_outputs = {int(r["OutUnitID"]) for r in recipes}
    raw_custom_infos = table(snapshot, "GOnlineGame", "tbl_CustomCapsuleInfo")
    native_custom_candidate_unit_ids = sorted({int(r["UnitID"]) for r in raw_custom_infos})
    categories = table(snapshot, "GOnlineGame", "tbl_ItemCategory")
    inventory_ids = {int(c["RefIndex"]) for c in categories
                     if int(c["CatalogueID"]) // 100000 == 11 and not int(c["TimeLimit"])}
    display, identity = capsule_display(client)
    meta = Path(snapshot) / "_meta.json"
    if meta.exists() and "files" in read_json(meta):
        identity["snapshot_fingerprint"] = digest(read_json(meta)["files"])
    sprites = {slot["unit_id"]: slot["sprites"] for machine in display for slot in machine["slots"]}
    gp = {int(r["MachineID"]): r for r in table(snapshot, "GOnlineGame", "tbl_MachinePrice")}
    dice = collections.defaultdict(dict)
    for row in table(snapshot, "GOnlineGame", "tbl_MachineDice"):
        dice[int(row["MachineID"])][int(row["UnitID"])] = row
    pool = collections.defaultdict(collections.Counter)
    for row in table(snapshot, "GOnlineGame", "tbl_CapsuleList1"):
        pool[int(row["MachineID"])][int(row["UnitID"])] += 1
    catalog = {"schema_version": 1, "status": "authoring-only", "source_identity": identity,
               "custom_candidate_policy": {
                   "version": 1,
                   "native_candidate_unit_ids": native_custom_candidate_unit_ids,
                   "new_candidate_grades": ["SR", "SS-without-blueprint-recipe"]},
               "units": [{"id": uid, "name": u["UnitModelName"].split("\0")[0],
                          "grade": grade(u), "rank": int(u["UnitGradeNumber"]),
                          "subgrade": int(u["UnitGradeNumber2"]), "eligible": eligible(u),
                           "has_blueprint_recipe": uid in recipe_outputs,
                          "transform_to": int(u["UnitNumber_Transform"]),
                          "collection": bool(u.get("CollectionUnit", 0)),
                          "has_inventory_category": uid in inventory_ids,
                          "capsule_sprites": sprites.get(uid, []),
                          "review_placeholder": u["UnitModelName"].strip().lower() == "name"
                                                and int(u["UnitGraph"]) == 1010001}
                         for uid, u in sorted(units.items())],
               "fixed_machines": [], "custom_machines": []}
    for entry in display:
        mid = entry["id"]
        price = gp.get(mid)
        if price is None:
            raise ValueError(f"Client machine {mid} has no server price configuration")
        slots = []
        for slot in entry["slots"]:
            uid = slot["unit_id"]
            if uid not in units:
                raise ValueError(f"Client machine references unknown server unit {uid}")
            source = dice[mid].get(uid)
            if source is None:
                raise ValueError(f"No authoritative dice row for {mid}/{uid}")
            slots.append({**slot, "weight": int(source["Prob1"]),
                          "native_draw_group": int(source["UnitGradeNumber"]),
                          "expanded_entries": pool[mid][uid]})
        catalog["fixed_machines"].append({"id": mid, "grid_id": entry["grid_id"],
            "name": f"Capsule {mid // 100}", "enabled": not bool(price["Disable"]),
            "currency": "GP", "price": int(price["Price"]), "owner": "native",
            "draw_policy": "native_sequence", "slots": slots})
    infos = []
    info_keys = {}
    for info in raw_custom_infos:
        key = (int(info["MachineID"]), int(info["UnitID"]))
        if key in info_keys:
            if info_keys[key] != info:
                raise ValueError(f"Conflicting custom candidate definitions: {key}")
            catalog.setdefault("source_warnings", []).append(f"Identical duplicate custom row collapsed: {key}")
        else:
            info_keys[key] = info
            infos.append(info)
    for row in table(snapshot, "GOnlineGame", "tbl_CustomCapsuleList"):
        mid = int(row["MachineID"])
        catalog["custom_machines"].append({"id": mid, "name": row["MachineTitle"],
            "enabled": True, "owner": "native", "currency_type": int(row["CostType"]),
            "price": int(row["BasicCost"]), "draw_policy": "native_mileage",
            "slot_rules": [{"slot": i, "rank_max": int(row[f"Rank{i}"]),
                            "subgrades": GRADE_SETS[int(row[f"Grade{i}"])],
                            "extension": i >= 7} for i in range(1, 9)],
            "candidates": [{"unit_id": int(r["UnitID"]), "basic_weight": int(r["BasicProb"]),
                            "mileage": float(r["Mileage"]), "extra_price": int(r["AdditionalCost"]),
                            "reset_mileage": bool(r["MileageResetFlag"])}
                           for r in infos if int(r["MachineID"]) == mid]})
    # Custom candidates, prices and rank rules are independently present in the
    # shipped client. Refuse to initialize from mismatched server/client data.
    reader = ClientData(client)
    size, records = gdt_records(reader.read("gdrs/0155.gdt"))
    if size != 18:
        raise ValueError("Unknown custom candidate GDT record layout")
    client_candidates = {(struct.unpack_from("<I", r, 5)[0], struct.unpack_from("<I", r, 9)[0])
                         for r in records}
    if client_candidates != set(info_keys):
        raise ValueError("Client/server custom candidate definitions differ")
    size, records = gdt_records(reader.read("gdrs/0154.gdt"))
    if size != 231:
        raise ValueError("Unknown custom machine GDT record layout")
    client_machines = {struct.unpack_from("<I", r, 5)[0]: r for r in records}
    server_machines = table(snapshot, "GOnlineGame", "tbl_CustomCapsuleList")
    if set(client_machines) != {int(m["MachineID"]) for m in server_machines}:
        raise ValueError("Client/server custom machine lists differ")
    for m in server_machines:
        row = client_machines[int(m["MachineID"])]
        expected_rules = bytes(v for i in range(1, 9) for v in (int(m[f"Rank{i}"]), int(m[f"Grade{i}"])))
        if (struct.unpack_from("<H", row, 9)[0] != int(m["CostType"])
            or struct.unpack_from("<I", row, 11)[0] != int(m["BasicCost"])
            or row[115:131] != expected_rules):
            raise ValueError("Client/server custom prices or slot rules differ")
    validate(catalog)
    return catalog


def custom_candidate_allowed(catalog, unit):
    """Whether a unit may appear under the source-bound custom policy.

    Catalogues created before the policy metadata existed retain their prior
    validation behavior. Native baseline candidates remain usable; other units
    require SR, or SS with explicit proof that no recipe exists. Imported
    catalogues carry recipe-presence metadata, so a temporarily unavailable
    blueprint cannot be mistaken for no recipe at all.
    """
    policy = catalog.get("custom_candidate_policy")
    if policy is None:
        return True
    if unit.get("id") in policy.get("native_candidate_unit_ids", []):
        return True
    if unit["rank"] == 4 and unit["subgrade"] == 4:
        return True
    return (unit["rank"] == 4 and unit["subgrade"] == 3
            and unit.get("has_blueprint_recipe") is False)


def native_capsule_card_available(unit):
    """Whether an exact native shown/hidden card pair is available."""
    sprites = unit.get("capsule_sprites")
    return (isinstance(sprites, list) and len(sprites) == 2
            and all(isinstance(path, str) and path.strip() for path in sprites)
            and sprites[0].lower() != sprites[1].lower())


def validate(catalog):
    if catalog.get("schema_version") != 1:
        raise ValueError("Unsupported catalog schema")
    units = {u["id"]: u for u in catalog["units"]}
    if len(units) != len(catalog["units"]):
        raise ValueError("Duplicate unit ID")
    policy = catalog.get("custom_candidate_policy")
    if policy is not None:
        if (not isinstance(policy, dict) or policy.get("version") != 1
            or not isinstance(policy.get("native_candidate_unit_ids"), list)):
            raise ValueError("Unsupported custom candidate policy")
        native_ids = policy["native_candidate_unit_ids"]
        if (len(set(native_ids)) != len(native_ids)
            or any(type(uid) is not int or uid not in units for uid in native_ids)):
            raise ValueError("Invalid native custom candidate unit list")
    for kind in ("fixed_machines", "custom_machines"):
        ids = set()
        for machine in catalog[kind]:
            mid = machine["id"]
            if type(mid) is not int or mid <= 0 or mid in ids:
                raise ValueError("Invalid or duplicate machine ID")
            ids.add(mid)
            if type(machine.get("enabled")) is not bool:
                raise ValueError("Machine enabled must be a boolean")
            if type(machine["price"]) is not int or machine["price"] < 0:
                raise ValueError("Price must be a nonnegative integer")
            entries = machine.get("slots", machine.get("candidates", []))
            selected = [e["unit_id"] for e in entries]
            if len(set(selected)) != len(selected) or any(uid not in units for uid in selected):
                raise ValueError("Duplicate or unknown unit in machine")
            if kind == "fixed_machines":
                if len(entries) != 6 or any(type(s["weight"]) is not int or s["weight"] <= 0 for s in entries):
                    raise ValueError("Fixed machines require six positive integer weights")
                if sum(s["weight"] for s in entries) != 1000:
                    raise ValueError("Fixed weights must total 1000")
            else:
                rules = machine["slot_rules"]
                if [r["slot"] for r in rules] != list(range(1, 9)):
                    raise ValueError("Custom machines require eight ordered slot rules")
                for item in entries:
                    u = units[item["unit_id"]]
                    if not any(u["rank"] <= r["rank_max"] and u["subgrade"] in r["subgrades"] for r in rules):
                        raise ValueError("Custom candidate has no permitted slot")
                    if not custom_candidate_allowed(catalog, u):
                        raise ValueError(
                            "Custom candidate policy permits only native baseline units, "
                            "SR, or SS without any blueprint recipe")
                    if (type(item["basic_weight"]) is not int or item["basic_weight"] <= 0
                        or not math.isfinite(item["mileage"]) or item["mileage"] < 0
                        or type(item["extra_price"]) is not int or item["extra_price"] < 0):
                        raise ValueError("Invalid custom weight, mileage or price")


def patterns(catalog):
    units = {u["id"]: u for u in catalog["units"]}
    groups = collections.defaultdict(list)
    for m in catalog["fixed_machines"]:
        groups[tuple(units[s["unit_id"]]["grade"] for s in m["slots"])].append(m)
    return [{"grades": list(k), "machines": [m["id"] for m in v],
             "weight_variants": sorted({tuple(s["weight"] for s in m["slots"]) for m in v})}
            for k, v in sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0]))]


def acquisition(snapshot, catalog, additional=()):
    """Least fixed point over actual sources and AND/OR blueprint requirements.

    Possessing a recipe record is insufficient: its item must have a source.
    Rental products are not permanent roots. Cycles do not create a root.
    """
    units = {u["id"]: u for u in catalog["units"]}
    cats = {int(r["CatalogueID"]): r for r in table(snapshot, "GOnlineGame", "tbl_ItemCategory")}
    recipes = table(snapshot, "GOnlineGame", "tbl_MixTreeFile")
    recipe_ids = {int(r["Idx"]) for r in recipes}
    known, plans, done_quests = {}, {}, set()
    for kind in ("fixed_machines", "custom_machines"):
        for m in catalog[kind]:
            if m.get("enabled", True):
                for s in m.get("slots", m.get("candidates", [])):
                    known.setdefault(s["unit_id"], {"source": kind, "machine_id": m["id"]})
    # Enabled coin variants can contain units absent from the GP machine.
    coin_enabled = {int(r["MachineID"]) for r in table(snapshot, "GOnlineGame", "tbl_CoinMachinePrice")
                    if not r["Disable"]}
    visible_coin = {m["id"] + 1 for m in catalog["fixed_machines"] if not m.get("retired", False)}
    active_coin = coin_enabled & visible_coin
    for r in table(snapshot, "GOnlineGame", "tbl_CoinCapsuleList"):
        if int(r["MachineID"]) in active_coin:
            known.setdefault(int(r["UnitID"]), {"source": "coin_capsule", "machine_id": int(r["MachineID"])})
    for uid in additional:
        known[int(uid)] = {"source": "initialization"}

    def grant(category, reason):
        c = cats.get(int(category))
        if c is None or int(c["TimeLimit"]) != 0:
            return
        ref = int(c["RefIndex"])
        # Catalogue IDs are product categories, not recipe IDs. In the native
        # dataset plans use 154xxxx categories whose RefIndex is 200xxx.
        if ref in recipe_ids:
            plans.setdefault(ref, reason)
        elif int(category) // 100000 == 11 and ref in units:
            known.setdefault(ref, reason)

    products = table(snapshot, "GDStoreBilling", "tbls_StoreItems")
    visible_items = {str(r["f_itemid"]).strip() for r in products
                     if str(r["f_display"]).strip() == "1" and int(r["f_depth0"]) > 1
                     and int(r["f_quantity"]) != 0}
    for r in table(snapshot, "GDStoreBilling", "tbls_SubItems"):
        if str(r["f_itemid"]).strip() in visible_items and str(r["f_display"]).strip() == "1":
            grant(r["f_categoryNumber"], {"source": "shop", "item_id": str(r["f_itemid"]).strip()})
    drop_machines = set()
    for row in table(snapshot, "GOnlineGame", "tbl_BattleResultItemDropSetting"):
        drop_machines.update(int(row[k]) for k in ("NormalMachineID", "RareMachineID", "UniqueMachineID") if row[k])
    for row in table(snapshot, "GOnlineGame", "tbl_BattleResultItemDropDice"):
        if int(row["ItemMachineID"]) in drop_machines and float(row["Prob"]) > 0:
            grant(row["CategoryID"], {"source": "configured_battle_drop", "drop_machine_id": int(row["ItemMachineID"])})
    quests = table(snapshot, "GOnlineGame", "tbl_QuestInfo")
    deps = collections.defaultdict(set)
    for r in table(snapshot, "GOnlineGame", "tbl_QuestPrerequest"):
        deps[int(r["QuestID"])].add(int(r["PreQuest"]))
    skipped_quests = []
    while True:
        before = (len(known), len(plans), len(done_quests))
        for q in quests:
            qid = int(q["QuestID"])
            # UnitMode's condition language has not been reconstructed: do not
            # treat conditional collection quests as unconditional free units.
            if (qid in done_quests or not q["Activation"] or int(q["UnitMode"]) != 0
                or not deps[qid] <= done_quests):
                continue
            done_quests.add(qid)
            if int(q["RewardType"]) == 1 and int(q["RewardCount"]) > 0:
                grant(q["Reward"], {"source": "configured_quest", "quest_id": qid})
        for r in recipes:
            rid, out = int(r["Idx"]), int(r["OutUnitID"])
            materials = [int(r[f"UnitID{i}"]) for i in range(1, 8) if int(r[f"UnitID{i}"]) > 0]
            if rid in plans and materials and all(uid in known for uid in materials):
                known.setdefault(out, {"source": "blueprint", "recipe_id": rid, "materials": materials})
        if before == (len(known), len(plans), len(done_quests)):
            break
    for q in quests:
        if q["Activation"] and int(q["QuestID"]) not in done_quests:
            skipped_quests.append(int(q["QuestID"]))
    blocked = []
    for r in recipes:
        if int(r["OutUnitID"]) not in known:
            materials = [int(r[f"UnitID{i}"]) for i in range(1, 8) if int(r[f"UnitID{i}"]) > 0]
            blocked.append({"recipe_id": int(r["Idx"]), "output": int(r["OutUnitID"]),
                            "blueprint_available": int(r["Idx"]) in plans,
                            "missing_materials": [uid for uid in materials if uid not in known]})
    # A transformation is access to a model state, not a second inventory item.
    # Keep these separate so a transformed model cannot become a recipe material.
    states = set(known)
    while True:
        next_states = {units[uid].get("transform_to", 0) for uid in states if uid in units}
        next_states = {uid for uid in next_states if uid in units} - states
        if not next_states:
            break
        states.update(next_states)
    forms = sorted(states - set(known))
    return {"known": known, "forms": forms, "blueprints": plans, "blocked_recipes": blocked,
            "unresolved_quests": skipped_quests,
            "limitations": ["Configuration reachability, not gameplay acceptance.",
                "UnitMode collection quests need a separately verified condition adapter.",
                "Configured quest/drop unlock conditions have not been exercised in game.",
                "No account state, event rewards or admin grants are assumed."]}


def initialize(snapshot, catalog, price=None):
    if price is not None and (type(price) is not int or price < 0):
        raise ValueError("Initialization price override must be a nonnegative integer or null")
    validate(catalog)
    catalog = copy.deepcopy(catalog)
    units = {u["id"]: u for u in catalog["units"]}
    initial = acquisition(snapshot, catalog)
    # Seed missing roots in ascending base rank; re-evaluate all recipe paths
    # after every root to avoid seeding things newly craftable from earlier ones.
    seeds, available = [], initial
    for u in sorted(units.values(), key=lambda u: (u["rank"], not u.get("has_inventory_category", False),
                                                  not u.get("collection", False),
                                                  "AFTER" in u["name"].upper(), u["id"])):
        if u["eligible"] and u["id"] not in available["known"] and u["id"] not in available["forms"]:
            seeds.append(u["id"])
            available = acquisition(snapshot, catalog, seeds)
    # Remove redundant roots after later additions have unlocked more recipes.
    # This is deterministic inclusion-minimal coverage, not a global set-cover
    # optimum. Check all target units, because forms are not recipe materials.
    targets = {u["id"] for u in units.values() if u["eligible"]}
    for uid in list(reversed(seeds)):
        trial = [other for other in seeds if other != uid]
        reach = acquisition(snapshot, catalog, trial)
        if targets <= set(reach["known"]) | set(reach["forms"]):
            seeds = trial
    pending = set(seeds)
    baseline = [m for m in catalog["fixed_machines"] if m["owner"] == "native" and m["enabled"]]
    occupied = {m["id"] for m in catalog["fixed_machines"]}
    occupied.update(int(r["MachineID"]) for r in table(snapshot, "GOnlineGame", "tbl_MachinePrice"))
    next_id = (max(occupied, default=0) // 100 + 1) * 100 + 1
    new_machines = []
    derived_machines = []
    fixed_machine_mix = []
    filler_usage = collections.Counter()
    baseline_known = {uid for uid in initial["known"] if uid in units}
    configured_fixed_units = set()
    max_new_roots_per_machine = 4

    def add_fixed_machines(derived=False):
        nonlocal next_id
        while pending:
            choices = []
            for model in baseline:
                slots, used, covered, derived_units = [], set(), [], []
                baseline_fillers, initialized_fillers = [], []
                for slot_index, ref in enumerate(model["slots"], 1):
                    ref_unit = units[ref["unit_id"]]
                    if derived and len(covered) < max_new_roots_per_machine:
                        candidates = [uid for uid in sorted(pending)
                                      if uid not in used
                                      and ref_unit["rank"] <= 3
                                      and ref_unit["subgrade"] == 2
                                      and units[uid]["rank"] == ref_unit["rank"]
                                      and units[uid]["subgrade"] == 3
                                      and native_capsule_card_available(units[uid])]
                    elif not derived and len(covered) < max_new_roots_per_machine:
                        candidates = [uid for uid in sorted(pending)
                                      if uid not in used
                                      and units[uid]["grade"] == ref_unit["grade"]]
                    else:
                        candidates = []
                    if candidates:
                        uid = candidates[0]
                        covered.append(uid)
                        if derived:
                            derived_units.append(uid)
                        role = "new_root"
                    else:
                        available_fillers = baseline_known | configured_fixed_units

                        def filler_matches(uid):
                            candidate = units[uid]
                            if candidate["grade"] == ref_unit["grade"]:
                                return True
                            return (derived and ref_unit["rank"] <= 3
                                    and ref_unit["subgrade"] == 2
                                    and candidate["rank"] == ref_unit["rank"]
                                    and candidate["subgrade"] == 3
                                    and native_capsule_card_available(candidate))

                        fillers = [uid for uid in available_fillers
                                   if uid not in used and filler_matches(uid)]
                        # Every generated machine retains at least one genuinely
                        # baseline unit. Other filler slots may rotate through
                        # roots that earlier generated machines actually added.
                        if not baseline_fillers:
                            fillers = [uid for uid in fillers if uid in baseline_known]
                        fillers.sort(key=lambda uid: (filler_usage[uid], uid))
                        if not fillers:
                            break
                        uid = fillers[0]
                        if uid in baseline_known:
                            role = "filler_baseline"
                            baseline_fillers.append(uid)
                        else:
                            role = "filler_initialized"
                            initialized_fillers.append(uid)
                    used.add(uid)
                    slot = {"unit_id": uid, "weight": ref["weight"],
                            "native_draw_group": ref["native_draw_group"],
                            "sprites": units[uid].get("capsule_sprites", []),
                            "template_unit_id": ref["unit_id"],
                            "template_grade": ref_unit["grade"],
                            "template_slot": slot_index,
                            "template_sprites": ref.get("sprites", []),
                            "initialization_role": role}
                    if role != "new_root":
                        slot["filler_usage_before"] = filler_usage[uid]
                    slots.append(slot)
                if len(slots) == 6 and covered:
                    choices.append((len(covered), -model["id"], model, slots,
                                    covered, derived_units, baseline_fillers,
                                    initialized_fillers))
            if not choices:
                break
            (_, _, model, slots, covered, derived_units, baseline_fillers,
             initialized_fillers) = max(
                choices, key=lambda x: (x[0], x[1]))
            while next_id in occupied or next_id + 1 in occupied:
                next_id += 100
            machine_price = model["price"] if price is None else price
            pricing_source = "template" if price is None else "override"
            m = {"id": next_id, "name": f"Acquisition {len(new_machines) + 1}",
                 "enabled": True, "owner": "initialization", "currency": "GP",
                 "price": machine_price, "pricing_source": pricing_source,
                 "template_price": model["price"],
                 "draw_policy": "independent_weighted",
                 "template_id": model["id"],
                 "template_match": "base_rank_derived" if derived else "exact_grade",
                 "initialization_mix": {
                     "new_root_limit": max_new_roots_per_machine,
                     "new_root_unit_ids": list(covered),
                     "baseline_filler_unit_ids": list(baseline_fillers),
                     "initialized_filler_unit_ids": list(initialized_fillers)},
                 "slots": slots}
            if derived_units:
                m["derived_template_units"] = sorted(derived_units)
                derived_machines.append(next_id)
            catalog["fixed_machines"].append(m)
            new_machines.append(next_id)
            fixed_machine_mix.append({
                "machine_id": next_id,
                "template_id": model["id"],
                "template_price": model["price"],
                "price": machine_price,
                "pricing_source": pricing_source,
                **m["initialization_mix"]})
            for slot in slots:
                if slot["initialization_role"] != "new_root":
                    filler_usage[slot["unit_id"]] += 1
            configured_fixed_units.update(covered)
            pending.difference_update(covered)
            occupied.add(next_id)
            next_id += 100

    add_fixed_machines()
    # AS/BS/CS have no stock exact-grade patterns. Treat them as their base
    # A/B/C slot only for rank <= 3, preserving the native slot's weight, draw
    # group, position and artwork role. Rank-4 S/SS never use this fallback.
    add_fixed_machines(derived=True)

    # New high-rank custom candidates are restricted to SR and SS for which no
    # recipe record exists at all. Blueprint visibility and material
    # reachability do not turn a recipe-bearing SS into a custom candidate.
    recipe_rows = table(snapshot, "GOnlineGame", "tbl_MixTreeFile")
    recipes_by_output = collections.defaultdict(list)
    for row in recipe_rows:
        recipes_by_output[int(row["OutUnitID"])].append(int(row["Idx"]))
    blocked_by_output = collections.defaultdict(list)
    for row in initial["blocked_recipes"]:
        blocked_by_output[row["output"]].append(row)
    enforce_custom_policy = "custom_candidate_policy" in catalog
    exclusion_explanations = {
        "ordinary_s_not_custom_eligible":
            "Ordinary S units are not eligible for new custom-capsule entries.",
        "ss_has_blueprint_recipe":
            "This SS unit has a recipe record, even though its blueprint or materials may be unavailable.",
        "missing_native_capsule_card":
            "No exact native shown/hidden capsule-card sprite pair is available.",
        "no_native_base_rank_fixed_template":
            "No native fixed-capsule base-rank slot template can place this unit.",
        "custom_candidate_policy_excludes_grade":
            "This grade is not eligible for a new custom-capsule entry.",
        "no_compatible_custom_machine_template":
            "No enabled custom machine has compatible slot and pricing rules.",
    }
    custom_added = []
    custom_exclusions = []
    for uid in sorted(pending):
        u = units[uid]
        allowed_by_policy = custom_candidate_allowed(catalog, u)
        if u["rank"] == 4 and u["subgrade"] == 3 and uid in recipes_by_output:
            allowed_by_policy = False
        if enforce_custom_policy and not allowed_by_policy:
            if u["rank"] == 4 and u["subgrade"] == 2:
                reason = "ordinary_s_not_custom_eligible"
            elif u["rank"] == 4 and u["subgrade"] == 3 and uid in recipes_by_output:
                reason = "ss_has_blueprint_recipe"
            elif u["rank"] <= 3 and u["subgrade"] == 3:
                reason = ("no_native_base_rank_fixed_template"
                          if native_capsule_card_available(u)
                          else "missing_native_capsule_card")
            else:
                reason = "custom_candidate_policy_excludes_grade"
            custom_exclusions.append({
                **u, "unit_id": uid, "reason": reason,
                "explanation": exclusion_explanations[reason],
                "recipe_ids": sorted(recipes_by_output.get(uid, [])),
                "blocked_recipes": blocked_by_output.get(uid, [])})
            continue
        inserted = False
        for m in catalog["custom_machines"]:
            if not m["enabled"] or not any(u["rank"] <= r["rank_max"] and u["subgrade"] in r["subgrades"] for r in m["slot_rules"]):
                continue
            candidates = [r for r in m["candidates"] if units[r["unit_id"]]["rank"] == u["rank"]]
            if not candidates:
                continue
            same = [r for r in candidates if units[r["unit_id"]]["subgrade"] == u["subgrade"]]
            archetypes = collections.Counter((r["basic_weight"], r["mileage"], r["extra_price"], r["reset_mileage"])
                                             for r in (same or candidates))
            weight, mileage, extra, reset = archetypes.most_common(1)[0][0]
            m["candidates"].append({"unit_id": uid, "basic_weight": weight, "mileage": mileage,
                                    "extra_price": extra, "reset_mileage": reset})
            custom_added.append({"machine_id": m["id"], "unit_id": uid})
            inserted = True
        if not inserted:
            if enforce_custom_policy:
                custom_exclusions.append({
                    **u, "unit_id": uid,
                    "reason": "no_compatible_custom_machine_template",
                    "explanation": exclusion_explanations[
                        "no_compatible_custom_machine_template"],
                    "recipe_ids": sorted(recipes_by_output.get(uid, [])),
                    "blocked_recipes": blocked_by_output.get(uid, [])})
            else:
                raise ValueError(f"No supported acquisition template for unit {uid}")
    validate(catalog)
    final = acquisition(snapshot, catalog)
    remaining = sorted(u["id"] for u in units.values()
                       if u["eligible"] and u["id"] not in final["known"]
                       and u["id"] not in final["forms"])
    if remaining and not enforce_custom_policy:
        raise ValueError(f"Initialization left unreachable eligible units: {remaining}")
    exclusion_by_unit = {row["unit_id"]: row for row in custom_exclusions}
    transform_links = collections.defaultdict(set)
    for u in units.values():
        target = u.get("transform_to", 0)
        if target in units:
            transform_links[u["id"]].add(target)
            transform_links[target].add(u["id"])
    transform_blocker = {}
    for blocked_uid in sorted(exclusion_by_unit):
        frontier = [blocked_uid]
        seen = {blocked_uid}
        while frontier:
            current = frontier.pop()
            for linked in transform_links[current] - seen:
                seen.add(linked)
                frontier.append(linked)
                transform_blocker.setdefault(linked, blocked_uid)
    remaining_details = []
    for uid in remaining:
        if uid in exclusion_by_unit:
            remaining_details.append(exclusion_by_unit[uid])
        elif uid in transform_blocker:
            blocked_uid = transform_blocker[uid]
            remaining_details.append({
                **units[uid], "unit_id": uid,
                "reason": "transform_family_depends_on_excluded_root",
                "explanation":
                    "This transform state has no source because its linked root was excluded.",
                "blocked_by_unit_id": blocked_uid,
                "blocked_reason": exclusion_by_unit[blocked_uid]["reason"]})
        else:
            remaining_details.append({
                **units[uid], "unit_id": uid,
                "reason": "no_configured_source_after_initialization",
                "explanation":
                    "No fixed capsule, custom capsule, complete recipe path, or transform root was configured."})
    remaining_by_unit = {row["unit_id"]: row for row in remaining_details}
    suggested_seeds = [{
        **units[uid], "unit_id": uid,
        "reason": "no configured source or complete blueprint path"}
        for uid in seeds]
    configured_seeds = []
    unconfigured_seeds = []
    for uid in seeds:
        if uid in final["known"]:
            configured_seeds.append({
                **units[uid], "unit_id": uid,
                "reason": "configured by initialization",
                "configured_source": final["known"][uid]})
        elif uid in final["forms"]:
            configured_seeds.append({
                **units[uid], "unit_id": uid,
                "reason": "available as a transform state after initialization"})
        else:
            unconfigured_seeds.append(remaining_by_unit.get(uid, {
                **units[uid], "unit_id": uid,
                "reason": "no_configured_source_after_initialization",
                "explanation":
                    "No fixed capsule, custom capsule, complete recipe path, or transform root was configured."}))
    report = {"seeds": configured_seeds,
              "suggested_seeds": suggested_seeds,
              "unconfigured_seeds": unconfigured_seeds,
              "new_fixed_machine_ids": new_machines,
              "derived_fixed_machine_ids": derived_machines,
              "fixed_machine_mix": fixed_machine_mix,
              "fixed_filler_usage": [
                  {"unit_id": uid, "grade": units[uid]["grade"], "uses": count}
                  for uid, count in sorted(filler_usage.items())],
              "custom_candidate_additions": custom_added,
              "custom_candidate_exclusions": custom_exclusions,
              "derived_fixed_art_blocked": [
                  row for row in custom_exclusions
                  if row["reason"] == "missing_native_capsule_card"],
              "before_inventory": len(initial["known"]), "after_inventory": len(final["known"]),
              "before_forms": len(initial["forms"]), "after_forms": len(final["forms"]),
              "remaining": remaining,
              "remaining_details": remaining_details,
              "manual_review_units": [u for u in units.values() if u.get("review_placeholder")],
              "missing_native_capsule_art": [uid for uid in seeds if not units[uid].get("capsule_sprites")],
              "limitations": initial["limitations"], "blocked_recipes_before": initial["blocked_recipes"]}
    return catalog, report


def get_machine(catalog, kind, machine_id):
    return next((m for m in catalog[kind] if m["id"] == machine_id), None)


def mutate(catalog, kind, action, machine_id, value=None):
    result = copy.deepcopy(catalog)
    current = get_machine(result, kind, machine_id)
    if action == "create":
        if current is not None or value["id"] != machine_id:
            raise ValueError("Machine ID exists or differs from payload")
        item = copy.deepcopy(value)
        if kind == "fixed_machines":
            # A copied native record is still a new machine. It must not
            # accidentally delegate to a stock sequence which does not exist.
            if item.get("owner") in (None, "native"):
                item["owner"] = "editor"
            item["draw_policy"] = "independent_weighted"
        result[kind].append(item)
    elif action == "update":
        if current is None or value.get("id", machine_id) != machine_id:
            raise ValueError("Machine missing or ID change requested")
        def draw_signature(machine):
            return [(s.get("unit_id"), s.get("weight"), s.get("native_draw_group"))
                    for s in machine.get("slots", [])]
        before_draw = draw_signature(current)
        current.update(copy.deepcopy(value))
        if kind == "fixed_machines" and before_draw != draw_signature(current):
            current["draw_policy"] = "independent_weighted"
    elif action == "delete":
        if current is None:
            raise ValueError("Machine missing")
        current["enabled"] = False
        current["retired"] = True
    else:
        raise ValueError("Unknown mutation")
    validate(result)
    return result


def export_bundle(catalog, output):
    validate(catalog)
    out = Path(output)
    revision = digest(catalog)
    payload = {"schema_version": 1, "catalog_revision": revision,
               "requires_runtime_adapter": True,
               "fixed_machines": catalog["fixed_machines"], "custom_machines": catalog["custom_machines"],
               "units": catalog["units"]}
    if "custom_candidate_policy" in catalog:
        payload["custom_candidate_policy"] = copy.deepcopy(catalog["custom_candidate_policy"])
    # Identical semantic payloads prevent client/server configuration divergence.
    for role in ("client", "server"):
        write_json(out / f"{role}.json", {**payload, "role": role})
    write_json(out / "manifest.json", {"schema_version": 1, "catalog_revision": revision,
        "status": "semantic configuration; compile and install with the matching runtime",
        "files": {f"{role}.json": hashlib.sha256((out / f"{role}.json").read_bytes()).hexdigest()
                  for role in ("client", "server")}})
