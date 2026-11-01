"""Portable, metadata-free PNG portrait overrides for capsule cards."""
from __future__ import annotations

import base64
import binascii
import hashlib
import io
import math

from PIL import Image, UnidentifiedImageError


MAX_IMAGE_BYTES = 4 * 1024 * 1024
MAX_TOTAL_BYTES = 32 * 1024 * 1024
MAX_IMAGE_DIMENSION = 2048
CARD_ASPECT = 163 / 64
ENTRY_KEYS = {"png_base64", "sha256", "crop"}
BOUNDS_EPSILON = 1e-6


def _decode_base64(value):
    if not isinstance(value, str) or not value:
        raise ValueError("Portrait PNG must be nonempty base64 text")
    if len(value) > 4 * ((MAX_IMAGE_BYTES + 2) // 3):
        raise ValueError("Portrait PNG must be at most 4 MiB")
    try:
        raw = base64.b64decode(value, validate=True)
    except (binascii.Error, ValueError) as error:
        raise ValueError("Portrait PNG has invalid base64") from error
    if not raw or len(raw) > MAX_IMAGE_BYTES:
        raise ValueError("Portrait PNG must be at most 4 MiB")
    return raw


def _load_png(raw):
    if not raw.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("Portrait upload must be a PNG image")
    try:
        with Image.open(io.BytesIO(raw)) as source:
            if source.format != "PNG":
                raise ValueError("Portrait upload must be a PNG image")
            if getattr(source, "n_frames", 1) != 1:
                raise ValueError("Animated PNG portraits are not supported")
            width, height = source.size
            if (not 1 <= width <= MAX_IMAGE_DIMENSION
                    or not 1 <= height <= MAX_IMAGE_DIMENSION):
                raise ValueError("Portrait PNG dimensions must be 1..2048 pixels")
            source.load()
            image = source.convert("RGBA")
    except (UnidentifiedImageError, OSError) as error:
        raise ValueError("Portrait PNG is corrupt or unsupported") from error
    image.info.clear()
    return image


def _canonical_png(raw):
    image = _load_png(raw)
    stream = io.BytesIO()
    image.save(stream, "PNG", compress_level=9)
    canonical = stream.getvalue()
    if len(canonical) > MAX_IMAGE_BYTES:
        raise ValueError("Canonical portrait PNG exceeds 4 MiB")
    return image, canonical


def _validate_crop(crop, width, height):
    if not isinstance(crop, list) or len(crop) != 4:
        raise ValueError("Portrait crop must be [x, y, width, height]")
    normalized = []
    for value in crop:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ValueError("Portrait crop values must be finite numbers")
        value = float(value)
        if not math.isfinite(value):
            raise ValueError("Portrait crop values must be finite numbers")
        normalized.append(value)
    x, y, crop_width, crop_height = normalized
    if x < 0 or y < 0 or crop_width <= 0 or crop_height <= 0:
        raise ValueError("Portrait crop must have positive in-image dimensions")
    if (x + crop_width > width + BOUNDS_EPSILON
            or y + crop_height > height + BOUNDS_EPSILON):
        raise ValueError("Portrait crop is outside the PNG bounds")
    if abs(crop_width - crop_height * CARD_ASPECT) > 1:
        raise ValueError("Portrait crop must use the 163:64 card aspect ratio")
    return normalized


def make_override(png_base64, crop):
    """Normalize an uploaded PNG and return its portable catalog entry."""
    image, canonical = _canonical_png(_decode_base64(png_base64))
    normalized_crop = _validate_crop(crop, image.width, image.height)
    return {
        "png_base64": base64.b64encode(canonical).decode("ascii"),
        "sha256": hashlib.sha256(canonical).hexdigest(),
        "crop": normalized_crop,
    }


def decode_override(entry):
    """Validate a portable entry and return its RGBA image and crop."""
    if not isinstance(entry, dict) or set(entry) != ENTRY_KEYS:
        raise ValueError(
            "Portrait override requires only png_base64, sha256, and crop")
    raw = _decode_base64(entry["png_base64"])
    image = _load_png(raw)
    expected = hashlib.sha256(raw).hexdigest()
    if (not isinstance(entry["sha256"], str)
            or entry["sha256"] != expected):
        raise ValueError("Portrait PNG hash does not match its bytes")
    crop = _validate_crop(entry["crop"], image.width, image.height)
    return image, crop


def get_override(catalog, uid, variant):
    if type(uid) is not int or type(variant) is not int or variant not in (0, 1):
        return None
    variants = catalog.get("portrait_overrides", {}).get(str(uid), {})
    entry = variants.get(str(variant))
    return entry if isinstance(entry, dict) else None


def validate_overrides(catalog):
    overrides = catalog.get("portrait_overrides")
    if overrides is None:
        return
    if not isinstance(overrides, dict):
        raise ValueError("portrait_overrides must be an object")
    units = {
        unit["id"]: unit for unit in catalog.get("units", [])
        if isinstance(unit, dict) and type(unit.get("id")) is int
    }
    total = 0
    for uid_text, variants in overrides.items():
        if (not isinstance(uid_text, str) or not uid_text.isdecimal()
                or str(int(uid_text)) != uid_text
                or int(uid_text) not in units):
            raise ValueError("Portrait override references an unknown unit")
        unit = units[int(uid_text)]
        if (unit.get("rank", 99) > 3
                or not str(unit.get("grade", "")).startswith(("A", "B", "C"))):
            raise ValueError(
                "Portrait overrides require a verified native A/B/C card frame")
        if not isinstance(variants, dict) or not variants:
            raise ValueError("Portrait override variants must be a nonempty object")
        if any(variant not in ("0", "1") for variant in variants):
            raise ValueError("Portrait override variant must be 0 or 1")
        for entry in variants.values():
            image, _ = decode_override(entry)
            del image
            total += len(_decode_base64(entry["png_base64"]))
            if total > MAX_TOTAL_BYTES:
                raise ValueError("Portrait overrides exceed the 32 MiB total limit")
