"""Exercise the migrated business logic through registered Manager providers."""
import copy
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from manager.workspace import Workspace
from manager.providers import attach
from manager import storage
from manager_capsule_fixture import fixture_catalog
from manager_operator_fixture import fixture

ROOT=Path(__file__).resolve().parents[1]


class ProviderTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.workspace=Workspace(self.root/'manager',ROOT/'mods');attach(self.workspace)

    def tearDown(self):
        for provider in self.workspace.providers.values():
            if provider.engine is not None and hasattr(provider.engine,'close'):provider.engine.close()
        self.temp.cleanup()

    def test_shop_crud_identity_reservation_and_portable_export(self):
        provider=self.workspace.providers['shop'];self.workspace.mode='developer'
        provider.configure({'workspace':str(self.root/'shop'),'demo':'1'});self.workspace.mode='standard'
        state=provider.request('GET','/api/shop/catalog',{},{});self.assertTrue(state['items'])
        def action(name,body):
            nonlocal state
            state=provider.request('POST','/api/shop/'+name,body,{'X-Shop-Revision':state['revision']})
        action('create',{'category_id':state['items'][0]['Id']});first=state['catalog']['products'][0]
        key=first['key'];number=first['product_id']
        action('update',{'key':key,'values':{'name':'Manager product','price':631,'currency':'MB'}})
        self.assertEqual(state['catalog']['products'][0]['price'],631)
        action('delete',{'key':key});self.assertTrue(state['catalog']['products'][0]['retired'])
        action('restore',{'key':key});self.assertFalse(state['catalog']['products'][0]['retired'])
        action('create',{'category_id':state['items'][0]['Id']})
        self.assertNotEqual(number,state['catalog']['products'][1]['product_id'])
        before=(self.root/'shop/shop.json').read_bytes()
        with self.assertRaises(ValueError):action('update',{'key':key,'values':{'product_id':1}})
        self.assertEqual(before,(self.root/'shop/shop.json').read_bytes())
        exported=provider.request('GET','/api/shop/export',{},{}).data
        self.assertNotIn(str(self.root).encode(),exported);self.assertIn(b'Manager product',exported)

    def test_operator_crud_uses_original_validation_and_revisions(self):
        from manager.engines.operators import model
        from manager.engines.operators.workspace import Workspace as OperatorWorkspace
        value=fixture();engine=OperatorWorkspace.__new__(OperatorWorkspace)
        engine.root=self.root/'operators';engine.root.mkdir();engine.path=engine.root/'operators.json'
        engine.assets=SimpleNamespace(index_hash='fixture');engine.baseline=copy.deepcopy(value)
        engine.server_ini=None;engine.module_binary=None;model.save(engine.path,value)
        provider=self.workspace.providers['operators'];provider.engine=engine
        state=provider.request('GET','/api/operators',{},{});old=state['revision']
        state=provider.request('POST','/api/operators/new',{'template_id':60003},{'X-Catalog-Revision':old})
        identity=state['selected_id'];self.assertNotEqual(identity,60003)
        state=provider.request('PUT','/api/operators/'+str(identity),{'name':'Manager operator'},{'X-Catalog-Revision':state['revision']})
        self.assertEqual(next(o for o in state['operators'] if o['id']==identity)['name'],'Manager operator')
        with self.assertRaises(ValueError):provider.request('PUT','/api/operators/'+str(identity),{'name':'stale'},{'X-Catalog-Revision':old})
        state=provider.request('DELETE','/api/operators/'+str(identity),{}, {'X-Catalog-Revision':state['revision']})
        self.assertFalse(next(o for o in state['operators'] if o['id']==identity)['enabled'])
        self.assertFalse(state['capabilities']['server_apply'])

    def test_capsule_edit_delete_restore_and_stale_revision(self):
        path=self.root/'capsule.json';storage.atomic(path,storage.json_bytes(fixture_catalog()))
        provider=self.workspace.providers['capsules'];provider.configure({'catalog':str(path)})
        state=provider.request('GET','/api/bootstrap',{},{});old=state['catalog_revision']
        machine=copy.deepcopy(fixture_catalog()['fixed_machines'][0]);machine['name']='Manager capsule';machine['price']=1300
        state=provider.request('POST','/api/machines/update',{'kind':'fixed_machines','id':101,'machine':machine,'expected_revision':old},{})
        current=state['catalog_revision'];self.assertNotEqual(old,current)
        with self.assertRaises(Exception):provider.request('POST','/api/machines/update',{'kind':'fixed_machines','id':101,'machine':machine,'expected_revision':old},{})
        state=provider.request('POST','/api/machines/retire',{'kind':'fixed_machines','id':101,'expected_revision':current},{})
        state=provider.request('POST','/api/machines/restore',{'kind':'fixed_machines','id':101,'expected_revision':state['catalog_revision']},{})
        record=provider.request('GET','/api/machine?kind=fixed_machines&id=101',{}, {})
        self.assertTrue(record['machine']['enabled']);self.assertEqual(record['machine']['price'],1300)

    def test_standard_setup_hides_developer_data_paths_but_preserves_them(self):
        provider=self.workspace.providers['operators']
        self.workspace.mode='developer';provider.configure({'workspace':str(self.root),'snapshot':'existing-private-snapshot'})
        self.workspace.mode='standard'
        self.assertNotIn('snapshot',provider.describe()['fields'])
        provider.configure({'client':'selected-client'})
        self.assertEqual(provider.config['snapshot'],'existing-private-snapshot')


if __name__=='__main__':unittest.main()
