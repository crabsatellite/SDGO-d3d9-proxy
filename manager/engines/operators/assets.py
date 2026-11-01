"""Read existing game assets without altering the client archive."""
import functools
import hashlib
import io
import math
from pathlib import Path
import re
import struct
import zlib

OP_SIZE = 1671
ITEM_SIZE = 614
SPRITE_FIELDS = [(205, 54), (313, 54), (421, 54), (533, 50), (633, 50), (733, 50), (833, 50)]


def wchar(data, offset, capacity):
    return data[offset:offset + capacity * 2].decode("utf-16-le").split("\0")[0]


def put_wchar(data, offset, capacity, value):
    raw = value.encode("utf-16-le")
    if len(raw) >= capacity * 2:
        raise ValueError("名称或资源路径超过原生字段容量")
    data[offset:offset + capacity * 2] = raw.ljust(capacity * 2, b"\0")


class Assets:
    def __init__(self, client):
        self.root = Path(client)
        data = (self.root / "GPackData.zdx").read_bytes()
        self.index_hash = hashlib.sha256(data).hexdigest()
        version, self.chunk_size, _, _, count, _ = struct.unpack_from("<6I", data)
        if version != 1 or not 0 < self.chunk_size <= 1048576 or count > 1000000:
            raise ValueError("不支持的游戏资源包")
        self.entries = {}
        pos = 24
        for _ in range(count):
            chars = []
            while True:
                code = struct.unpack_from("<H", data, pos)[0] ^ 0xAA
                pos += 2
                if not code:
                    break
                chars.append(chr(code))
                if len(chars) > 4096:
                    raise ValueError("资源名称过长")
            size, first, chunks = struct.unpack_from("<III", data, pos)
            pos += 36
            name = "".join(chars).replace("\\", "/").lower()
            if name in self.entries:
                raise ValueError("重复的游戏资源编号")
            self.entries[name] = (size, first, chunks)

    @functools.lru_cache(maxsize=128)
    def read(self, name):
        name = name.replace("\\", "/").lower()
        if not re.fullmatch(r"(?:gdrs|sprs|txrs)/[0-9]+\.(?:gdt|spr|txr)", name):
            raise ValueError("无效资源路径")
        loose = self.root.joinpath(*name.split("/"))
        if loose.exists():
            data = loose.read_bytes()
        else:
            size, cursor, chunks = self.entries[name]
            if size > 67108864 or chunks > 200000:
                raise ValueError("资源超过读取上限")
            result, seen = bytearray(), set()
            with (self.root / "GPackData.zpk").open("rb") as f:
                for _ in range(chunks):
                    if cursor in seen:
                        raise ValueError("资源块循环")
                    seen.add(cursor)
                    f.seek(cursor * (self.chunk_size + 8))
                    block = f.read(self.chunk_size + 8)
                    if len(block) != self.chunk_size + 8:
                        raise ValueError("资源包不完整")
                    cursor = struct.unpack_from("<I", block)[0]
                    result.extend(block[8:])
            data = bytes(result[:size])
        if data.startswith(b"ZOAGZIP1"):
            expected = struct.unpack_from("<I", data, 8)[0]
            if expected > 67108864:
                raise ValueError("解压资源超过上限")
            decoder = zlib.decompressobj(31)
            result = decoder.decompress(data[12:], expected + 1)
            if len(result) != expected or not decoder.eof:
                raise ValueError("资源压缩校验失败")
            return result
        return data

    @functools.lru_cache(maxsize=4)
    def records(self, name, expected_size):
        data = self.read(name)
        if data[:2] != b"\xf9\xf8":
            raise ValueError("副官适配器只支持当前 UTF16LE GDT 格式")
        size, count, extra = struct.unpack_from("<III", data, 2)
        key_size = (size + count + extra) % 65 + 192
        key = data[14:14 + key_size]
        stride = math.ceil(size / key_size) * key_size
        if size != expected_size or len(key) != key_size or max(key) >= key_size or len(data) != 14 + key_size + count * stride:
            raise ValueError("副官记录布局不匹配")
        result = {}
        for i in range(count):
            decoded = bytearray()
            start = 14 + key_size + i * stride
            for offset in range(0, stride, key_size):
                block = data[start + offset:start + offset + key_size]
                decoded.extend(block[key[j]] ^ key[(768 + 3*j) % key_size] for j in range(key_size))
            row = bytes(decoded[:size])
            identity = struct.unpack_from("<I", row)[0]
            if identity in result:
                raise ValueError("重复的副官或物品编号")
            result[identity] = row
        return result

    @functools.lru_cache(maxsize=96)
    def portrait(self, operator_id):
        from PIL import Image
        rows = self.records("gdrs/0255.gdt", OP_SIZE)
        row = rows[operator_id]
        path = next((wchar(row, off, cap) for off,cap in [(533,50),(205,54),(313,54),(421,54)]
                     if wchar(row,off,cap)), None)
        if not path:
            raise ValueError("副官缺少头像资源")
        sprite = self.read(path)
        version = struct.unpack_from("<I", sprite)[0]
        if version not in (10000,11000):
            raise ValueError("副官缩略图的 SPR 版本尚不支持")
        u0, v0, u1, v1 = struct.unpack_from("<4f", sprite, 20)
        match = re.search(rb"[tT]\x00[xX]\x00[rR]\x00[sS]\x00\\\x00", sprite)
        if match is None:
            raise ValueError("SPR 缺少纹理引用")
        pos, end = match.start(), match.start()
        while end + 1 < len(sprite) and sprite[end:end+2] != b"\0\0":
            end += 2
        texture = self.read(sprite[pos:end].decode("utf-16-le"))
        if texture.startswith(b"ZOATEX1\0"):
            texture = texture[12:]
        with Image.open(io.BytesIO(texture)) as image:
            image.load()
            if image.width * image.height > 16777216 or not all(math.isfinite(x) and 0 <= x <= 1 for x in (u0,v0,u1,v1)) or u1 <= u0 or v1 <= v0:
                raise ValueError("头像纹理或裁切范围无效")
            crop = image.crop((round(u0*image.width), round(v0*image.height), round(u1*image.width), round(v1*image.height))).convert("RGBA")
            buf = io.BytesIO()
            crop.save(buf, "PNG")
            return buf.getvalue()


def validate_png(data):
    from PIL import Image
    if len(data) > 8388608 or not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("请使用不超过 8 MiB 的 PNG 头像")
    if len(data)<24 or not all(1<=v<=2048 for v in struct.unpack_from('>II',data,16)):
        raise ValueError("头像边长必须为 1–2048 像素")
    with Image.open(io.BytesIO(data)) as img:
        img.verify()
    with Image.open(io.BytesIO(data)) as img:
        if not 1 <= img.width <= 2048 or not 1 <= img.height <= 2048:
            raise ValueError("头像边长必须为 1–2048 像素")
        return img.width, img.height


def sprite_binary(texture_path, width, height):
    out = bytearray(176)
    struct.pack_into("<5I4f", out, 0, 10000, 1, 20, height, width, 0, 0, 1, 1)
    put_wchar(out, 44, 66, texture_path)
    return bytes(out)
