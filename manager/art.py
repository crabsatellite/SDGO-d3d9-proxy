"""Artwork for the Manager's look, read at run time from the player's own client.

The repository and the package carry no game art: each picture is a texture of the selected
client's GPackData (the same reader as the operator portraits), cut to a fixed box and served
by /api/art/<key>. Without a client, or when a texture is missing, the request fails and the
UI library falls back to its own drawn styling.
"""
from __future__ import annotations
import io
import threading
from pathlib import Path

# key: (texture, crop box (x0, y0, x1, y1) in texture pixels). The loading-screen
# illustrations fill y 128..896 of their 1024 texture with the title at the top left and an
# info box at the bottom left (x < 470); the crops leave both out.
CATALOG = {
    'hero-unicorn': ('txrs/1890.txr', (470, 128, 1024, 896)),
    'hero-strike': ('txrs/1888.txr', (470, 128, 1024, 896)),
    'hero-maoh': ('txrs/0702.txr', (470, 128, 1024, 896)),
    'hero-awaken': ('txrs/0701.txr', (470, 128, 1024, 896)),
    'hero-space': ('txrs/7578.txr', (280, 128, 1024, 896)),
    'hangar': ('txrs/2654.txr', (0, 190, 1024, 845)),
    'plating': ('txrs/2700.txr', (0, 0, 1024, 930)),
    'nebula': ('txrs/0929.txr', (0, 0, 1024, 1024)),
    'squad': ('txrs/10701.txr', (10, 10, 909, 981)),
}

_lock = threading.Lock()
_readers: dict[str, object] = {}
_cache: dict[tuple[str, str], bytes] = {}


def _client_root(workspace, host_id=None):
    hosts = [h for h in workspace.settings.get('hosts', []) if h.get('role') == 'client']
    if host_id:
        hosts = [h for h in hosts if h.get('id') == host_id]
    for host in hosts:
        root = Path(host['path'])
        if (root / 'GPackData.zdx').is_file() and (root / 'GPackData.zpk').is_file():
            return root
    raise LookupError('没有可读取美术资源的客户端')


def image(workspace, key, host_id=None):
    """JPEG bytes of a catalogued picture from a registered client."""
    if key not in CATALOG:
        raise LookupError('未登记的美术资源')
    root = _client_root(workspace, host_id)
    with _lock:
        cached = _cache.get((str(root), key))
        if cached is not None:
            return cached
        reader = _readers.get(str(root))
        if reader is None:
            from .engines.operators.assets import Assets
            reader = _readers[str(root)] = Assets(root)
    from PIL import Image
    texture, box = CATALOG[key]
    data = reader.read(texture)
    if data.startswith(b'ZOATEX1\0'):
        data = data[12:]
    with Image.open(io.BytesIO(data)) as picture:
        picture.load()
        if picture.width * picture.height > 16777216:
            raise ValueError('美术纹理过大')
        x0, y0, x1, y1 = box
        cut = picture.crop((x0, y0, min(x1, picture.width), min(y1, picture.height))).convert('RGB')
    out = io.BytesIO()
    cut.save(out, 'JPEG', quality=86, optimize=True)
    result = out.getvalue()
    with _lock:
        _cache[(str(root), key)] = result
    return result
