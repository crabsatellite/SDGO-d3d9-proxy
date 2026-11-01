"""Match native permanent unit gifts and allocate missing content metadata.

These records describe delivery, not pool membership. The live unit graph still
decides the prize pool; native unit imports can publish matching categories.
"""
from __future__ import annotations

import configparser
import hashlib
import json
from pathlib import Path
import struct

from .client import ArchiveReader, decode_gdt

OWNED_CATEGORY_MIN = 1180000
OWNED_CATEGORY_MAX = 1199999


def _table(snapshot: Path, name: str) -> list[dict]:
    root = snapshot / 'GOnlineGame'
    files = sorted(root.glob(name + '.part*.json'))
    single = root / (name + '.json')
    if single.is_file():
        if files:
            raise ValueError('Both complete and split unit item metadata exist')
        files = [single]
    result = []
    meta_path = snapshot / '_meta.json'
    meta = json.loads(meta_path.read_text(encoding='utf-8-sig')) if meta_path.is_file() else {}
    for path in files:
        expected = meta.get('files', {}).get(path.relative_to(snapshot).as_posix(), {}).get('sha256')
        if expected and hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError('Unit item metadata differs from its snapshot receipt')
        value = json.loads(path.read_text(encoding='utf-8-sig'))
        if not isinstance(value, list):
            raise ValueError('Unit item metadata must be a table array')
        result.extend(value)
    return result


def _client_categories(client_root: Path | None, client_gdrs: Path | None) -> dict[int, bytes]:
    if client_gdrs is not None:
        raw = (client_gdrs / '0031.gdt').read_bytes()
    else:
        registry = client_root / 'sdgo/mods/capsule_catalog'
        config = configparser.ConfigParser(interpolation=None)
        runtime = registry / 'runtime.ini'
        if runtime.is_file():
            config.read(runtime, encoding='utf-8-sig')
        entry = config.get('gdt', '0031', fallback=None)
        if entry:
            path = registry.joinpath(*entry.replace('\\', '/').split('/')).resolve()
            if not path.is_relative_to(registry.resolve()):
                raise ValueError('Unit item registry path leaves its directory')
            raw = path.read_bytes()
            if hashlib.sha256(raw).hexdigest() != config.get('gdt', '0031_sha256'):
                raise ValueError('Installed unit item category table hash differs')
        else:
            reader = ArchiveReader(client_root / 'GPackData.zdx', client_root / 'GPackData.zpk', loose_root=client_root)
            raw = reader.read('gdrs/0031.gdt')
    size, rows, _ = decode_gdt(raw)
    if size != 614:
        raise ValueError('Unsupported native unit item category layout')
    result = {}
    for row in rows:
        identity = struct.unpack_from('<I', row)[0]
        if identity in result:
            raise ValueError('Duplicate native item category ID')
        result[identity] = row
    return result


def build_unit_items(snapshot: Path, roots: list[dict], *, client_root: Path | None = None,
                     client_gdrs: Path | None = None) -> list[dict]:
    server = _table(snapshot, 'dbo.tbl_ItemCategory')
    if not server:
        raise ValueError('Export the target server item categories before building activity capsules')
    categories = {}
    for row in server:
        identity = row.get('CatalogueID')
        if type(identity) is not int or identity <= 0 or identity in categories:
            raise ValueError('Invalid server item category identity')
        categories[identity] = row
    client = _client_categories(client_root, client_gdrs)
    owned = _table(snapshot, 'collection.unit-items')
    result, used, owned_units = [], set(categories) | set(client), set()
    for row in owned:
        unit, category = row.get('UnitID'), row.get('CategoryID')
        if (type(unit) is not int or unit <= 0 or unit in owned_units or type(category) is not int
                or not OWNED_CATEGORY_MIN <= category <= OWNED_CATEGORY_MAX):
            raise ValueError('Invalid owned unit gift metadata')
        existing = categories.get(category)
        if (not existing or existing.get('RefIndex') != unit or existing.get('TimeLimit') != 0
                or existing.get('Option1') != 1 or existing.get('Option2') != 2):
            raise ValueError('An owned unit gift category was removed or externally edited')
        result.append({'unit_id': unit, 'category_id': category, 'name': str(row.get('Name') or unit),
                       'created_by_collection': True})
        owned_units.add(unit)
    allocated = {row['unit_id'] for row in result}
    cursor = OWNED_CATEGORY_MIN
    for root in sorted(roots, key=lambda row: row['unit_id']):
        unit = root['unit_id']
        name = str(root['name']).split('\0', 1)[0].strip().encode('utf-16-le')[:96].decode('utf-16-le', 'ignore') or str(unit)
        if unit in allocated:
            continue
        candidates = []
        for identity, row in categories.items():
            record = client.get(identity)
            if (1100000 <= identity < 1200000 and row.get('RefIndex') == unit
                    and row.get('TimeLimit') == 0 and row.get('Option1') == 1 and row.get('Option2') == 2
                    and record and record[4] == 0 and struct.unpack_from('<I', record, 105)[0] == unit
                    and record[109] == 0 and struct.unpack_from('<I', record, 110)[0] == 1):
                candidates.append(identity)
        if candidates:
            result.append({'unit_id': unit, 'category_id': min(candidates), 'name': name,
                           'created_by_collection': False})
        else:
            while cursor in used and cursor <= OWNED_CATEGORY_MAX:
                cursor += 1
            if cursor > OWNED_CATEGORY_MAX:
                raise ValueError('No free native unit gift category IDs remain')
            result.append({'unit_id': unit, 'category_id': cursor, 'name': name,
                           'created_by_collection': True})
            used.add(cursor)
            cursor += 1
        allocated.add(unit)
    return result
