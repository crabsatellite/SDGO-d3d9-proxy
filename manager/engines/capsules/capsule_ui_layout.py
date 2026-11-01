"""Fail-closed patches for the fixed capsule machine UI layout.

The shipped 1161.uir price controls contain numeric design-time defaults.
When a control's first runtime price equals its default, the native text cache
can skip its first glyph build. This module clears only those defaults. It
does not edit control geometry, colors, visibility, or any other UI resource.
"""
from __future__ import annotations

import hashlib


CAPSULE_UI_RESOURCE = "uirs/1161.uir"
SUPPORTED_DECODED_LENGTH = 36183
SUPPORTED_SOURCE_SHA256 = (
    "01671cdc6dab9789935c47d3a23486117e302f68a6824d43eac769e1f53f4cd6"
)
SUPPORTED_PATCHED_SHA256 = (
    "f3852c7bcc179c54ef0af8af7caea290fd388c9a0474c8ebcfc271f9c2ed489e"
)
PRICE_TEXT_RELATIVE_OFFSET = 0x7B
PRICE_TEXT_FIELD_BYTES = 64

# Slot order is the native UI array order. Records 5-7 are stored out of
# numerical order in the resource, so every absolute offset remains explicit.
POINT_STATIC_PRICE_FIELDS = (
    (0, "PointStatic0", 0x7EC0, 0x7F3B, "10000"),
    (1, "PointStatic1", 0x7FA2, 0x801D, "1200"),
    (2, "PointStatic2", 0x8084, 0x80FF, "999"),
    (3, "PointStatic3", 0x8166, 0x81E1, "999"),
    (4, "PointStatic4", 0x8248, 0x82C3, "999"),
    (5, "PointStatic5", 0x84EE, 0x8569, "999"),
    (6, "PointStatic6", 0x832A, 0x83A5, "999"),
    (7, "PointStatic7", 0x840C, 0x8487, "999"),
)

CAPSULE_PRICE_LAYOUT_EVIDENCE = {
    "resource": CAPSULE_UI_RESOURCE,
    "decoded_length": SUPPORTED_DECODED_LENGTH,
    "source_sha256": SUPPORTED_SOURCE_SHA256,
    "patched_sha256": SUPPORTED_PATCHED_SHA256,
    "host": {
        "image": "GOnline.exe",
        "pe_timestamp": "0x54BE1568",
        "size_of_image": "0x00C97000",
    },
    "native_functions": {
        "static_text_set_text_va": "0x00C72100",
        "static_text_rebuild_va": "0x00C74B00",
        "wide_string_compare_va": "0x0048BED0",
    },
    "finding": (
        "SetText copies current text to backup before replacing current text. "
        "For ordinary static text it does not force the dirty byte; the rebuild "
        "path skips glyph creation when backup and current text compare equal."
    ),
    "patch": (
        "Clear the first UTF-16LE code unit in each fixed 64-byte price field; "
        "all offsets, field sizes, and remaining bytes stay unchanged."
    ),
}


class CapsuleUILayoutError(ValueError):
    """The supplied UI resource is not the one this patch supports."""


def _sha256(data):
    return hashlib.sha256(data).hexdigest()


def _price_field(data, offset):
    end = offset + PRICE_TEXT_FIELD_BYTES
    if end > len(data):
        raise CapsuleUILayoutError("Truncated PointStatic price field")
    raw = data[offset:end]
    terminator = None
    for index in range(0, len(raw), 2):
        if raw[index:index + 2] == b"\0\0":
            terminator = index
            break
    if terminator is None:
        raise CapsuleUILayoutError("Unterminated PointStatic price field")
    try:
        text = raw[:terminator].decode("utf-16-le")
    except UnicodeDecodeError as error:
        raise CapsuleUILayoutError("Invalid PointStatic UTF-16LE price field") from error
    return raw, text


def _layout_state(data):
    states = []
    records = []
    for slot, name, expected_name_offset, expected_text_offset, default in (
        POINT_STATIC_PRICE_FIELDS
    ):
        encoded_name = name.encode("utf-16-le")
        occurrences = []
        cursor = 0
        while True:
            found = data.find(encoded_name, cursor)
            if found < 0:
                break
            occurrences.append(found)
            cursor = found + 2
        if len(occurrences) != 1:
            raise CapsuleUILayoutError(
                f"{name} must occur exactly once; found {len(occurrences)}"
            )
        name_offset = occurrences[0]
        if name_offset != expected_name_offset:
            raise CapsuleUILayoutError(f"Unexpected {name} record offset")
        if data[
            name_offset + len(encoded_name):name_offset + len(encoded_name) + 2
        ] != b"\0\0":
            raise CapsuleUILayoutError(f"Unterminated {name} control name")
        text_offset = name_offset + PRICE_TEXT_RELATIVE_OFFSET
        if text_offset != expected_text_offset:
            raise CapsuleUILayoutError(f"Unexpected {name} price field offset")
        raw, text = _price_field(data, text_offset)
        expected = (default + "\0").encode("utf-16-le")
        if text == default and text.isascii() and text.isdecimal():
            state = "source"
        elif (
            text == ""
            and raw[:2] == b"\0\0"
            and raw[2:len(expected)] == expected[2:]
        ):
            state = "patched"
        else:
            raise CapsuleUILayoutError(
                f"{name} price default is neither the supported numeric value "
                "nor its empty patched form"
            )
        states.append(state)
        records.append(
            {
                "slot": slot,
                "name": name,
                "name_offset": name_offset,
                "text_offset": text_offset,
                "field_bytes": PRICE_TEXT_FIELD_BYTES,
                "original_default": default,
            }
        )
    if len(set(states)) != 1:
        raise CapsuleUILayoutError("Partially patched PointStatic defaults are unsupported")
    return states[0], records


def patch_capsule_price_defaults(data):
    """Return ``(patched_bytes, receipt)`` for the supported decoded 1161.uir.

    The operation is idempotent for the exact supported patched bytes. Every
    unknown length, layout, default, partial patch, or whole-resource hash is
    rejected before any result is returned.
    """
    if not isinstance(data, bytes):
        raise TypeError("Decoded UI resource must be bytes")
    if len(data) != SUPPORTED_DECODED_LENGTH:
        raise CapsuleUILayoutError(
            f"Unsupported decoded 1161.uir length: {len(data)}"
        )

    state, records = _layout_state(data)
    input_sha256 = _sha256(data)
    if state == "source":
        if input_sha256 != SUPPORTED_SOURCE_SHA256:
            raise CapsuleUILayoutError("Unsupported source 1161.uir hash")
        result = bytearray(data)
        for record in records:
            offset = record["text_offset"]
            result[offset:offset + 2] = b"\0\0"
        output = bytes(result)
        if _sha256(output) != SUPPORTED_PATCHED_SHA256:
            raise CapsuleUILayoutError("Patched 1161.uir hash did not match evidence")
        changed = True
    else:
        if input_sha256 != SUPPORTED_PATCHED_SHA256:
            raise CapsuleUILayoutError("Unsupported patched 1161.uir hash")
        output = data
        changed = False

    receipt = {
        "schema_version": 1,
        "resource": CAPSULE_UI_RESOURCE,
        "decoded_length": len(data),
        "input_sha256": input_sha256,
        "output_sha256": _sha256(output),
        "supported_source_sha256": SUPPORTED_SOURCE_SHA256,
        "supported_patched_sha256": SUPPORTED_PATCHED_SHA256,
        "changed": changed,
        "fields": records,
    }
    return output, receipt
