"""Prepare collection activity-capsule cover art for the native poster slot."""
from pathlib import Path

from .client import COVER_SIZE


def prepare_cover(source: Path, output: Path) -> None:
    """Scale a complete supplied poster to the native 361x322 sprite frame."""
    from PIL import Image

    source = Path(source)
    output = Path(output)
    with Image.open(source) as opened:
        opened.load()
        if opened.width < COVER_SIZE[0] or opened.height < COVER_SIZE[1]:
            raise ValueError(
                f"Cover source must be at least {COVER_SIZE[0]}x{COVER_SIZE[1]}"
            )
        image = opened.convert("RGB").resize(
            COVER_SIZE, Image.Resampling.LANCZOS
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output, "PNG", optimize=False, compress_level=9)
