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
    # A few presentation renders put a wing or weapon above the actual face.
    # These anchors are measured in the shipped 256x256 source textures and
    # keep heads/turrets centered when a tall gun, wing or backpack otherwise
    # wins the generic alpha scan. The third value is the source crop width.
    HEAD_ANCHORS = {
        (10208, 0): (128, 128, 150),
        (10208, 1): (118, 128, 150),
        (10219, 0): (126, 105, 112),
        (10219, 1): (132, 104, 112),
        (14016, 0): (128, 108, 112),
        (14016, 1): (129, 108, 118),
        (24713, 0): (128, 82, 108),
        (24713, 1): (130, 83, 108),
        (24716, 0): (130, 103, 100),
        (24716, 1): (158, 108, 100),
        (24720, 0): (126, 88, 105),
        (24720, 1): (126, 91, 110),
    }

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

        # Restore the opaque rank banner from a real card. A polygon avoids
        # copying the donor robot beneath the banner's diagonal transparent
        # edge.
        banner = Image.new("L", self.CARD_SIZE, 0)
        ImageDraw.Draw(banner).polygon(
            [(10, 10), (96, 10), (79, 23), (10, 23)], fill=255)
        donor.paste(samples[0], (0, 0), banner)
        return donor

    def _head_crop(self, uid, variant):
        """Create a head/upper-chest close-up from the same unit and pose."""
        from PIL import Image
        source = self._source_art(uid, variant)
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
            crop_width = min(source.width, anchor[2])
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
        center_x = anchor[0] if anchor else (left + right) // 2
        crop_left = max(0, min(source.width - crop_width,
                               center_x - crop_width // 2))
        if anchor:
            crop_top = anchor[1] - crop_height * 2 // 5
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

        # Reapply the native fixed pixels (rank banner and legal-text rows)
        # after the robot, matching their foreground order in stock cards.
        frame = self._native_frame(base_grade)
        from PIL import Image, ImageDraw
        overlay = Image.new("L", self.CARD_SIZE, 0)
        draw = ImageDraw.Draw(overlay)
        draw.polygon([(10, 10), (96, 10), (79, 23), (10, 23)], fill=255)
        draw.rectangle((10, 67, 172, 73), fill=255)
        card.paste(frame, (0, 0), overlay)
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


