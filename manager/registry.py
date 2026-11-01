"""Read Mod-owned management contracts; UI never guesses writable settings."""
from __future__ import annotations
import json
from pathlib import Path
import re

ID = re.compile(r'[a-z][a-z0-9_-]{0,63}\Z')
ROLES = {'client', 'battle', 'agent', 'world'}


def load(root):
    records = {}
    for path in sorted(Path(root).glob('*/manager.json')):
        value = json.loads(path.read_text(encoding='utf-8-sig'))
        identity = value.get('id', '')
        if value.get('schema_version') != 1 or not ID.fullmatch(identity) or identity != path.parent.name:
            raise ValueError('管理注册无效：' + str(path))
        if identity in records or not value.get('title') or not set(value.get('hosts', [])) <= ROLES:
            raise ValueError('管理注册重复或宿主无效：' + identity)
        seen = set()
        for field in value.get('settings', []):
            key = field['id']
            if not ID.fullmatch(key) or key in seen or field.get('audience') not in ('user','developer'):
                raise ValueError('配置必须登记可见层级：' + identity)
            seen.add(key)
            if field['type'] not in ('boolean', 'integer', 'number', 'string', 'integer_list', 'choice'):
                raise ValueError('字段类型不受支持：' + identity)
            if field['file'] not in ('mod.ini', 'config.ini') or field.get('section', 'settings') != 'settings':
                raise ValueError('字段不能写入运行代码或任意文件：' + identity)
        records[identity] = value
    return records


def validate(field, value):
    kind = field['type']
    if kind == 'boolean':
        if type(value) is not bool:
            raise ValueError(field['label'] + '必须是开关')
        return '1' if value else '0'
    if kind in ('integer', 'number'):
        if isinstance(value, bool) or not isinstance(value, (int, float)) or (kind == 'integer' and type(value) is not int):
            raise ValueError(field['label'] + '数值无效')
        if not field.get('minimum', -2147483648) <= value <= field.get('maximum', 2147483647):
            raise ValueError(field['label'] + '超出允许范围')
        return str(value)
    if not isinstance(value, str) or any(c in value for c in '\r\n\0') or len(value) > field.get('max_length', 4096):
        raise ValueError(field['label'] + '文本无效')
    if kind == 'integer_list' and value:
        if not re.fullmatch(r'\d+(?:\s*,\s*\d+)*', value) or any(not 1 <= int(x) <= 2147483647 for x in value.split(',')):
            raise ValueError(field['label'] + '需要用逗号分隔正整数编号')
    if kind == 'choice' and value not in field['options']:
        raise ValueError(field['label'] + '选项无效')
    if field.get('pattern') and not re.fullmatch(field['pattern'],value):
        raise ValueError(field['label']+'格式无效')
    return value
