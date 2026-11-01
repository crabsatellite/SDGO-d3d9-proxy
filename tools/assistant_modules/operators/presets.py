"""Portable defaults, selected by native IDs and typed references, never SQL names."""
import copy
from pathlib import Path
from . import assets, model

VERSION = 1
NAMES = {60003: "设计图补给官·希美", 60013: "经验补给官·瑞希", 60018: "装饰补给官·彩",
         60025: "强化补给官·惠", 60041: "扭蛋币补给官·富美"}
LEGACY_ALIASES = {60003: "Nozomi(PlanMix)", 60013: "Mizuki(EXP)", 60018: "Aya(R_Dec)",
                  60025: "Megumi(Upgkit)", 60041: "Tomi(Coin)"}
TABLES = ("tbl_MixTreeFile", "tbl_Units", "tbl_PaintItem", "tbl_StickerItem", "tbl_EtcItems")


def available(snapshot):
    return all((Path(snapshot)/"GOnlineGame"/("dbo."+name+".json")).is_file() for name in TABLES)


def initialize(catalog, snapshot, client_assets):
    result = copy.deepcopy(catalog)
    if result.get('default_pack_version') == VERSION:
        model.validate(result)
        return result
    root = Path(snapshot)/"GOnlineGame"
    source = {name: model.load(root/("dbo."+name+".json")) for name in TABLES}
    items = {r['id']: r for r in result['items']}
    native = client_assets.records('gdrs/0031.gdt', assets.ITEM_SIZE)
    def matches(item):
        row = native.get(item['id'])
        return (row is not None and int.from_bytes(row[105:109], 'little') == item['ref_index']
                and row[109] == item['time_limit'] and item['time_limit'] == 0)
    units = {r['UnitNumber'] for r in source['tbl_Units']}
    recipes = {r['Idx']: r for r in source['tbl_MixTreeFile']
               if r['OutUnitID'] in units and r['UnitID1'] in units
               and all(not r['UnitID'+str(i)] or r['UnitID'+str(i)] in units for i in range(1, 8))}
    paints = {r['PaintID']: r for r in source['tbl_PaintItem']}
    stickers = {r['StickerID'] for r in source['tbl_StickerItem']}
    etc = {r['EtcItemNumber']: r for r in source['tbl_EtcItems']}
    pools = {}
    for key, label, family, target in (
            ('blueprints', '随机有效设计图', 15, recipes),
            ('stickers', '随机贴纸', 17, stickers),
            ('paints', '随机涂装', 16, {k for k, v in paints.items() if v['Type'] in (0, 1)}),
            ('polish', '随机抛光剂', 16, {k for k, v in paints.items() if v['Type'] == 2})):
        # Aliases of one recipe/paint do not give it extra probability.
        unique = {}
        for item in sorted(items.values(), key=lambda r:r['id']):
            if item['id']//100000 == family and item['ref_index'] in target and matches(item):
                unique.setdefault(item['ref_index'], item['id'])
        if not unique:
            raise ValueError('默认副官缺少经过两端核对的物品池：'+label)
        pools[key] = {'label': label, 'category_ids': sorted(unique.values())}
    for key, pool in pools.items():result.setdefault('drop_pools', {}).setdefault(key,pool)
    pools=result['drop_pools']
    def fixed(identity, chance, low, high, type_id, exp=None):
        item = items.get(identity)
        definition = etc.get(item['ref_index']) if item else None
        if not item or not matches(item) or not definition or definition['Type'] != type_id or (
                exp is not None and definition['ExpSize'] != exp):
            raise ValueError('默认副官的固定物品两端登记不一致：'+str(identity))
        return dict(category_id=identity, chance_bps=chance, min_amount=low, max_amount=high)
    def pooled(key, chance, low, high):
        return dict(category_id=pools[key]['category_ids'][0], pool=key, chance_bps=chance,
                    min_amount=low, max_amount=high)
    # The old scripts resampled these branches every 30 seconds. The same weights
    # and ranges now apply independently to each native operator-drop trigger.
    rules = {
        60003: [pooled('blueprints', 10000, 1, 1)],
        60013: [fixed(2220012,2500,2,10,2,5000), fixed(2220002,2500,5,15,2,1000),
                fixed(2220010,2500,8,12,2,1500), fixed(2220008,2500,10,20,2,200)],
        60018: [pooled('stickers',4000,2,5), pooled('paints',4000,2,5), pooled('polish',2000,1,1)],
        60025: [fixed(2220021,10000,10,30,15)],
    }
    # Preserve the original coin amount/probability, rather than invent a new one.
    coin = next((o for o in result['operators'] if o['id'] == 60041), None)
    if coin is None or len(coin['drops']) != 1 or coin['drops'][0]['category_id'] != 2250019:
        raise ValueError('默认扭蛋币副官的原生掉落配置无法确认')
    old = coin['drops'][0]
    rules[60041] = [fixed(2250019,old['chance_bps'],old['min_amount'],old['max_amount'],8)]
    for identity, name in NAMES.items():
        op = next((o for o in result['operators'] if o['id'] == identity), None)
        if op is None:
            raise ValueError('默认副官不存在：'+str(identity))
        # Imported published definitions, including intentionally disabled ones,
        # are user configuration and must survive a reinstall.
        if not op['managed']:
            op.update(name=name, managed=True, enabled=True, delivery='all_players', drops=rules[identity])
    result['default_pack_version'] = VERSION
    model.validate(result)
    return result
