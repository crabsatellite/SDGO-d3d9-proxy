"""Small, lossless configuration transactions. No game or database writes."""
from __future__ import annotations
import configparser
import hashlib
import json
import os
from pathlib import Path
import re
import threading
import uuid

LOCK = threading.RLock()


class Conflict(ValueError):
    pass


def revision(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def atomic(path: Path, raw: bytes):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name('.' + path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        with temporary.open('xb') as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n').encode('utf-8')


def read_json(path, default=None):
    return json.loads(Path(path).read_text(encoding='utf-8-sig')) if Path(path).exists() else default


def decode_ini(raw):
    if raw.startswith(b'\xff\xfe'):
        return raw[2:].decode('utf-16-le'), 'utf-16-le', b'\xff\xfe'
    if raw.startswith(b'\xfe\xff'):
        return raw[2:].decode('utf-16-be'), 'utf-16-be', b'\xfe\xff'
    if raw.startswith(b'\xef\xbb\xbf'):
        return raw[3:].decode('utf-8'), 'utf-8', b'\xef\xbb\xbf'
    try:
        return raw.decode('utf-8'), 'utf-8', b''
    except UnicodeDecodeError:
        # Native private servers commonly use the local Windows ANSI encoding.
        return raw.decode('mbcs' if os.name == 'nt' else 'cp1252'), 'mbcs' if os.name == 'nt' else 'cp1252', b''


def ini_values(raw):
    text, _, _ = decode_ini(raw)
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    parser.read_string(text)
    return parser


def patch_ini(raw, changes):
    """Change exact registered keys, preserving unknown keys/comments/encoding."""
    text, encoding, bom = decode_ini(raw)
    newline = '\r\n' if '\r\n' in text else '\n'
    for section, key, value in changes:
        if not re.fullmatch(r'[A-Za-z0-9_.-]+', section) or not re.fullmatch(r'[A-Za-z0-9_.-]+', key):
            raise ValueError('配置键无效')
        value = str(value)
        if any(c in value for c in '\r\n\0'):
            raise ValueError('配置值不能包含换行或空字符')
        lines = text.splitlines(keepends=True)
        active = False
        start = end = None
        hits = []
        for i, line in enumerate(lines):
            header = re.match(r'^\s*\[([^\]]+)\]\s*(?:[;#].*)?$', line.strip())
            if header:
                if active:
                    end = i
                active = header[1].casefold() == section.casefold()
                if active:
                    if start is not None:
                        raise ValueError('配置节重复，请先修正原文件：' + section)
                    start = i
                continue
            if active and re.match(r'^\s*' + re.escape(key) + r'\s*=', line, re.I):
                hits.append(i)
        if len(hits) > 1:
            raise ValueError('配置键重复，请先修正原文件：' + key)
        if hits:
            line = lines[hits[0]]
            prefix = line[:line.index('=') + 1]
            ending = '\r\n' if line.endswith('\r\n') else '\n' if line.endswith('\n') else ''
            lines[hits[0]] = prefix + value + ending
        elif start is not None:
            at = end if end is not None else len(lines)
            if at and not lines[at - 1].endswith(('\r', '\n')):
                lines[at - 1] += newline
            lines.insert(at, key + '=' + value + newline)
        else:
            if lines and not lines[-1].endswith(('\r', '\n')):
                lines[-1] += newline
            lines.extend([newline + '[' + section + ']' + newline, key + '=' + value + newline])
        text = ''.join(lines)
    # Native Windows INI APIs need a Unicode BOM for newly entered Unicode
    # paths; an ASCII/ANSI file cannot represent every legitimate owner path.
    if not bom and any(ord(c)>127 for _,_,value in changes for c in str(value)):
        return b'\xff\xfe'+text.encode('utf-16-le')
    return bom + text.encode(encoding)


def safe_child(root, relative):
    root = Path(root).resolve()
    relative = str(relative).replace('\\', '/')
    if not relative or ':' in relative or relative.startswith('/') or any(p in ('', '.', '..') for p in relative.split('/')):
        raise ValueError('配置相对路径无效')
    result = (root / relative).resolve()
    if not result.is_relative_to(root):
        raise ValueError('配置路径越界')
    return result
