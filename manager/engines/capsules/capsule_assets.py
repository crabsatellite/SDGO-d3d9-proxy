"""Build capsule images from the game's real SPR/texture resources.

SPR UVs and frame color/opacity are applied before composition. No game
file is modified. The output can be consumed by the editor and runtime compiler.
"""
from __future__ import annotations
import argparse
from collections import Counter
import functools
import io
import json
import struct
from pathlib import Path

from .capsule_catalog import ClientData, read_json, write_json


def _normalized_head_anchors(authoring_grid):
    """Convert 256-grid review coordinates to source-relative values."""
    return {
        key: tuple(value / 256 for value in anchor)
        for key, anchor in authoring_grid.items()
    }


def sprite_frame(data, frame=0):
    version, count = struct.unpack_from("<II", data)
    if version not in (10000, 11000) or not 0 <= frame < count <= 1000:
        raise ValueError("Unsupported sprite version/frame")
    stride = 164 if version == 10000 else 172
    pos = 8 + frame * stride
    if len(data) != 8 + count * stride + 4:
        raise ValueError("Unexpected sprite layout")
    duration, height, width = struct.unpack_from("<III", data, pos)
    uv = struct.unpack_from("<4f", data, pos + 12)
    path_offset = pos + 36
    color, opacity = 0xFFFFFFFF, 255
    if version == 11000:
        # CSprite frame +0x0c/+0x10 default to 0xffffffff/255 in the
        # shipped constructor. They are color/opacity, not mirror flags.
        color, opacity = struct.unpack_from("<II", data, path_offset)
        path_offset += 8
    paths = [data[path_offset + i * 64:path_offset + (i + 1) * 64]
             .decode("utf-16-le").split("\0")[0] for i in range(2)]
    if not 0 < width <= 8192 or not 0 < height <= 8192:
        raise ValueError("Invalid sprite dimensions")
    # Several shipped portraits use a one-texel overhang at the image edge.
    # Pillow's crop preserves that overhang as transparent padding.
    if not (-0.02 <= uv[0] < uv[2] <= 1.02 and -0.02 <= uv[1] < uv[3] <= 1.02):
        raise ValueError("Invalid sprite UV")
    return {"width": width, "height": height, "uv": uv, "color": color, "opacity": opacity,
            "texture": paths[0], "annotation": paths[1]}


def standalone_sprite(texture_path, width, height, uv=(0, 0, 1, 1)):
    raw_path = (texture_path + "\0").encode("utf-16-le")
    if len(raw_path) > 64:
        raise ValueError("Runtime texture path exceeds native sprite field")
    if (not isinstance(uv, (tuple, list)) or len(uv) != 4
            or not all(isinstance(value, (int, float)) for value in uv)
            or not 0 <= uv[0] < uv[2] <= 1
            or not 0 <= uv[1] < uv[3] <= 1):
        raise ValueError("Invalid runtime sprite UV")
    header = struct.pack(
        "<IIIII4f2f", 10000, 1, 20, height, width, *uv, 0, 0)
    return header + raw_path.ljust(64, b"\0") + bytes(64) + struct.pack("<f", 1.0)


class Assets:
    CARD_SIZE = (183, 107)
    # Exact stock viewport, verified against native A/B/C cards: x=10..172 and
    # y=10..73. x=5..9 and x=173..177 are intentional native inner-border
    # pixels, not padding to remove. Pillow boxes use exclusive right/bottom.
    CARD_PICTURE_BOX = (10, 10, 173, 74)
    # The stock footer is two tiny rasterized legal-text rows inside the
    # picture. It is foreground text, not a background strip: native robots
    # remain visible through every gap between the glyphs.
    LEGAL_TEXT_BOX = (10, 67, 173, 74)
    # Rank chrome overlaps the top-left of the picture. The extra top/right
    # span includes its one-pixel native top rule and diagonal antialiasing.
    HEADER_RECOVERY_BOX = (10, 10, 111, 30)
    # A few presentation renders put a wing or weapon above the actual face.
    # Anchors are reviewed on a normalized 256x256 authoring grid, then stored
    # source-relative. This is essential because shipped portrait sources use
    # mixed 128, 129x113 and 256 pixel canvases. The third authoring value is
    # crop width; no runtime path assumes a physical 256x256 source.
    # When an action pose places a weapon directly across the face, use the
    # same unit's unobstructed shipped pose with a separately anchored crop.
    HEAD_SOURCE_VARIANTS = {
        (10202, 1): 0,
        (10210, 1): 0,
        (20032, 1): 0,
        (24722, 1): 0,
    }
    HEAD_ANCHORS = _normalized_head_anchors({
        (10086, 0): (128, 94, 90),
        (10086, 1): (125, 95, 95),
        (10101, 0): (138, 88, 90),
        (10101, 1): (122, 90, 90),
        (10113, 0): (128, 98, 85),
        (10113, 1): (126, 98, 85),
        (10202, 0): (127, 96, 95),
        (10202, 1): (136, 96, 95),
        (10208, 0): (128, 128, 150),
        (10208, 1): (118, 128, 150),
        (10210, 0): (127, 96, 100),
        (10210, 1): (136, 96, 100),
        (10219, 0): (126, 105, 112),
        (10219, 1): (132, 104, 112),
        (11013, 0): (126, 60, 105),
        (11013, 1): (121, 58, 105),
        (11016, 0): (128, 64, 95),
        (11016, 1): (125, 64, 95),
        (11046, 0): (163, 72, 95),
        (11046, 1): (122, 76, 95),
        (11048, 0): (128, 96, 100),
        (11048, 1): (122, 96, 100),
        (12007, 0): (124, 68, 100),
        (12007, 1): (119, 72, 100),
        (13005, 0): (130, 72, 95),
        (13005, 1): (131, 72, 95),
        (13009, 0): (125, 72, 100),
        (13009, 1): (154, 72, 100),
        (13036, 0): (122, 78, 95),
        (13036, 1): (122, 78, 95),
        (14016, 0): (128, 108, 112),
        (14016, 1): (129, 108, 118),
        (15066, 0): (130, 90, 100),
        (15066, 1): (128, 90, 100),
        (15086, 0): (128, 94, 95),
        (15086, 1): (130, 94, 95),
        (15089, 0): (128, 92, 100),
        (15089, 1): (132, 92, 100),
        (15102, 0): (127, 96, 95),
        (15102, 1): (140, 96, 95),
        (15114, 0): (134, 104, 100),
        (15114, 1): (134, 104, 100),
        (17081, 0): (130, 96, 90),
        (17081, 1): (130, 96, 90),
        (18018, 0): (132, 88, 110),
        (18018, 1): (128, 88, 110),
        (18027, 0): (133, 92, 105),
        (18027, 1): (128, 92, 105),
        (18028, 0): (128, 106, 100),
        (18028, 1): (127, 106, 100),
        (20032, 0): (128, 94, 95),
        (20032, 1): (136, 94, 95),
        (22029, 0): (128, 100, 90),
        (22029, 1): (128, 100, 90),
        (22050, 0): (130, 100, 95),
        (22050, 1): (128, 94, 95),
        (22127, 0): (128, 100, 95),
        (22127, 1): (128, 96, 95),
        (23024, 0): (128, 104, 90),
        (23024, 1): (128, 104, 90),
        (24601, 0): (126, 102, 95),
        (24601, 1): (125, 100, 95),
        (24605, 0): (128, 90, 105),
        (24605, 1): (120, 94, 105),
        (24702, 0): (128, 105, 95),
        (24702, 1): (130, 100, 100),
        (24713, 0): (128, 82, 108),
        (24713, 1): (130, 83, 108),
        (24716, 0): (130, 103, 100),
        (24716, 1): (158, 108, 100),
        (24720, 0): (126, 88, 105),
        (24720, 1): (126, 91, 110),
        (24722, 0): (128, 94, 90),
        (24722, 1): (136, 94, 90),
    })

    def __init__(self, client, lookup, catalog, capsule_lookup=None):
        self.reader = ClientData(client)
        source = read_json(lookup)
        if "units" in source:
            if source.get("zdx_sha256") != self.reader.index_hash:
                raise ValueError("Unit sprite lookup does not match client index")
            source = source["units"]
        self.lookup = source
        if capsule_lookup is None:
            candidate = Path(lookup).with_name("capsule_spr_lookup.json")
            capsule_lookup = candidate if candidate.is_file() else None
        capsule_source = read_json(capsule_lookup) if capsule_lookup else {}
        if "capsules" in capsule_source:
            if (capsule_source.get("zdx_sha256") is not None
                    and capsule_source["zdx_sha256"] != self.reader.index_hash):
                raise ValueError("Capsule sprite lookup does not match client index")
            capsule_source = capsule_source["capsules"]
        self.capsule_lookup = capsule_source
        self.units = {u["id"]: u for u in catalog["units"]}

    @staticmethod
    def _sprite_pair(source, uid, field):
        paths = source.get(str(uid))
        if isinstance(paths, dict):
            paths = paths.get(field)
        if paths is None or paths == []:
            return None
        if (not isinstance(paths, list) or len(paths) != 2
                or any(not isinstance(path, str) or not path.lower().startswith("sprs\\")
                       or not path.lower().endswith(".spr") for path in paths)):
            raise ValueError(f"Invalid {field.replace('_', ' ')} for unit {uid}")
        return paths

    @functools.lru_cache(maxsize=64)
    def texture(self, path):
        from PIL import Image
        data = self.reader.read_resource(path)
        if data.startswith(b"ZOATEX1\0"):
            data = data[12:]
        image = Image.open(io.BytesIO(data))
        image.load()
        if image.width > 8192 or image.height > 8192:
            raise ValueError("Texture too large")
        return image.convert("RGBA")

    def sprite(self, path):
        from PIL import Image, ImageChops
        frame = sprite_frame(self.reader.read_resource(path))
        image = self.texture(frame["texture"])
        u0, v0, u1, v1 = frame["uv"]
        bounds = (round(u0 * image.width), round(v0 * image.height),
                  round(u1 * image.width), round(v1 * image.height))
        image = image.crop(bounds)
        # The second 64-byte string is an author/editor annotation in these
        # resources (e.g. timestamp+author), not another texture reference.
        if frame["color"] != 0xFFFFFFFF:
            value = frame["color"]
            tint = Image.new("RGBA", image.size, ((value >> 16) & 255, (value >> 8) & 255,
                                                  value & 255, (value >> 24) & 255))
            image = ImageChops.multiply(image, tint)
        if frame["opacity"] != 255:
            if frame["opacity"] > 255:
                raise ValueError("Unsupported sprite opacity")
            image.putalpha(image.getchannel("A").point(lambda a: a * frame["opacity"] // 255))
        if image.size != (frame["width"], frame["height"]):
            image = image.resize((frame["width"], frame["height"]), Image.Resampling.LANCZOS)
        return image

    def portrait(self, uid, variant=0):
        paths = self._sprite_pair(self.lookup, uid, "unit_sprites")
        if not paths:
            raise ValueError(f"No source portrait for unit {uid}")
        return self.sprite(paths[min(variant, len(paths) - 1)])

    def card_provenance(self, uid):
        unit = self.units[uid]
        if self._sprite_pair({str(uid): unit.get("capsule_sprites")},
                             uid, "capsule_sprites"):
            return "gdt-native"
        if self._sprite_pair(self.capsule_lookup, uid, "capsule_sprites"):
            return "legacy-generated-resident"
        return "missing"

    @functools.lru_cache(maxsize=1024)
    def card_sprite_paths(self, uid):
        unit = self.units[uid]
        # Catalog pairs were read directly from the native GDT. The separate
        # lookup also contains older generated-resident 30000+ pairs, so it is
        # useful for inventory/readback but is not evidence of stock authorship.
        paths = self._sprite_pair({str(uid): unit.get("capsule_sprites")},
                                  uid, "capsule_sprites")
        if not paths:
            paths = self._sprite_pair(self.capsule_lookup, uid, "capsule_sprites")
        if not paths:
            raise ValueError(
                f"No resident capsule card pair for unit {uid}")
        for path in paths:
            frame = sprite_frame(self.reader.read_resource(path))
            if not (181 <= frame["width"] <= 183 and 107 <= frame["height"] <= 109):
                raise ValueError(
                    f"Capsule card has unexpected dimensions for unit {uid}: "
                    f"{path} is {frame['width']}x{frame['height']}")
        return tuple(paths)

    def _source_art(self, uid, variant):
        """Read the source texture at its real pixels, before SPR resizing."""
        from PIL import Image, ImageChops
        paths = self._sprite_pair(self.lookup, uid, "unit_sprites")
        if not paths:
            raise ValueError(f"No source unit art for capsule card {uid}")
        frame = sprite_frame(
            self.reader.read_resource(paths[min(variant, len(paths) - 1)]))
        image = self.texture(frame["texture"])
        u0, v0, u1, v1 = frame["uv"]
        bounds = (round(u0 * image.width), round(v0 * image.height),
                  round(u1 * image.width), round(v1 * image.height))
        image = image.crop(bounds)
        if frame["color"] != 0xFFFFFFFF:
            value = frame["color"]
            tint = Image.new(
                "RGBA", image.size,
                ((value >> 16) & 255, (value >> 8) & 255,
                 value & 255, (value >> 24) & 255))
            image = ImageChops.multiply(image, tint)
        if frame["opacity"] != 255:
            if frame["opacity"] > 255:
                raise ValueError("Unsupported sprite opacity")
            image.putalpha(
                image.getchannel("A").point(
                    lambda alpha: alpha * frame["opacity"] // 255))
        return image

    @staticmethod
    def _is_starfield(pixel):
        red, green, blue, alpha = pixel
        if alpha < 192:
            return False
        # White robot armor is common and must never be mistaken for a star.
        # Fixed white stars are still recovered by the high-support consensus
        # branch in _native_frame.
        return (blue >= 38 and blue >= red * 6 // 5
                and blue >= green * 21 // 20)

    def _native_card_samples(self, base_grade):
        samples = []
        for unit in sorted(self.units.values(), key=lambda item: item["id"]):
            if unit.get("grade") != base_grade:
                continue
            paths = self._sprite_pair(
                {str(unit["id"]): unit.get("capsule_sprites")},
                unit["id"], "capsule_sprites")
            if not paths:
                continue
            for path in paths:
                image = self.sprite(path)
                if image.size == self.CARD_SIZE:
                    samples.append(image)
            # More samples improve consensus, but compiling a catalog should
            # not decode every stock card atlas merely to recover one frame.
            if len(samples) >= 48:
                break
        if len(samples) < 2:
            raise ValueError(
                f"No two 183x107 native {base_grade}-rank cards available "
                "to recover capsule chrome")
        return samples

    def _native_rare_samples(self, base_grade):
        """Read true stock R cards whose dark subjects expose fixed footer text."""
        samples = []
        for unit in sorted(self.units.values(), key=lambda item: item["id"]):
            if unit.get("grade") != base_grade + "R":
                continue
            paths = self._sprite_pair(
                {str(unit["id"]): unit.get("capsule_sprites")},
                unit["id"], "capsule_sprites")
            if not paths:
                continue
            # Some GDT rows deliberately reference the same card twice.
            for path in dict.fromkeys(paths):
                image = self.sprite(path)
                if image.size == self.CARD_SIZE:
                    samples.append(image)
            if len(samples) >= 48:
                break
        if len(samples) < 8:
            raise ValueError(
                f"No eight 183x107 native {base_grade}R cards available "
                "to recover capsule footer text")
        return samples

    @classmethod
    def _header_overlay_from_grade_samples(cls, grade_samples):
        """Recover exact rank chrome without copying a donor robot below it."""
        from PIL import Image
        if set(grade_samples) != {"A", "B", "C"}:
            raise ValueError("Native header recovery requires A, B and C samples")
        if any(
                len(samples) < 2
                or any(image.size != cls.CARD_SIZE for image in samples)
                for samples in grade_samples.values()):
            raise ValueError("Native header recovery requires two cards per grade")

        left, top, right, bottom = cls.HEADER_RECOVERY_BOX
        width, height = right - left, bottom - top
        modes = {}
        supports = {}
        for grade, samples in grade_samples.items():
            image = Image.new("RGBA", (width, height))
            support = [[0] * width for _ in range(height)]
            for y in range(height):
                for x in range(width):
                    pixel, count = Counter(
                        sample.getpixel((left + x, top + y))
                        for sample in samples).most_common(1)[0]
                    image.putpixel((x, y), pixel)
                    support[y][x] = count
            modes[grade] = image
            supports[grade] = support

        # Grade-colored banner pixels differ across A/B/C while the picture
        # below them is shared. Requiring 75% within-grade agreement excludes
        # robots. This recovers the exact stepped diagonal instead of guessing
        # it with a polygon.
        fixed = [
            [
                all(
                    supports[grade][y][x] * 4
                    >= len(grade_samples[grade]) * 3
                    for grade in ("A", "B", "C"))
                for x in range(width)
            ]
            for y in range(height)
        ]
        mask = [
            [
                fixed[y][x]
                and len({
                    modes[grade].getpixel((x, y))
                    for grade in ("A", "B", "C")
                }) > 1
                for x in range(width)
            ]
            for y in range(height)
        ]

        # The one-pixel dark top/left rules are common to all grades. Grow only
        # through neutral, high-support pixels touching the colored banner;
        # saturated starfield and donor armor cannot enter this component.
        changed = True
        while changed:
            changed = False
            expanded = [row[:] for row in mask]
            for y in range(height):
                for x in range(width):
                    if mask[y][x] or not fixed[y][x]:
                        continue
                    pixels = [
                        modes[grade].getpixel((x, y))
                        for grade in ("A", "B", "C")
                    ]
                    red, green, blue, alpha = pixels[0]
                    if (len(set(pixels)) != 1 or alpha < 192
                            or max(red, green, blue) - min(red, green, blue) > 35):
                        continue
                    if any(
                            mask[near_y][near_x]
                            for near_y in range(max(0, y - 1), min(height, y + 2))
                            for near_x in range(max(0, x - 1), min(width, x + 2))):
                        expanded[y][x] = True
                        changed = True
            mask = expanded

        overlays = {}
        for grade in ("A", "B", "C"):
            overlay = Image.new("RGBA", cls.CARD_SIZE, (0, 0, 0, 0))
            for y in range(height):
                for x in range(width):
                    if mask[y][x]:
                        overlay.putpixel(
                            (left + x, top + y),
                            modes[grade].getpixel((x, y)))
            if not overlay.getchannel("A").getbbox():
                raise ValueError(f"Native {grade}-rank header recovery was empty")
            overlays[grade] = overlay
        return overlays

    @functools.lru_cache(maxsize=1)
    def _native_header_overlays(self):
        return self._header_overlay_from_grade_samples({
            grade: self._native_card_samples(grade)
            for grade in ("A", "B", "C")
        })

    def _native_header_overlay(self, base_grade):
        return self._native_header_overlays()[base_grade]

    @classmethod
    def _legal_overlay_from_samples(cls, samples):
        """Recover only native footer glyphs, with no donor background pixels."""
        from PIL import Image
        left, top, right, bottom = cls.LEGAL_TEXT_BOX
        width, height = right - left, bottom - top
        if len(samples) < 8 or any(image.size != cls.CARD_SIZE for image in samples):
            raise ValueError("Native legal-text recovery requires eight card samples")

        votes = [[0] * width for _ in range(height)]
        values = [[[] for _ in range(width)] for _ in range(height)]
        for image in samples:
            for row, y in enumerate(range(top, bottom)):
                for column, x in enumerate(range(left, right)):
                    red, green, blue, alpha = image.getpixel((x, y))
                    luminance = (red + green + blue) // 3
                    chroma = max(red, green, blue) - min(red, green, blue)
                    # Stock legal glyphs are neutral gray even on dark R-card
                    # subjects. The saturated blue starfield is deliberately
                    # excluded; unrelated neutral robot pixels disappear in
                    # the cross-card vote below.
                    if alpha >= 192 and luminance >= 35 and chroma <= 65:
                        votes[row][column] += 1
                        values[row][column].append((red, green, blue))

        # A legal glyph is fixed in at least 65% of unrelated native R cards.
        # Requiring a nearby fixed pixel removes isolated stock stars without
        # filling any of the real spaces between letters.
        fixed = [
            [votes[y][x] * 20 >= len(samples) * 13 for x in range(width)]
            for y in range(height)
        ]
        overlay = Image.new("RGBA", cls.CARD_SIZE, (0, 0, 0, 0))
        for y in range(height):
            for x in range(width):
                if not fixed[y][x]:
                    continue
                neighbors = sum(
                    1
                    for nearby_y in range(max(0, y - 1), min(height, y + 2))
                    for nearby_x in range(max(0, x - 2), min(width, x + 3))
                    if (nearby_x != x or nearby_y != y)
                    and fixed[nearby_y][nearby_x])
                if not neighbors:
                    continue
                pixels = values[y][x]
                color = tuple(
                    sorted(pixel[channel] for pixel in pixels)[len(pixels) // 2]
                    for channel in range(3))
                overlay.putpixel((left + x, top + y), (*color, 255))
        if not overlay.getchannel("A").getbbox():
            raise ValueError("Native capsule footer text recovery was empty")
        return overlay

    @functools.lru_cache(maxsize=3)
    def _native_legal_overlay(self, base_grade):
        return self._legal_overlay_from_samples(
            self._native_rare_samples(base_grade))

    @functools.lru_cache(maxsize=3)
    def _native_frame(self, base_grade):
        """Recover stock chrome/background without retaining a donor robot."""
        from PIL import Image, ImageDraw
        samples = self._native_card_samples(base_grade)
        donor = samples[0].copy()
        left, top, right, bottom = self.CARD_PICTURE_BOX
        width, height = right - left, bottom - top

        # Begin with a deterministic blue field. Native consensus below
        # replaces nearly all of it; this only fills pixels hidden by a robot
        # in every sampled stock card.
        picture = Image.new("RGBA", (width, height))
        draw = ImageDraw.Draw(picture)
        for y in range(height):
            for x in range(width):
                noise = ((x * 37 + y * 61 + x * y * 3) ^ (x << 3)) & 15
                red = 2 + noise // 5
                green = 38 + (height - y) * 22 // height + noise
                blue = 91 + (height - y) * 55 // height + noise * 2
                if ((x * 131 + y * 197 + x * y * 17) % 211) == 0:
                    red, green, blue = 205, 226, 255
                draw.point((x, y), fill=(red, green, blue, 255))

        threshold = max(3, len(samples) // 5)
        for y in range(top, bottom):
            for x in range(left, right):
                counts = Counter(image.getpixel((x, y)) for image in samples)
                pixel, support = counts.most_common(1)[0]
                # Exact high-support pixels recover the fixed star map and the
                # two tiny stock footer rows. A lower-support blue mode is also
                # safe background; robot colors vary between source cards.
                if support >= threshold or (
                        support >= 3 and self._is_starfield(pixel)):
                    picture.putpixel((x - left, y - top), pixel)
        donor.paste(picture, (left, top))

        donor.alpha_composite(self._native_header_overlay(base_grade))
        return donor

    def _head_crop(self, uid, variant):
        """Create a head/upper-chest close-up from the same unit and pose."""
        from PIL import Image
        source_variant = self.HEAD_SOURCE_VARIANTS.get(
            (uid, min(variant, 1)), variant)
        source = self._source_art(uid, source_variant)
        bounds = source.getchannel("A").getbbox()
        if not bounds:
            raise ValueError(f"Source unit art is empty for capsule card {uid}")
        left, top, right, bottom = bounds
        body_width = right - left
        body_height = bottom - top

        # Unit textures are full-body presentation renders. Crop the upper
        # portion first, then scale that crop to the card window. Scaling the
        # whole body would recreate the small model shown in the bad UI.
        presentation_width = min(
            source.width,
            body_width + max(8, body_width // 12),
            max(96, round(body_height * 1.35)))
        # Stock capsule cards are portraits, not the unit-select full-body
        # thumbnail. Zoom roughly 1.45x after selecting the upper body.
        crop_width = min(
            body_width + max(6, body_width // 16),
            max(96, round(presentation_width / 1.45)))
        anchor = self.HEAD_ANCHORS.get((uid, min(variant, 1)))
        if anchor:
            crop_width = min(
                source.width, max(1, round(anchor[2] * source.width)))
        crop_height = min(body_height, max(36, round(crop_width / 2.15)))

        # Wings, guns and antennae can begin well above the face. Find the
        # first sustained opaque run through the central body band instead of
        # blindly treating the alpha bounding-box top as the head.
        alpha = source.getchannel("A")
        band_left = left + body_width // 4
        band_right = right - body_width // 4
        row_threshold = max(6, (band_right - band_left) // 8)
        focus_top = top
        for candidate in range(top, min(bottom - 3, top + body_height * 2 // 3)):
            if all(sum(
                    1 for value in alpha.crop(
                        (band_left, row, band_right, row + 1)).getdata()
                    if value > 32) >= row_threshold
                   for row in range(candidate, candidate + 3)):
                focus_top = candidate
                break
        center_x = (
            round(anchor[0] * source.width)
            if anchor else (left + right) // 2)
        crop_left = max(0, min(source.width - crop_width,
                               center_x - crop_width // 2))
        if anchor:
            face_y = round(anchor[1] * source.height)
            crop_top = face_y - crop_height * 2 // 5
        else:
            crop_top = focus_top - max(2, body_height // 40)
        crop_top = max(0, crop_top)
        crop_top = min(crop_top, source.height - crop_height)
        crop = source.crop(
            (crop_left, crop_top, crop_left + crop_width,
             crop_top + crop_height))

        target_width = self.CARD_PICTURE_BOX[2] - self.CARD_PICTURE_BOX[0]
        target_height = self.CARD_PICTURE_BOX[3] - self.CARD_PICTURE_BOX[1]
        scale = max(target_width / crop.width, target_height / crop.height)
        resized = crop.resize(
            (max(1, round(crop.width * scale)),
             max(1, round(crop.height * scale))),
            Image.Resampling.LANCZOS)
        x = max(0, (resized.width - target_width) // 2)
        # Keep antennae and the face near the top while allowing some chest.
        y = max(0, min(resized.height - target_height,
                       (resized.height - target_height) // 3))
        return resized.crop((x, y, x + target_width, y + target_height))

    def _reconstructed_card(self, uid, variant):
        from PIL import ImageEnhance
        unit = self.units[uid]
        base_grade = unit["grade"][:1]
        if base_grade not in ("A", "B", "C"):
            raise ValueError(
                f"No native capsule frame policy for grade {unit['grade']} "
                f"(unit {uid})")
        card = self._native_frame(base_grade).copy()
        head = self._head_crop(uid, variant)
        if unit.get("subgrade") == 4:
            # Stock R cards keep the starfield and chrome bright; only the
            # robot is concealed. Preserve alpha while reducing RGB.
            alpha = head.getchannel("A")
            head = ImageEnhance.Brightness(head).enhance(0.15)
            head.putalpha(alpha)
        card.alpha_composite(
            head, (self.CARD_PICTURE_BOX[0], self.CARD_PICTURE_BOX[1]))

        # Reapply native foreground pixels after the robot. The rank banner is
        # opaque, while the footer must remain a sparse text-only overlay so
        # the robot continues beneath every gap between its tiny glyphs.
        card.alpha_composite(self._native_header_overlay(base_grade))
        card.alpha_composite(self._native_legal_overlay(base_grade))
        return card

    def card(self, uid, variant=0):
        unit = self.units[uid]
        native = self._sprite_pair(
            {str(uid): unit.get("capsule_sprites")}, uid, "capsule_sprites")
        if native:
            return self.sprite(native[min(variant, 1)])
        return self._reconstructed_card(uid, min(variant, 1))


def build_previews(client, lookup, catalog_path, output):
    catalog = read_json(catalog_path)
    assets = Assets(client, lookup, catalog)
    out = Path(output)
    (out / "units").mkdir(parents=True, exist_ok=True)
    used = {s["unit_id"] for kind in ("fixed_machines", "custom_machines")
            for m in catalog[kind] for s in m.get("slots", m.get("candidates", []))}
    used.update(u["id"] for u in catalog["units"] if u["eligible"])
    results, failures = [], []
    for uid in sorted(used):
        try:
            image = assets.portrait(uid)
            image.save(out / "units" / f"{uid}.png")
            results.append(uid)
        except (ValueError, OSError, KeyError, struct.error) as error:
            failures.append({"unit_id": uid, "reason": str(error)})
    report = {"units": results, "failures": failures, "zdx_sha256": assets.reader.index_hash}
    write_json(out / "asset-report.json", report)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--client", type=Path, required=True)
    p.add_argument("--lookup", type=Path, required=True)
    p.add_argument("--catalog", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    report = build_previews(args.client, args.lookup, args.catalog, args.output)
    print(json.dumps({"previews": len(report["units"]), "failures": len(report["failures"])}))


