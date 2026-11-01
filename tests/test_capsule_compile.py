import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from capsule_catalog import ClientData, gdt_records
from capsule_assets import Assets, sprite_frame, standalone_sprite
from capsule_client_compile import build_archive, card_texture, compile_catalog, encode_gdt, put_wide


class NativeFormatTests(unittest.TestCase):
    def test_gdt_row_count_changes_and_native_key_size_boundaries(self):
        for count in (0, 1, 2, 64, 65, 104, 623):
            rows = [struct.pack("<II", 155001 + i, i) for i in range(count)]
            raw = encode_gdt(8, rows)
            self.assertEqual(gdt_records(raw), (8, rows))
            self.assertEqual(struct.unpack_from("<I", raw, 6)[0], count)

    def test_invalid_gdt_row_size_rejected(self):
        with self.assertRaises(ValueError):
            encode_gdt(8, [bytes(7)])

    def test_sprite_short_path_and_annotations_not_texture_masks(self):
        raw = standalone_sprite("txrs\\60000.txr", 183, 107)
        self.assertEqual(len(raw), 176)
        f = sprite_frame(raw)
        self.assertEqual((f["width"], f["height"]), (183, 107))
        self.assertEqual(f["color"], 0xFFFFFFFF)
        self.assertEqual(f["texture"], "txrs\\60000.txr")
        with self.assertRaises(ValueError):
            standalone_sprite("txrs\\" + "1" * 40 + ".txr", 183, 107)

    def test_sprite_atlas_uv_crops_the_selected_portrait(self):
        from PIL import Image
        atlas = Image.new("RGBA", (4, 2), (255, 0, 0, 255))
        for x in (2, 3):
            for y in (0, 1):
                atlas.putpixel((x, y), (0, 255, 0, 255))
        raw = bytearray(standalone_sprite("txrs\\1.txr", 2, 2))
        struct.pack_into("<4f", raw, 20, .5, 0, 1, 1)
        assets = Assets.__new__(Assets)
        class Reader:
            def read_resource(self, name):
                return bytes(raw)
        assets.reader = Reader()
        assets.texture = lambda path: atlas
        result = assets.sprite("sprs\\1.spr")
        self.assertEqual(result.size, (2, 2))
        self.assertTrue(all(pixel == (0, 255, 0, 255) for pixel in result.getdata()))

    def test_padded_card_texture_preserves_native_card_pixels_without_new_gutters(self):
        from PIL import Image
        original = Image.new("RGBA", (183, 107), (17, 34, 51, 255))
        for x in range(183):
            original.putpixel((x, 0), (255, 205, 0, 255))
            original.putpixel((x, 106), (13, 14, 15, 255))
        texture, uv = card_texture(original)
        self.assertEqual(texture.size, (256, 128))
        self.assertEqual(uv, (8/256, 8/128, 191/256, 115/128))
        raw = standalone_sprite("txrs\\60000.txr", 183, 107, uv=uv)
        assets = Assets.__new__(Assets)
        class Reader:
            def read_resource(self, name):
                return raw
        assets.reader = Reader()
        assets.texture = lambda path: texture
        rendered = assets.sprite("sprs\\60000.spr")
        self.assertEqual(rendered.size, original.size)
        self.assertEqual(rendered.tobytes(), original.tobytes())

    def test_linked_overlay_archive_round_trip(self):
        # A deterministic non-compressible stream forces multiple chunk links.
        import random
        rng = random.Random(123)
        raw = bytes(rng.randrange(256) for _ in range(4096))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = build_archive({"gdrs/0033.gdt": raw}, root / "GPackData")
            self.assertGreater(archive["entries"][0]["chunks"], 1)
            self.assertEqual(ClientData(root).read("gdrs/0033.gdt"), raw)

    def test_native_string_field_preserves_terminator(self):
        row = bytearray(100)
        put_wide(row, 0, 100, "A" * 49)
        self.assertEqual(row[-2:], bytes(2))
        with self.assertRaises(ValueError):
            put_wide(row, 0, 100, "A" * 50)

    def test_compiled_machine_keeps_native_card_pair_and_gp_price_without_coin_variant(self):
        native_fixed = bytearray(1019)
        struct.pack_into("<I", native_fixed, 5, 101)
        native_coin = bytearray(114)
        struct.pack_into("<I", native_coin, 5, 102)
        original = {
            "0033": encode_gdt(1019, [bytes(native_fixed)]),
            "0101": encode_gdt(114, [bytes(native_coin)], extra=6),
            "0154": encode_gdt(231, [], extra=30),
            "0155": encode_gdt(18, [], extra=35),
            "1161": b"unpatched-ui-fixture",
        }
        class Reader:
            entries = {}
            index_hash = "0" * 64
            def read_resource(self, name):
                return original[Path(name).stem]
        class NativeAssets:
            reader = Reader()
            def __init__(self, *args):
                pass
            def card_sprite_paths(self, uid):
                return (f"sprs\\{100 + uid * 2}.spr", f"sprs\\{101 + uid * 2}.spr")
            def card(self, *args):
                raise AssertionError("Native capsule sprites must not be repainted")
        catalog = {
            "schema_version": 1,
            "units": [{"id": uid, "name": str(uid), "rank": 3,
                       "capsule_sprites": [f"sprs\\{100+uid*2}.spr", f"sprs\\{101+uid*2}.spr"],
                       "subgrade": 4 if uid == 1 else 2} for uid in range(1, 7)],
            "fixed_machines": [{"id": 7901, "name": "New", "enabled": True,
                "currency": "GP", "price": 1234,
                "slots": [{"unit_id": uid, "weight": weight}
                          for uid, weight in zip(range(1, 7), [20, 80, 250, 250, 250, 150])]}],
            "custom_machines": [],
        }
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary) / "compiled"
            with patch("capsule_client_compile.Assets", NativeAssets), patch(
                "capsule_client_compile.patch_capsule_price_defaults",
                return_value=(b"patched-ui-fixture", {"test_fixture": True}),
            ):
                result = compile_catalog(catalog, "client", "lookup.json", out)
            size, rows = gdt_records((out / "data/0033.gdt").read_bytes())
            self.assertEqual(size, 1019)
            self.assertEqual(struct.unpack_from("<I", rows[0], 15)[0], 1234)
            self.assertEqual(rows[0][19:25], bytes([1, 0, 0, 0, 0, 0]))
            first_pair = [rows[0][59 + j * 60:119 + j * 60].decode("utf-16-le").split("\0")[0]
                          for j in range(2)]
            self.assertEqual(first_pair, ["sprs\\102.spr", "sprs\\103.spr"])
            self.assertEqual(gdt_records((out / "data/0101.gdt").read_bytes())[1], [])
            self.assertEqual(result["generated_cards"], 0)
            self.assertFalse((out / "assets").exists())

            # Replacing one pose must retain the other pose's authored SPR.
            import base64
            import json
            from PIL import Image
            from capsule_portraits import make_override
            png = io.BytesIO()
            Image.new("RGBA", (163, 64), (22, 88, 144, 255)).save(png, "PNG")
            catalog["units"][0]["grade"] = "AR"
            catalog["portrait_overrides"] = {"1": {"0": make_override(
                base64.b64encode(png.getvalue()).decode("ascii"), [0, 0, 163, 64])}}

            class UploadedAssets(NativeAssets):
                def has_portrait_override(self, uid, variant=None):
                    return uid == 1 and variant in (None, 0)
                def card(self, uid, variant):
                    if (uid, variant) != (1, 0):
                        raise AssertionError("Unedited native poses must retain their SPR")
                    return Image.new("RGBA", (183, 107), (22, 88, 144, 255))

            uploaded_out = Path(temporary) / "uploaded"
            with patch("capsule_client_compile.Assets", UploadedAssets), patch(
                "capsule_client_compile.patch_capsule_price_defaults",
                return_value=(b"patched-ui-fixture", {"test_fixture": True}),
            ):
                uploaded_result = compile_catalog(catalog, "client", "lookup.json", uploaded_out)
            uploaded_row = gdt_records((uploaded_out / "data/0033.gdt").read_bytes())[1][0]
            uploaded_pair = [uploaded_row[59+j*60:119+j*60].decode("utf-16-le").split("\0")[0]
                             for j in range(2)]
            self.assertEqual(uploaded_pair, ["sprs\\60000.spr", "sprs\\103.spr"])
            self.assertEqual(uploaded_result["generated_cards"], 1)
            manifest = json.loads((uploaded_out / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["generated_cards"][0]["source"], "native_frame_and_uploaded_png")
            self.assertEqual(manifest["generated_cards"][0]["uploaded_png_sha256"],
                             catalog["portrait_overrides"]["1"]["0"]["sha256"])


if __name__ == "__main__":
    unittest.main()
