"""Deterministic placeholder art; replaceable without touching draw policy."""
from pathlib import Path


def placeholder_png(path: Path):
    from PIL import Image, ImageDraw, ImageFont
    scale = 4
    image = Image.new("RGB", (182 * scale, 108 * scale), "#101b2b")
    draw = ImageDraw.Draw(image)

    def box(rect):
        return tuple(round(value * scale) for value in rect)

    def font(size):
        for name in ("arial.ttf", "DejaVuSans.ttf"):
            try:
                return ImageFont.truetype(name, size * scale)
            except OSError:
                pass
        return ImageFont.load_default(size=size * scale)

    def label(text, y, size, color):
        draw.text((91 * scale, y * scale), text, font=font(size), anchor="mt", fill=color)

    draw.rounded_rectangle(box((3, 3, 178, 104)), radius=5 * scale, outline="#50d8d0", width=2 * scale)
    for y in (28, 82):
        draw.line(box((8, y, 174, y)), fill="#28495d", width=scale)
    label("COLLECTION", 8, 13, "#d6f7f3")
    draw.rounded_rectangle(box((72, 34, 110, 64)), radius=14 * scale,
                           fill="#193c50", outline="#50d8d0", width=2 * scale)
    draw.line(box((73, 49, 109, 49)), fill="#50d8d0", width=scale)
    draw.ellipse(box((81, 39, 101, 59)), fill="#101b2b")
    label("?", 38, 18, "#ffffff")
    label("NEW UNIT / BASE FORM", 68, 9, "#d6f7f3")
    label("100,000 GP", 86, 12, "#f4ce72")
    draw.text((172 * scale, 78 * scale), "PLACEHOLDER", font=font(5), anchor="rs", fill="#73949e")
    path.parent.mkdir(parents=True, exist_ok=True)
    image.resize((182, 108), Image.Resampling.LANCZOS).save(path)
