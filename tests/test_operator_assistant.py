import base64
import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request,urlopen

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from assistant_modules.operators import model,server
from assistant_modules.operators.assets import validate_png,sprite_binary,put_wchar
from assistant_modules.operators.workspace import Workspace
from mod_assistant import make_server

PNG=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9Zl1sAAAAASUVORK5CYII=')


def fixture():
    return {'schema_version':1,'native_operator_ids':[60001,60003],'client_index_sha256':'fixture',
            'uploaded_assets':{},'native_server_data':{},
            'items':[{'id':1210001,'name':'operator','ref_index':60001,'time_limit':0},
                     {'id':1210002,'name':'operator A','ref_index':60003,'time_limit':1},
                     {'id':2220010,'name':'EXP','ref_index':3240010,'time_limit':0}],
            'operators':[{'id':60003,'category_id':1210002,'template_id':60003,'name':'副官A',
                          'managed':False,'enabled':True,'delivery':'product',
                          'avatar':{'kind':'native','operator_id':60003},
                          'drops':[{'category_id':2220010,'chance_bps':2500,'min_amount':2,'max_amount':5}]}]}


class ModelTests(unittest.TestCase):
    def test_percent_exactness(self):
        self.assertEqual(model.percent_bps('1.25'),125)
        self.assertEqual(model.percent_bps(100),10000)
        for v in ['0.001','NaN',-1,101,True]:
            with self.assertRaises(ValueError):model.percent_bps(v)

    def test_probabilities_have_exact_boundaries_and_remainder(self):
        op=fixture()['operators'][0]
        for q in range(4):self.assertEqual(model.choose_drop(op,2499,q),(2220010,2+q))
        self.assertIsNone(model.choose_drop(op,2500))
        self.assertIsNone(model.choose_drop(op,9999))
        self.assertEqual(sum(model.choose_drop(op,r) is not None for r in range(10000)),2500)

    def test_invalid_probability_or_quantity_does_not_change_original(self):
        c=fixture();before=copy.deepcopy(c)
        for changes in [{'drops':[{'category_id':2220010,'chance_bps':10001,'min_amount':1,'max_amount':1}]},
                        {'drops':[{'category_id':2220010,'chance_bps':1,'min_amount':5,'max_amount':4}]},
                        {'drops':[{'category_id':999,'chance_bps':1,'min_amount':1,'max_amount':1}]},
                        {'delivery':'sale'},{'name':'x'*49},{'price':100}]:
            with self.assertRaises(ValueError):model.update_operator(c,60003,changes)
        self.assertEqual(c,before)

    def test_registration_does_not_list_or_price_anything(self):
        c=model.update_operator(fixture(),60003,{'delivery':'product'})
        reg=model.registrations(c)
        self.assertEqual(reg['items'][0]['registration'],'product')
        self.assertFalse(reg['storefront_changes'])
        self.assertNotIn('price',json.dumps(reg))
        self.assertNotIn('f_productid',json.dumps(reg))
        c=model.update_operator(c,60003,{'delivery':'all_players'})
        self.assertEqual(model.registrations(c)['items'][0]['registration'],'free_for_all')

    def test_new_operator_uses_native_template_and_distinct_item(self):
        c,id=model.create_operator(fixture(),60003)
        self.assertEqual(id,60004)
        op=c['operators'][-1]
        self.assertEqual(op['template_id'],60003)
        self.assertEqual(op['category_id'],1219004)
        self.assertEqual(op['delivery'],'product')
        self.assertEqual(fixture()['operators'][0]['managed'],False)

    def test_unicode_quotes_and_controls(self):
        c=model.update_operator(fixture(),60003,{'name':'补给 "希望" 🙂'})
        self.assertEqual(c['operators'][0]['name'],'补给 "希望" 🙂')
        with self.assertRaises(ValueError):model.update_operator(c,60003,{'name':'first\n[bad]'})

    def test_native_prelude_is_preserved_once_and_only_allowed_procs_wrap(self):
        original='CREATE PROC sp_OperatorDrop AS\nSET NOCOUNT ON\nDECLARE @OperatorNumber INT\nUPDATE account SET bonus=bonus+1000\nDECLARE @v_Probability INT\nSELECT 1'
        wrapped=server.wrap('sp_OperatorDrop',original)
        self.assertEqual(wrapped.count('bonus=bonus+1000'),1)
        self.assertLess(wrapped.index('bonus=bonus+1000'),wrapped.index(server.MARKER))
        self.assertIn('sdgo_OperatorManagedDrop',wrapped)
        with self.assertRaises(ValueError):server.wrap('sp_StoreItemsList',original)
        with self.assertRaises(ValueError):server.wrap('sp_OperatorDrop',wrapped)

    def test_unsupported_proc_fails_closed(self):
        with self.assertRaises(ValueError):server.wrap('sp_OperatorDrop','CREATE PROC sp_OperatorDrop AS SELECT 1')

    def test_host_ini_supports_unicode_and_literal_percent(self):
        with tempfile.TemporaryDirectory() as d:
            for encoding in ['utf-8-sig','utf-16']:
                p=Path(d)/'server.ini'
                p.write_text('[DB Game]\nDBName=test\nID=owner\nPWD=test%literal};\n',encoding=encoding)
                self.assertEqual(server.read_ini(p)['DB Game']['PWD'],'test%literal};')

    def test_native_sprite_paths_and_png_validation(self):
        # A decoder-generated small PNG avoids relying on an external image service.
        from PIL import Image
        b=io.BytesIO();Image.new('RGBA',(2,3),(20,30,40,128)).save(b,'PNG')
        self.assertEqual(validate_png(b.getvalue()),(2,3))
        with self.assertRaises(ValueError):validate_png(b'not an image')
        spr=sprite_binary('sdgo\\mods\\operator_catalog\\assets\\60003.txr',128,134)
        self.assertEqual(len(spr),176)
        self.assertIn('60003.txr'.encode('utf-16-le'),spr)

    def test_fresh_assistant_recovers_installed_custom_definitions(self):
        import struct
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);game=root/'client';snapshot=root/'source';(snapshot/'GOnlineGame').mkdir(parents=True)
            op=copy.deepcopy(fixture()['operators'][0]);op.update(id=60047,category_id=1219047,name='可移植副官',managed=True)
            sidecar=game/'sdgo/mods/operator_catalog/definitions.json'
            model.save(sidecar,{'schema_version':1,'revision':'published','client_index_sha256':'fixture',
                                'operators':[op],'uploaded_assets':{}})
            model.save(snapshot/'GOnlineGame/dbo.tbl_OperatorData.json',[{'OperatorID':60003},{'OperatorID':60047}])
            model.save(snapshot/'GOnlineGame/dbo.tbl_OperatorDropDice.json',[])
            model.save(snapshot/'GOnlineGame/dbo.tbl_ItemCategory.json',[
                {'CatalogueID':1210002,'Name':'native','RefIndex':60003,'TimeLimit':1},
                {'CatalogueID':1219047,'Name':'registered','RefIndex':60047,'TimeLimit':0},
                {'CatalogueID':2220010,'Name':'EXP','RefIndex':3240010,'TimeLimit':0}])
            model.save(snapshot/'GOnlineGame/dbo.sdgo_OperatorProfiles.json',[{'OperatorID':60047,'Enabled':True,'Revision':'published'}])
            row=bytearray(1671);struct.pack_into('<I',row,0,60003);put_wchar(row,5,100,'原生副官')
            class Data:
                index_hash='fixture'
                def records(self,*args):return {60003:bytes(row)}
            data=Data();data.root=game
            recovered=model.import_catalog(snapshot,data)
            self.assertEqual(recovered['native_operator_ids'],[60003])
            self.assertEqual(recovered['operators'][-1]['name'],'可移植副官')
            self.assertEqual(recovered['operators'][-1]['template_id'],60003)
            model.save(snapshot/'GOnlineGame/dbo.sdgo_OperatorProfiles.json',[{'OperatorID':60047,'Enabled':True,'Revision':'newer'}])
            with self.assertRaises(ValueError):model.import_catalog(snapshot,data)


class FakeAssets:
    index_hash='fixture'
    def portrait(self,id):return PNG


class HTTPTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.ws=Workspace.__new__(Workspace)
        self.ws.root=Path(self.temp.name);self.ws.path=self.ws.root/'operators.json'
        self.ws.assets=FakeAssets();self.ws.baseline=fixture();self.ws.server_ini=None;self.ws.module_binary=None
        model.save(self.ws.path,fixture())
        self.http=make_server(self.ws)
        self.thread=threading.Thread(target=self.http.serve_forever,daemon=True);self.thread.start()
        self.origin=f'http://127.0.0.1:{self.http.server_port}'
        self.initial=json.load(urlopen(self.origin+'/api/operators'))

    def tearDown(self):
        self.http.shutdown();self.http.server_close();self.thread.join();self.temp.cleanup()

    def put(self,payload,token=True,origin=None,revision=None,host=None):
        headers={'Content-Type':'application/json','Origin':origin or self.origin,
                 'X-Catalog-Revision':revision or self.initial['revision']}
        if token:headers['X-Assistant-Token']=self.initial['session_token']
        if host:headers['Host']=host
        return urlopen(Request(self.origin+'/api/operators/60003',json.dumps(payload).encode(),headers,method='PUT'))

    def test_save_free_registration_and_reload(self):
        result=json.load(self.put({'name':'免费补给','delivery':'all_players'}))
        self.assertEqual(result['operators'][0]['delivery'],'all_players')
        saved=model.load(self.ws.path)
        self.assertEqual(saved['operators'][0]['name'],'免费补给')
        self.assertFalse(result['capabilities']['server_apply'])
        self.assertTrue((self.ws.root/'product-registrations.json').exists())

    def test_foreign_origin_token_and_host_cannot_write(self):
        for kw in [{'token':False},{'origin':'https://unrelated.invalid'},{'host':'unrelated.invalid'}]:
            with self.assertRaises(HTTPError) as caught:self.put({'name':'bad'},**kw)
            self.assertEqual(caught.exception.code,403)
            caught.exception.close()
        self.assertEqual(model.load(self.ws.path),fixture())

    def test_stale_editor_cannot_overwrite_new_version(self):
        json.load(self.put({'name':'first save'}))
        with self.assertRaises(HTTPError) as caught:self.put({'name':'stale save'})
        caught.exception.close()
        self.assertEqual(model.load(self.ws.path)['operators'][0]['name'],'first save')


if __name__=='__main__':unittest.main()
