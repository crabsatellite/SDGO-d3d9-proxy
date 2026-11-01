"""Ordinary edits retain activity rewards and invalidate stale poster caches."""
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest.mock import patch

from PIL import Image
from manager.engines.capsules import capsule_client_compile as compiler
from manager.engines.capsules.capsule_catalog import ClientData
from manager_capsule_fixture import fixture_catalog
from owner.collection_capsule.client import ArchiveReader, build_archive, encode_gdt
from test_collection_client import build_registry, make_client, make_cover


class FakeArt:
    def __init__(self, client, lookup, catalog):
        self.reader = ClientData(client)

    def card(self, unit, variant=0):
        return Image.new('RGBA', (182, 108), (25, 50, 75, 255))


class CollectionRegistryCoexistenceTests(unittest.TestCase):
    def test_normal_compile_retains_activity_and_historical_unit_gifts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            client = make_client(root / 'client')
            reader = ArchiveReader(client / 'GPackData.zdx', client / 'GPackData.zpk')
            resources = {name: reader.read(name) for name in reader.entries}
            candidate = bytearray(18)
            struct.pack_into('<I', candidate, 0, 3520001)
            struct.pack_into('<III', candidate, 5, 1, 7, 100)
            resources['gdrs/0155.gdt'] = encode_gdt(18, [bytes(candidate)], extra=35)
            zdx, zpk, _ = build_archive(resources)
            (client / 'GPackData.zdx').write_bytes(zdx)
            (client / 'GPackData.zpk').write_bytes(zpk)
            cover = root / 'cover.png'; make_cover(cover)
            initial = root / 'initial'
            build_registry(client, cover, initial)
            resident = client / 'sdgo/mods/capsule_catalog'
            shutil.copytree(initial, resident)
            fingerprint = compiler.extension_fingerprint(client)
            catalog = fixture_catalog()
            for unit in catalog['units']:
                unit['capsule_sprites'] = []
            with patch.object(compiler, 'Assets', FakeArt), patch.object(
                compiler, 'patch_capsule_price_defaults', return_value=(b'fixture-ui', {'fixture': True})
            ):
                result = compiler.compile_catalog(catalog, client, root / 'unused.json', root / 'ordinary')
            ordinary = root / 'ordinary'
            self.assertEqual(result['preserved_extensions_signature'], fingerprint)
            for name in ('0031', '0099'):
                self.assertEqual((initial / 'data' / (name + '.gdt')).read_bytes(),
                                 (ordinary / 'data' / (name + '.gdt')).read_bytes())
            before = ArchiveReader(initial / 'overlay.zdx', initial / 'overlay.zpk')
            after = ArchiveReader(ordinary / 'overlay.zdx', ordinary / 'overlay.zpk')
            for path in ('sprs/65534.spr', 'txrs/65534.txr', 'gdrs/0031.gdt', 'gdrs/0099.gdt'):
                self.assertEqual(before.read(path), after.read(path))
            old_meta = json.loads((initial / 'manifest.json').read_text(encoding='utf-8'))['unit_items']
            new_meta = json.loads((ordinary / 'manifest.json').read_text(encoding='utf-8'))['unit_items']
            self.assertEqual(old_meta, new_meta)
            shutil.copytree(ordinary, resident, dirs_exist_ok=True)
            self.assertEqual(compiler.extension_fingerprint(client), fingerprint)
            Image.new('RGB', (361, 322), '#b04020').save(cover)
            updated = root / 'new-poster'
            build_registry(client, cover, updated, current=resident)
            shutil.copytree(updated, resident, dirs_exist_ok=True)
            self.assertNotEqual(compiler.extension_fingerprint(client), fingerprint)


if __name__ == '__main__':
    unittest.main()
