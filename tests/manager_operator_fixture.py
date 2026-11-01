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
