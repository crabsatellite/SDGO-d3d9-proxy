import base64
import copy
import hashlib
import http.client
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

from PIL import Image
from PIL.PngImagePlugin import PngInfo


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import capsule_catalog
import capsule_portraits
import capsule_ui
from tests.test_capsule_ui import fixture_catalog


CROP = [0, 0, 326, 128]


def png_bytes(*, width=326, height=128, metadata=True, compress_level=6):
    image = Image.new("RGB", (width, height), (30, 80, 140))
    stream = io.BytesIO()
    info = None
    if metadata:
        info = PngInfo()
        info.add_text("private", "must be removed")
    image.save(stream, "PNG", pnginfo=info, compress_level=compress_level)
    return stream.getvalue()


def png_base64(**kwargs):
    return base64.b64encode(png_bytes(**kwargs)).decode("ascii")


class PortraitValidationTests(unittest.TestCase):
    def test_make_override_reencodes_rgba_strips_metadata_and_hashes(self):
        source = png_base64()
        entry = capsule_portraits.make_override(source, CROP)
        raw = base64.b64decode(entry["png_base64"])
        self.assertEqual(entry["sha256"], hashlib.sha256(raw).hexdigest())
        self.assertNotEqual(entry["png_base64"], source)
        image, crop = capsule_portraits.decode_override(entry)
        self.assertEqual(image.mode, "RGBA")
        self.assertEqual(image.size, (326, 128))
        self.assertEqual(crop, [0.0, 0.0, 326.0, 128.0])
        with Image.open(io.BytesIO(raw)) as decoded:
            self.assertNotIn("private", decoded.info)

    def test_decode_accepts_portable_png_bytes_and_rejects_hash_tamper(self):
        raw = png_bytes(metadata=False, compress_level=0)
        entry = {
            "png_base64": base64.b64encode(raw).decode("ascii"),
            "sha256": hashlib.sha256(raw).hexdigest(),
            "crop": CROP,
        }
        image, crop = capsule_portraits.decode_override(entry)
        self.assertEqual(image.mode, "RGBA")
        self.assertEqual(crop, [0.0, 0.0, 326.0, 128.0])

        tampered = copy.deepcopy(entry)
        tampered["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "hash"):
            capsule_portraits.decode_override(tampered)

    def test_rejects_invalid_base64_corrupt_non_png_and_oversized_data(self):
        invalid = (
            "%%%",
            base64.b64encode(b"not png").decode("ascii"),
            base64.b64encode(b"\x89PNG\r\n\x1a\ncorrupt").decode("ascii"),
        )
        for value in invalid:
            with self.subTest(value=value[:8]):
                with self.assertRaises(ValueError):
                    capsule_portraits.make_override(value, CROP)
        oversized = base64.b64encode(
            b"\x89PNG\r\n\x1a\n" + b"x" * capsule_portraits.MAX_IMAGE_BYTES
        ).decode("ascii")
        with self.assertRaisesRegex(ValueError, "4 MiB"):
            capsule_portraits.make_override(oversized, CROP)
        with self.assertRaisesRegex(ValueError, "1..2048"):
            capsule_portraits.make_override(
                png_base64(width=2049, height=1, metadata=False),
                [0, 0, 2.546875, 1],
            )

    def test_rejects_bad_crop_values_bounds_and_aspect(self):
        source = png_base64(metadata=False)
        invalid = [
            [0, 0, 10],
            [0, 0, float("nan"), 10],
            [-1, 0, 326, 128],
            [0, 0, 327, 128],
            [0, 0, 300, 128],
        ]
        for crop in invalid:
            with self.subTest(crop=crop):
                with self.assertRaises(ValueError):
                    capsule_portraits.make_override(source, crop)
        # One pixel of width rounding is accepted.
        entry = capsule_portraits.make_override(
            source, [0, 0, 325, 128])
        self.assertEqual(entry["crop"][2], 325.0)
        epsilon_entry = capsule_portraits.make_override(
            source, [0, 0, 326.0000000000001, 128])
        self.assertEqual(epsilon_entry["crop"][2], 326.0000000000001)

    def test_catalog_hook_rejects_unknown_s_grade_external_fields_and_total(self):
        catalog = fixture_catalog()
        entry = capsule_portraits.make_override(
            png_base64(metadata=False), CROP)
        catalog["portrait_overrides"] = {"1": {"0": entry}}
        capsule_catalog.validate(catalog)

        unknown = copy.deepcopy(catalog)
        unknown["portrait_overrides"] = {"999": {"0": entry}}
        with self.assertRaisesRegex(ValueError, "unknown unit"):
            capsule_catalog.validate(unknown)

        high_rank = copy.deepcopy(catalog)
        high_rank["units"][0].update(rank=4, grade="S")
        with self.assertRaisesRegex(ValueError, "A/B/C"):
            capsule_catalog.validate(high_rank)

        external = copy.deepcopy(catalog)
        external["portrait_overrides"]["1"]["0"] = {
            **entry, "path": r"C:\private\portrait.png"}
        with self.assertRaisesRegex(ValueError, "requires only"):
            capsule_catalog.validate(external)

        both_variants = copy.deepcopy(catalog)
        both_variants["portrait_overrides"]["1"]["1"] = copy.deepcopy(entry)
        raw_size = len(base64.b64decode(entry["png_base64"]))
        with patch.object(
                capsule_portraits, "MAX_TOTAL_BYTES", raw_size * 2 - 1):
            with self.assertRaisesRegex(ValueError, "32 MiB"):
                capsule_catalog.validate(both_variants)

    def test_bundle_export_preserves_portable_overrides_for_both_roles(self):
        catalog = fixture_catalog()
        entry = {
            "png_base64": base64.b64encode(
                png_bytes(metadata=False, compress_level=0)).decode("ascii"),
            "sha256": "",
            "crop": CROP,
        }
        entry["sha256"] = hashlib.sha256(
            base64.b64decode(entry["png_base64"])).hexdigest()
        catalog["portrait_overrides"] = {"1": {"0": entry}}
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "bundle"
            capsule_catalog.export_bundle(catalog, output)
            for role in ("client", "server"):
                payload = capsule_catalog.read_json(output / f"{role}.json")
                self.assertEqual(
                    payload["portrait_overrides"],
                    catalog["portrait_overrides"],
                )
                self.assertEqual(
                    payload["catalog_revision"],
                    capsule_catalog.digest(catalog),
                )


class PortraitBackendTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.catalog_path = self.root / "catalog.json"
        capsule_catalog.write_json(self.catalog_path, fixture_catalog())
        self.store = capsule_ui.CatalogStore(self.catalog_path)

    def tearDown(self):
        self.temporary.cleanup()

    def test_save_apply_both_stale_reset_and_no_client_get(self):
        catalog = capsule_catalog.read_json(self.catalog_path)
        revision = capsule_catalog.digest(catalog)
        saved = self.store.portrait_save(
            1, 0, png_base64(), CROP, revision, apply_to_both=True)
        self.assertEqual(saved["saved_variants"], [0, 1])
        persisted = capsule_catalog.read_json(self.catalog_path)
        self.assertEqual(
            set(persisted["portrait_overrides"]["1"]), {"0", "1"})
        self.assertTrue(Path(saved["backup_path"]).is_file())
        self.assertIsNone(self.store.portrait_get(1, 0)["card_data_url"])

        with self.assertRaises(capsule_ui.ConflictError):
            self.store.portrait_reset(1, 0, revision)
        reset = self.store.portrait_reset(
            1, 0, saved["catalog_revision"])
        self.assertFalse(reset["overridden"])
        persisted = capsule_catalog.read_json(self.catalog_path)
        self.assertEqual(set(persisted["portrait_overrides"]["1"]), {"1"})

        with self.assertRaises(capsule_ui.MissingError):
            self.store.portrait_get(999, 0)
        with self.assertRaisesRegex(
                capsule_ui.RequestError, "requires --client"):
            self.store.portrait_preview(
                1, 0, png_base64(), CROP, reset["catalog_revision"])

    def test_preview_renders_temporary_copy_without_saving(self):
        client = self.root / "client"
        client.mkdir()
        lookup = self.root / "lookup.json"
        lookup.write_text("{}", encoding="utf-8")
        store = capsule_ui.CatalogStore(
            self.catalog_path, client_path=client, lookup_path=lookup)
        before = self.catalog_path.read_bytes()
        revision = capsule_catalog.digest(
            capsule_catalog.read_json(self.catalog_path))
        with patch.object(
                capsule_ui, "_render_card",
                return_value=Image.new("RGBA", (326, 128), "red")):
            preview = store.portrait_preview(
                1, 0, png_base64(), CROP, revision)
        self.assertTrue(preview["card_data_url"].startswith("data:image/png;base64,"))
        self.assertEqual(self.catalog_path.read_bytes(), before)

    def test_http_get_save_reset_and_preview_error(self):
        server = capsule_ui.create_server(self.store, port=0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        host = f"127.0.0.1:{server.server_address[1]}"

        def request(method, path, payload=None):
            connection = http.client.HTTPConnection(
                "127.0.0.1", server.server_address[1], timeout=5)
            headers = {"Host": host, "Accept": "application/json"}
            body = None
            if payload is not None:
                body = json.dumps(payload).encode("utf-8")
                headers.update({
                    "Content-Type": "application/json",
                    "Origin": f"http://{host}",
                    "X-Capsule-Token": server.app.token,
                })
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            data = json.loads(response.read() or b"{}")
            connection.close()
            return response.status, data

        try:
            status, current = request(
                "GET", "/api/portraits?unit_id=1&variant=0")
            self.assertEqual(status, 200)
            self.assertFalse(current["overridden"])
            self.assertIsNone(current["image_data_url"])
            self.assertIsNone(current["card_data_url"])

            status, saved = request("POST", "/api/portraits/save", {
                "unit_id": 1,
                "variant": 0,
                "png_base64": png_base64(),
                "crop": CROP,
                "expected_revision": current["catalog_revision"],
            })
            self.assertEqual(status, 200, saved)
            status, current = request(
                "GET", "/api/portraits?unit_id=1&variant=0")
            self.assertTrue(current["overridden"])
            self.assertTrue(current["image_data_url"].startswith(
                "data:image/png;base64,"))

            status, error = request("POST", "/api/portraits/preview", {
                "unit_id": 1,
                "variant": 0,
                "png_base64": png_base64(),
                "crop": CROP,
                "expected_revision": current["catalog_revision"],
            })
            self.assertEqual(status, 400)
            self.assertIn("requires --client", error["error"])

            status, reset = request("POST", "/api/portraits/reset", {
                "unit_id": 1,
                "variant": 0,
                "expected_revision": current["catalog_revision"],
            })
            self.assertEqual(status, 200, reset)
            self.assertNotIn(
                "portrait_overrides",
                capsule_catalog.read_json(self.catalog_path),
            )
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_http_rejects_invalid_json_utf8_and_variant(self):
        server = capsule_ui.create_server(self.store, port=0)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        host = f"127.0.0.1:{server.server_address[1]}"

        def raw_post(body):
            connection = http.client.HTTPConnection(
                "127.0.0.1", server.server_address[1], timeout=5)
            connection.request(
                "POST",
                "/api/portraits/save",
                body=body,
                headers={
                    "Host": host,
                    "Content-Type": "application/json",
                    "Origin": f"http://{host}",
                    "X-Capsule-Token": server.app.token,
                },
            )
            response = connection.getresponse()
            data = json.loads(response.read())
            connection.close()
            return response.status, data

        try:
            for body in (b"{", b"\xff"):
                with self.subTest(body=body):
                    status, error = raw_post(body)
                    self.assertEqual(status, 400)
                    self.assertEqual(error["status"], 400)

            connection = http.client.HTTPConnection(
                "127.0.0.1", server.server_address[1], timeout=5)
            connection.request(
                "GET",
                "/api/portraits?unit_id=1&variant=2",
                headers={"Host": host, "Accept": "application/json"},
            )
            response = connection.getresponse()
            error = json.loads(response.read())
            connection.close()
            self.assertEqual(response.status, 400)
            self.assertIn("variant must be 0 or 1", error["error"])
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
