"""Manager revisions and target boundaries for collection gameplay operations."""
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
import zipfile

from manager import storage
from manager.providers import attach
from manager.workspace import Workspace

ROOT = Path(__file__).resolve().parents[1]


class CollectionManagerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.workspace = Workspace(self.root / 'workspace', ROOT / 'mods')
        attach(self.workspace)
        self.provider = self.workspace.providers['collection_capsule']
        self.provider.configure({'client': str(self.root / 'client'),
                                 'server_ini': str(self.root / 'world/WorldServer.ini')})

    def tearDown(self):
        self.temp.cleanup()

    def build_state(self):
        state = {'build_id': 'a' * 32,
                 'machine_kind': 'activity',
                 'config_revision': storage.revision(storage.json_bytes(self.provider.config)),
                 'machine_id': 990001, 'eligible_roots': 1}
        storage.atomic(self.provider.state_file, storage.json_bytes(state))
        output, _ = self.provider.generated()
        output.mkdir(parents=True)
        return output

    def action(self, name, **values):
        return self.provider.request('POST', '/api/' + name,
                                     {'revision': self.provider.revision(), **values}, {})

    def test_stale_revision_cannot_generate_or_install(self):
        for action in ('generate', 'install-server', 'install-client', 'register-family'):
            with self.assertRaises(storage.Conflict):
                self.provider.request('POST', '/api/' + action, {'revision': 'stale'}, {})
        self.assertFalse(self.provider.state_file.exists())

    def test_changed_target_invalidates_previous_build(self):
        self.build_state()
        self.provider.configure({'client': str(self.root / 'different-client')})
        self.assertFalse(self.provider.public()['generated'])
        with self.assertRaisesRegex(ValueError, '重新生成'):
            self.action('install-client')

    def test_install_uses_generated_package_and_configured_target_only(self):
        output = self.build_state()
        with patch('tools.collection_capsule.install_server', return_value={'applied': True}) as install:
            self.action('install-server', server_ini='attacker-target', package='attacker-package')
        install.assert_called_once_with(output / 'server-package', Path(self.provider.config['server_ini']))

    def test_catalog_upgrade_needs_no_build_and_history_import_is_removed(self):
        database = Mock()
        with patch('owner.collection_capsule.deployment.connection', return_value=database), \
             patch('owner.collection_capsule.deployment.upgrade_catalog', return_value={'applied':True,'world_restart_required':False}) as upgrade:
            result = self.action('upgrade-catalog')
        self.assertTrue(result['applied']);self.assertFalse(result['world_restart_required'])
        upgrade.assert_called_once_with(database)
        database.close.assert_called_once()
        with self.assertRaises(ValueError):
            self.action('import-history', history={'complete':True})

    def test_family_can_register_before_first_build(self):
        database = Mock()
        with patch('owner.collection_capsule.deployment.connection', return_value=database), \
             patch('owner.collection_capsule.deployment.register_unit_family', return_value={'registered': True}) as register:
            result = self.action('register-family', family={'fixture': 'declared-root'})
        self.assertTrue(result['registered'])
        register.assert_called_once_with(database, {'fixture': 'declared-root'})
        database.close.assert_called_once()

    def test_stopped_world_requirement_is_a_readable_operation_error(self):
        self.build_state()
        with patch('tools.collection_capsule.install_server', side_effect=RuntimeError('Stop the selected World host')):
            with self.assertRaisesRegex(ValueError, 'Stop the selected World host'):
                self.action('install-server')

    def test_export_hashes_and_inventory_reject_tampered_player_payload(self):
        output = self.build_state()
        player = output / 'player'
        player.mkdir()
        (player / 'README.txt').write_bytes(b'portable player notes')
        (player / 'bundle-manifest.json').write_text(json.dumps({'files': {
            'README.txt': hashlib.sha256(b'portable player notes').hexdigest()}}), encoding='utf-8')
        value = self.action('export')
        with zipfile.ZipFile(io.BytesIO(value.data)) as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(archive.read('README.txt'), b'portable player notes')
        (player / 'private.ini').write_bytes(b'must never export')
        with self.assertRaisesRegex(ValueError, '清单'):
            self.action('export')
        (player / 'private.ini').unlink()
        (player / 'README.txt').write_bytes(b'changed after build')
        with self.assertRaisesRegex(ValueError, '内容'):
            self.action('export')

    def test_internal_machine_id_is_hidden_from_daily_settings(self):
        world = self.root / 'world'
        mod = world / 'sdgo/mods/collection_capsule'
        mod.mkdir(parents=True)
        (world / 'WorldServer.exe').write_bytes(b'fixture')
        (mod / 'mod.ini').write_bytes((ROOT / 'mods/collection_capsule/mod.ini').read_bytes())
        self.workspace.add_host(world)
        host = next(h for h in self.workspace.settings['hosts'] if h['role'] == 'world')
        self.assertEqual(self.workspace.details(host['id'], 'collection_capsule')['contract']['settings'], [])
        self.workspace.mode = 'developer'
        self.assertEqual(self.workspace.details(host['id'], 'collection_capsule')['values']['machine_id'], 990001)


if __name__ == '__main__':
    unittest.main()
