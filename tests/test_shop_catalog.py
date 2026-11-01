import configparser
import copy
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import shop_catalog as shop


def snapshot():
    return {"products": [{"f_productid": 1010001, "f_itemid": "110001", "f_package": "0", "f_depth1": 1, "f_display": "1"}],
            "options": [{"f_itemid": "110001", "f_categoryNumber": 1110549}],
            "categories": [{"CatalogueID": 1110549, "TimeLimit": 1, "Name": "租赁机体"},
                           {"CatalogueID": 1120022, "TimeLimit": 0, "Name": "永久机体"}]}


def catalog():
    return {"schema_version": 1, "catalog_id": "host-shop", "products": []}


def add(value=None, data=None, **kw):
    args = {"key": "zaku", "category_id": 1120022, "price": 500, "currency": "GB"}
    args.update(kw)
    return shop.create(value or catalog(), data or snapshot(), **args)


class ShopConfiguration(unittest.TestCase):
    def test_create_respects_native_tab_and_sku_kind(self):
        p = add()["products"][0]
        self.assertEqual(p["product_id"], 1019001)
        self.assertEqual(p["item_id"], "190001")
        self.assertEqual(p["label"], "永久")

    def test_rental_duration_remains_explicit(self):
        p = add(category_id=1110549, validity=7, currency="MB")["products"][0]
        self.assertEqual(p["label"], "租赁 7 天")
        self.assertEqual(p["validity"], 7)

    def test_native_id_collision_avoided(self):
        data = snapshot()
        data["products"].append({**data["products"][0], "f_productid": 1019001, "f_itemid": "190001"})
        p = add(data=data)["products"][0]
        self.assertEqual((p["product_id"], p["item_id"]), (1019002, "190002"))

    def test_update_delete_and_identity_stay_separate(self):
        original = add()
        changed = shop.update(original, "zaku", {"price": 900, "currency": "MB"})
        retired = shop.retire(changed, "zaku")
        self.assertTrue(retired["products"][0]["retired"])
        self.assertFalse(retired["products"][0]["enabled"])
        self.assertEqual(original["products"][0]["price"], 500)
        self.assertEqual(changed["products"][0]["product_id"], original["products"][0]["product_id"])
        with self.assertRaises(ValueError): shop.update(original, "zaku", {"product_id": 1010001})

    def test_retired_ids_are_not_reused(self):
        retired = shop.retire(add(), "zaku")
        p = add(retired, key="new")["products"][-1]
        self.assertEqual(p["product_id"], 1019002)

    def test_reject_unusable_free_price_and_bad_currency(self):
        for price in [0, -1, 1.2, True]:
            with self.assertRaises(ValueError): add(price=price)
        with self.assertRaises(ValueError): add(currency="GP")

    def test_permanent_unit_is_not_a_seven_day_sale(self):
        with self.assertRaises(ValueError): add(validity=7)

    def test_unknown_category_and_native_kind_rejected(self):
        with self.assertRaises(ValueError): add(category_id=9999999)
        data = snapshot()
        data["categories"].append({"CatalogueID": 1540001, "Name": "设计图", "TimeLimit": 0})
        with self.assertRaises(ValueError): add(data=data, category_id=1540001)

    def test_ini_injection_and_wrong_unicode_length_rejected(self):
        with self.assertRaises(ValueError): add(name="中文\n[shop]\nSchema=2")
        with self.assertRaises(ValueError): add(name="机"*51)
        with self.assertRaises(ValueError): add(key="a]\n[evil")

    def test_compile_is_utf16_and_portable(self):
        value = add(name="O'Hara; 中文商品")
        root = Path(__file__).resolve().parents[1] / "build" / "shop_config_test"
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="different host ", dir=root) as directory:
            path = Path(directory).resolve()
            self.assertTrue(path.is_relative_to(root.resolve()))
            ini = path / "catalog.ini"
            revision = shop.compile_catalog(value, ini, snapshot())
            self.assertTrue(ini.read_bytes().startswith(b"\xff\xfe"))
            parser = configparser.ConfigParser(interpolation=None)
            parser.read(ini, encoding="utf-16")
            self.assertEqual(parser["product:zaku"]["Name"], "O'Hara; 中文商品")
            self.assertEqual(parser["product:zaku"]["Currency"], "GB")
            self.assertEqual(parser["shop"]["Revision"], revision)
            first = ini.read_bytes()
            shop.compile_catalog(value, ini, snapshot())
            self.assertEqual(first, ini.read_bytes())
            self.assertNotIn(directory, ini.read_text(encoding="utf-16"))

    def test_compile_retired_entry_retires_in_runtime_by_omission(self):
        retired = shop.retire(add(), "zaku")
        root = Path(__file__).resolve().parents[1] / "build" / "shop_config_test"
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root) as directory:
            self.assertTrue(Path(directory).resolve().is_relative_to(root.resolve()))
            path = Path(directory) / "catalog.ini"
            shop.compile_catalog(retired, path)
            self.assertNotIn("[product:", path.read_text(encoding="utf-16"))


if __name__ == "__main__": unittest.main()
