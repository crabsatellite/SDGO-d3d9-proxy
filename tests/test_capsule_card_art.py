from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from capsule_assets import Assets, sprite_frame, standalone_sprite


class CapsuleCardArtTests(unittest.TestCase):
    def assets(self, unit_sprites=None, capsule_sprites=None, native_sprites=None):
        assets = Assets.__new__(Assets)
        assets.lookup = {"42": unit_sprites or ["sprs\\unit-a.spr", "sprs\\unit-b.spr"]}
        assets.capsule_lookup = (
            {"42": capsule_sprites} if capsule_sprites is not None else {})
        assets.units = {
            42: {
                "id": 42,
                "name": "Test Unit",
                "grade": "AR",
                "capsule_sprites": native_sprites or [],
            }
        }
        class Reader:
            @staticmethod
            def read_resource(_path):
                return standalone_sprite("txrs\\card.txr", 183, 107)
        assets.reader = Reader()
        seen = []
        assets.sprite = lambda path: seen.append(path) or path
        assets.portrait = lambda *_args, **_kwargs: self.fail(
            "whole-body portrait fallback must not be used for capsule cards")
        return assets, seen

    def test_capsule_lookup_supplies_both_resident_card_states(self):
        assets, seen = self.assets(
            capsule_sprites=["sprs\\card-default.spr", "sprs\\card-alternate.spr"])
        self.assertEqual(
            assets.card_sprite_paths(42),
            ("sprs\\card-default.spr", "sprs\\card-alternate.spr"))
        self.assertEqual(assets.card_provenance(42), "legacy-generated-resident")
        self.assertEqual(seen, [])

    def test_gdt_card_pair_has_priority_over_reference_lookup(self):
        assets, seen = self.assets(
            capsule_sprites=["sprs\\reference-0.spr", "sprs\\reference-1.spr"],
            native_sprites=["sprs\\gdt-0.spr", "sprs\\gdt-1.spr"])
        self.assertEqual(assets.card(42, 0), "sprs\\gdt-0.spr")
        self.assertEqual(assets.card(42, 1), "sprs\\gdt-1.spr")
        self.assertEqual(seen, ["sprs\\gdt-0.spr", "sprs\\gdt-1.spr"])
        self.assertEqual(assets.card_provenance(42), "gdt-native")

    def test_missing_resident_card_pair_reports_missing(self):
        assets, _ = self.assets()
        with self.assertRaisesRegex(ValueError, "No resident capsule card pair"):
            assets.card_sprite_paths(42)
        self.assertEqual(assets.card_provenance(42), "missing")

    def test_card_lookup_requires_an_exact_native_sprite_pair(self):
        assets, _ = self.assets(capsule_sprites=["sprs\\only-one.spr"])
        with self.assertRaisesRegex(ValueError, "Invalid capsule sprites"):
            assets.card_sprite_paths(42)

    def test_card_lookup_rejects_a_full_body_sprite_geometry(self):
        assets, _ = self.assets(
            capsule_sprites=["sprs\\unit-a.spr", "sprs\\unit-b.spr"])
        assets.reader.read_resource = lambda _path: standalone_sprite(
            "txrs\\unit.txr", 256, 256)
        with self.assertRaisesRegex(ValueError, "unexpected dimensions"):
            assets.card_sprite_paths(42)

    def test_standalone_sprite_accepts_exact_padded_texture_uv(self):
        expected = (8 / 256, 8 / 128, 191 / 256, 115 / 128)
        raw = standalone_sprite(
            "txrs\\60000.txr", 183, 107, uv=expected)
        actual = sprite_frame(raw)
        for got, want in zip(actual["uv"], expected):
            self.assertAlmostEqual(got, want, places=7)
        self.assertEqual(__import__("struct").unpack_from(
            "<f", raw, len(raw) - 4)[0], 1.0)
        with self.assertRaisesRegex(ValueError, "Invalid runtime sprite UV"):
            standalone_sprite(
                "txrs\\60000.txr", 183, 107, uv=(0, 0, 1.1, 1))


class ReconstructedCapsuleCardTests(unittest.TestCase):
    def assets(self, grade="AR", subgrade=4):
        from PIL import Image, ImageDraw
        assets = Assets.__new__(Assets)
        assets.lookup = {
            "42": ["sprs\\unit-0.spr", "sprs\\unit-1.spr"],
        }
        assets.capsule_lookup = {
            "42": ["sprs\\legacy-0.spr", "sprs\\legacy-1.spr"],
        }
        assets.units = {
            1: {
                "id": 1, "name": "Frame donor", "grade": grade[0],
                "subgrade": 2,
                "capsule_sprites": ["sprs\\native-0.spr", "sprs\\native-1.spr"],
            },
            42: {
                "id": 42, "name": "Target", "grade": grade,
                "subgrade": subgrade, "capsule_sprites": [],
            },
        }
        textures = {}
        native_paths = {}
        for variant in range(2):
            card = Image.new("RGBA", (183, 107), (255, 0, 255, 0))
            draw = ImageDraw.Draw(card)
            draw.rectangle((4, 3, 178, 103), fill=(55, 57, 31, 255))
            draw.rectangle((10, 10, 172, 73), fill=(4, 74, 156, 255))
            draw.rectangle((10, 79, 172, 97), fill=(12, 14, 12, 255))
            draw.polygon(
                [(10, 10), (96, 10), (79, 23), (10, 23)],
                fill=(82, 83, 28, 255))
            draw.point((15, 14), fill=(250, 225, 0, 255))
            draw.rectangle(
                (64 + variant * 30, 28, 85 + variant * 30, 63),
                fill=(180, 20, 20, 255))
            draw.line((10, 69, 172, 69), fill=(210, 210, 210, 255))
            textures[f"txrs\\native-{variant}.txr"] = card
            native_paths[f"sprs\\native-{variant}.spr"] = standalone_sprite(
                f"txrs\\native-{variant}.txr", 183, 107)

            unit = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
            unit_draw = ImageDraw.Draw(unit)
            # Red head/upper chest; green legs must be excluded by head crop.
            unit_draw.rectangle(
                (74 + variant * 8, 42, 181 + variant * 8, 105),
                fill=(240, 30 + variant * 30, 20, 255))
            unit_draw.rectangle(
                (92 + variant * 8, 106, 162 + variant * 8, 205),
                fill=(10, 220, 20, 255))
            textures[f"txrs\\unit-{variant}.txr"] = unit
            native_paths[f"sprs\\unit-{variant}.spr"] = standalone_sprite(
                f"txrs\\unit-{variant}.txr", 256, 256)
        for variant in range(2):
            native_paths[f"sprs\\legacy-{variant}.spr"] = standalone_sprite(
                f"txrs\\legacy-{variant}.txr", 183, 107)
            textures[f"txrs\\legacy-{variant}.txr"] = Image.new(
                "RGBA", (183, 107), (1, 2, 3, 255))

        class Reader:
            @staticmethod
            def read_resource(path):
                return native_paths[path]
        assets.reader = Reader()
        assets.texture = lambda path: textures[path].copy()
        header = Image.new("RGBA", assets.CARD_SIZE, (0, 0, 0, 0))
        header_draw = ImageDraw.Draw(header)
        header_draw.polygon(
            [(10, 10), (96, 10), (79, 23), (10, 23)],
            fill=(82, 83, 28, 255))
        header.putpixel((15, 14), (250, 225, 0, 255))
        assets._native_header_overlay = lambda _grade: header
        legal = Image.new("RGBA", assets.CARD_SIZE, (0, 0, 0, 0))
        legal.putpixel((15, 68), (120, 120, 120, 255))
        legal.putpixel((16, 68), (120, 120, 120, 255))
        legal.putpixel((15, 71), (100, 100, 100, 255))
        legal.putpixel((16, 71), (100, 100, 100, 255))
        assets._native_legal_overlay = lambda _grade: legal
        return assets

    def test_generated_card_uses_real_frame_and_a_head_crop(self):
        assets = self.assets(grade="A", subgrade=2)
        card = assets.card(42, 0)
        self.assertEqual(card.size, (183, 107))
        self.assertEqual(card.getpixel((15, 14)), (250, 225, 0, 255))
        self.assertEqual(card.getpixel((20, 85)), (12, 14, 12, 255))
        colors = set(card.crop(assets.CARD_PICTURE_BOX).getdata())
        self.assertTrue(any(red > 180 and green < 100 for red, green, _, a in colors if a))
        self.assertFalse(any(green > 170 and red < 80 for red, green, _, a in colors if a),
                         "lower-body legs leaked into the head/upper-chest crop")

    def test_r_mask_changes_robot_but_not_frame_or_background(self):
        from PIL import ImageChops
        normal = self.assets(grade="A", subgrade=2)
        rare = self.assets(grade="AR", subgrade=4)
        normal_card = normal.card(42, 0)
        rare_card = rare.card(42, 0)
        for point in ((15, 14), (20, 85), (176, 50)):
            self.assertEqual(normal_card.getpixel(point), rare_card.getpixel(point))
        difference = ImageChops.difference(
            normal_card.convert("RGB"), rare_card.convert("RGB")).getbbox()
        self.assertIsNotNone(difference)
        left, top, right, bottom = difference
        self.assertGreaterEqual(left, normal.CARD_PICTURE_BOX[0])
        self.assertGreaterEqual(top, normal.CARD_PICTURE_BOX[1])
        self.assertLessEqual(right, normal.CARD_PICTURE_BOX[2])
        self.assertLessEqual(bottom, normal.CARD_PICTURE_BOX[3])
        self.assertNotEqual(normal_card.getpixel((90, 45)),
                            rare_card.getpixel((90, 45)))
        self.assertLess(sum(rare_card.getpixel((90, 45))[:3]),
                        sum(normal_card.getpixel((90, 45))[:3]))

    def test_source_pair_produces_two_distinct_closeups(self):
        assets = self.assets(grade="A", subgrade=2)
        self.assertNotEqual(assets.card(42, 0).tobytes(),
                            assets.card(42, 1).tobytes())

    def test_obstructed_action_pose_can_reuse_same_unit_clear_pose(self):
        assets = self.assets(grade="A", subgrade=2)
        original = assets._source_art
        seen = []
        assets._source_art = (
            lambda uid, variant:
            seen.append((uid, variant)) or original(uid, variant))
        assets.HEAD_SOURCE_VARIANTS = {(42, 1): 0}
        assets.HEAD_ANCHORS = {
            (42, 1): (140 / 256, 75 / 256, 100 / 256)}
        assets.card(42, 1)
        self.assertEqual(seen, [(42, 0)])

    def test_normalized_anchor_tracks_the_same_face_at_128_and_256(self):
        from PIL import Image, ImageDraw

        assets = self.assets(grade="A", subgrade=2)
        assets.HEAD_ANCHORS = {
            (42, 0): (176 / 256, 72 / 256, 96 / 256)}

        def source(size):
            image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
            draw = ImageDraw.Draw(image)
            # Same unit composition at two shipped source resolutions. The
            # red face is intentionally far from the physical pixel x=176:
            # treating the authoring-grid anchor as raw pixels misses it in
            # the 128 source.
            scale = size / 256
            draw.rectangle(
                tuple(round(value * scale) for value in (150, 48, 202, 96)),
                fill=(240, 20, 10, 255))
            draw.rectangle(
                tuple(round(value * scale) for value in (130, 97, 220, 184)),
                fill=(20, 180, 30, 255))
            return image

        crops = []
        for size in (128, 256):
            assets._source_art = lambda _uid, _variant, size=size: source(size)
            crops.append(assets._head_crop(42, 0))

        for crop in crops:
            center = crop.crop((55, 8, 108, 52))
            red_pixels = sum(
                1 for red, green, blue, alpha in center.getdata()
                if alpha and red > 180 and green < 80 and blue < 80)
            self.assertGreater(
                red_pixels, 300,
                "normalized anchor must keep the face in the portrait")

    def test_header_recovery_keeps_exact_diagonal_without_donor_subject(self):
        from PIL import Image, ImageDraw
        grade_colors = {
            "A": (82, 83, 28, 255),
            "B": (70, 82, 91, 255),
            "C": (25, 73, 102, 255),
        }
        samples = {}
        for grade, color in grade_colors.items():
            cards = []
            for variant in range(2):
                image = Image.new(
                    "RGBA", Assets.CARD_SIZE, (4, 74, 156, 255))
                draw = ImageDraw.Draw(image)
                for y in range(10, 23):
                    draw.line((10, y, 86 - (y - 10), y), fill=color)
                # A changing bright subject immediately below the diagonal
                # must never become part of the recovered rank overlay.
                draw.rectangle(
                    (87, 15, 105, 25),
                    fill=(220 if variant else 20, 30, 40, 255))
                cards.append(image)
            samples[grade] = cards
        overlays = Assets._header_overlay_from_grade_samples(samples)
        self.assertEqual(overlays["A"].getpixel((20, 15)), grade_colors["A"])
        self.assertEqual(overlays["A"].getpixel((81, 15)), grade_colors["A"])
        self.assertEqual(overlays["A"].getpixel((82, 15))[3], 0)
        self.assertEqual(overlays["A"].getpixel((90, 20))[3], 0)

    def test_native_chrome_and_picture_edges_gain_no_new_gap(self):
        assets = self.assets(grade="A", subgrade=2)
        frame = assets._native_frame("A")
        card = assets.card(42, 0)
        left, top, right, bottom = assets.CARD_PICTURE_BOX
        for y in range(card.height):
            for x in range(card.width):
                if not (left <= x < right and top <= y < bottom):
                    self.assertEqual(
                        card.getpixel((x, y)), frame.getpixel((x, y)),
                        f"native chrome changed at {(x, y)}")
        for x in range(left, right):
            self.assertEqual(card.getpixel((x, top))[3], 255)
            self.assertEqual(card.getpixel((x, bottom - 1))[3], 255)
        for y in range(top, bottom):
            self.assertEqual(card.getpixel((left, y))[3], 255)
            self.assertEqual(card.getpixel((right - 1, y))[3], 255)

    def test_robot_continues_behind_footer_glyph_gaps(self):
        assets = self.assets(grade="A", subgrade=2)
        frame = assets._native_frame("A")
        card = assets.card(42, 0)
        # The recovered legal glyph is foreground.
        self.assertEqual(card.getpixel((15, 68)), (120, 120, 120, 255))
        # Its neighboring gap retains the target robot, rather than receiving
        # the native frame's blue footer strip.
        self.assertNotEqual(card.getpixel((20, 69)), frame.getpixel((20, 69)))
        red, green, _blue, alpha = card.getpixel((20, 69))
        self.assertEqual(alpha, 255)
        self.assertGreater(red, 180)
        self.assertLess(green, 100)

    def test_legal_recovery_returns_sparse_glyph_alpha_only(self):
        from PIL import Image
        samples = []
        for index in range(10):
            image = Image.new("RGBA", Assets.CARD_SIZE, (0, 0, 0, 255))
            # Vary every footer background so it cannot win the native vote.
            for y in range(67, 74):
                for x in range(10, 173):
                    image.putpixel(
                        (x, y),
                        ((index * 31 + x * 7) % 90,
                         (index * 47 + y * 9) % 120,
                         80 + (index * 23 + x) % 120, 255))
            # A fixed two-row neutral-gray glyph survives all ten cards.
            for point in ((15, 67), (16, 67), (15, 68), (16, 68)):
                image.putpixel(point, (90, 91, 92, 255))
            # An isolated fixed neutral star must not become foreground.
            image.putpixel((80, 70), (220, 220, 220, 255))
            samples.append(image)
        overlay = Assets._legal_overlay_from_samples(samples)
        self.assertEqual(overlay.getpixel((15, 67)), (90, 91, 92, 255))
        self.assertEqual(overlay.getpixel((16, 68)), (90, 91, 92, 255))
        self.assertEqual(overlay.getpixel((17, 67))[3], 0)
        self.assertEqual(overlay.getpixel((80, 70))[3], 0)
        self.assertLess(
            sum(1 for alpha in overlay.getchannel("A").getdata() if alpha),
            10,
            "legal recovery must never recreate an opaque footer rectangle")

    def test_legacy_resident_pair_is_not_reused_as_generated_art(self):
        assets = self.assets(grade="A", subgrade=2)
        self.assertEqual(assets.card_provenance(42), "legacy-generated-resident")
        self.assertNotEqual(assets.card(42, 0).getpixel((90, 45)),
                            (1, 2, 3, 255))


if __name__ == "__main__":
    unittest.main()
