import hashlib
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import capsule_ui_layout


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def source_fixture():
    data = bytearray(capsule_ui_layout.SUPPORTED_DECODED_LENGTH)
    for _, name, name_offset, text_offset, default in (
        capsule_ui_layout.POINT_STATIC_PRICE_FIELDS
    ):
        encoded_name = (name + "\0").encode("utf-16-le")
        data[name_offset:name_offset + len(encoded_name)] = encoded_name
        encoded_default = (default + "\0").encode("utf-16-le")
        data[text_offset:text_offset + len(encoded_default)] = encoded_default
    return bytes(data)


def expected_patch(data):
    result = bytearray(data)
    for _, _, _, text_offset, _ in capsule_ui_layout.POINT_STATIC_PRICE_FIELDS:
        result[text_offset:text_offset + 2] = b"\0\0"
    return bytes(result)


class CapsuleUILayoutTests(unittest.TestCase):
    def supported_hashes(self, source):
        patched = expected_patch(source)
        return patch.multiple(
            capsule_ui_layout,
            SUPPORTED_SOURCE_SHA256=sha256(source),
            SUPPORTED_PATCHED_SHA256=sha256(patched),
        )

    def test_evidence_locks_installed_decoded_layout(self):
        self.assertEqual(
            capsule_ui_layout.SUPPORTED_SOURCE_SHA256,
            "01671cdc6dab9789935c47d3a23486117e302f68a6824d43eac769e1f53f4cd6",
        )
        self.assertEqual(
            capsule_ui_layout.SUPPORTED_PATCHED_SHA256,
            "f3852c7bcc179c54ef0af8af7caea290fd388c9a0474c8ebcfc271f9c2ed489e",
        )
        self.assertEqual(capsule_ui_layout.SUPPORTED_DECODED_LENGTH, 36183)
        self.assertEqual(
            [row[4] for row in capsule_ui_layout.POINT_STATIC_PRICE_FIELDS],
            ["10000", "1200", "999", "999", "999", "999", "999", "999"],
        )

    def test_patch_changes_only_eight_first_utf16_code_units_and_is_idempotent(self):
        source = source_fixture()
        expected = expected_patch(source)
        with self.supported_hashes(source):
            output, receipt = capsule_ui_layout.patch_capsule_price_defaults(source)
            again, second_receipt = capsule_ui_layout.patch_capsule_price_defaults(output)

        self.assertEqual(output, expected)
        changed = [
            index
            for index, pair in enumerate(zip(source, output))
            if pair[0] != pair[1]
        ]
        expected_changed = sorted(
            row[3] for row in capsule_ui_layout.POINT_STATIC_PRICE_FIELDS
        )
        self.assertEqual(changed, expected_changed)
        self.assertTrue(receipt["changed"])
        self.assertEqual(receipt["output_sha256"], sha256(expected))
        self.assertEqual(len(receipt["fields"]), 8)
        self.assertIs(again, output)
        self.assertFalse(second_receipt["changed"])
        self.assertEqual(second_receipt["input_sha256"], sha256(expected))
        self.assertEqual(second_receipt["output_sha256"], sha256(expected))

    def test_missing_name_is_rejected(self):
        source = bytearray(source_fixture())
        source[0x7EC0] = ord("Q")
        with self.assertRaises(capsule_ui_layout.CapsuleUILayoutError):
            capsule_ui_layout.patch_capsule_price_defaults(bytes(source))

    def test_duplicate_name_is_rejected(self):
        source = bytearray(source_fixture())
        duplicate = "PointStatic0".encode("utf-16-le")
        source[0x100:0x100 + len(duplicate)] = duplicate
        with self.assertRaises(capsule_ui_layout.CapsuleUILayoutError):
            capsule_ui_layout.patch_capsule_price_defaults(bytes(source))

    def test_nonnumeric_default_is_rejected(self):
        source = bytearray(source_fixture())
        source[0x801D:0x801F] = "A".encode("utf-16-le")
        with self.assertRaises(capsule_ui_layout.CapsuleUILayoutError):
            capsule_ui_layout.patch_capsule_price_defaults(bytes(source))

    def test_truncated_layout_is_rejected(self):
        with self.assertRaises(capsule_ui_layout.CapsuleUILayoutError):
            capsule_ui_layout.patch_capsule_price_defaults(source_fixture()[:-1])

    def test_partial_patch_is_rejected(self):
        source = bytearray(source_fixture())
        source[0x7F3B:0x7F3D] = b"\0\0"
        with self.assertRaises(capsule_ui_layout.CapsuleUILayoutError):
            capsule_ui_layout.patch_capsule_price_defaults(bytes(source))

    def test_unknown_hash_is_rejected_after_layout_validation(self):
        source = bytearray(source_fixture())
        source[0x20] = 1
        with self.assertRaisesRegex(
            capsule_ui_layout.CapsuleUILayoutError, "Unsupported source"
        ):
            capsule_ui_layout.patch_capsule_price_defaults(bytes(source))

    def test_nonbytes_input_is_rejected(self):
        with self.assertRaises(TypeError):
            capsule_ui_layout.patch_capsule_price_defaults(bytearray(source_fixture()))


if __name__ == "__main__":
    unittest.main()
