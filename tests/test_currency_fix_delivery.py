import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile
from manager.workspace import Workspace
from manager import packages,storage

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('currency_package',ROOT/'tools/package_capsule_currency_fix.py');publisher=importlib.util.module_from_spec(spec);spec.loader.exec_module(publisher)


class CurrencyDelivery(unittest.TestCase):
    def test_actual_package_import_registers_fix_without_changing_catalog_data(self):
        binary=ROOT/'build/Release/capsule_currency_fix.dll'
        if not binary.exists():self.skipTest('Build native DLL first')
        with tempfile.TemporaryDirectory()as value:
            root=Path(value);client=root/'client';client.mkdir();(client/'GOnline.exe').write_bytes(b'fixture')
            resident=client/'sdgo/mods/capsule_catalog';resident.mkdir(parents=True);(resident/'runtime.ini').write_bytes(b'owner-state-preserve');(resident/'overlay.zpk').write_bytes(b'existing-overlay')
            symbol=client/'sdgo/symbols/GOnline-54BE1568-00C97000.ini';symbol.parent.mkdir();symbol.write_text('[other.mod]\nkind=data\nrva=0x1234\n',encoding='ascii')
            state=Workspace(root/'state',ROOT/'mods');state.add_host(client);host=state.settings['hosts'][0]['id']
            archive=root/'currency.zip';publisher.build(binary,archive)
            with zipfile.ZipFile(archive)as z:
                manifest=json.loads(z.read('manifest.json'))
                for name,digest in manifest['files'].items():self.assertEqual(hashlib.sha256(z.read(name)).hexdigest(),digest)
                self.assertFalse(any(n.endswith(('.ps1','.cmd','.exe','.py'))for n in z.namelist()))
            with patch.object(packages,'_running',return_value=False):packages.import_client(state,host,base64.b64encode(archive.read_bytes()).decode())
            host_state=state.public()['hosts'][0];mod=next(m for m in host_state['archive']if m['id']=='capsule_currency_fix')
            self.assertTrue(mod['enabled']);self.assertTrue(mod['registered']);self.assertEqual(mod['version'],'1.0.1')
            self.assertEqual((resident/'runtime.ini').read_bytes(),b'owner-state-preserve');self.assertEqual((resident/'overlay.zpk').read_bytes(),b'existing-overlay')
            self.assertIn('other.mod',storage.ini_values(symbol.read_bytes()));self.assertIn('capsule.activity.load_items',storage.ini_values(symbol.read_bytes()))
            state.services.close()


if __name__=='__main__':unittest.main()
