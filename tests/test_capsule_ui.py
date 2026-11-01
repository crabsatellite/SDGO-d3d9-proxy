import copy
import http.client
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import capsule_catalog as catalog_lib
import capsule_ui
from tests import capsule_http_fixture
from capsule_client_compile import COMPILER_VERSION


def fixture_catalog():
    units = [
        {
            "id": index,
            "name": f"Unit {index}",
            "grade": "C" if index <= 4 else "B",
            "rank": 1 if index <= 4 else 2,
            "subgrade": 2,
            "eligible": True,
            "transform_to": 0,
            "capsule_sprites": [f"sprs\\{index * 2}.spr", f"sprs\\{index * 2 + 1}.spr"],
        }
        for index in range(1, 9)
    ]
    fixed = {
        "id": 101,
        "name": "Native Fixed",
        "enabled": True,
        "owner": "native",
        "currency": "GP",
        "price": 1200,
        "draw_policy": "native_sequence",
        "slots": [
            {
                "unit_id": unit_id,
                "weight": weight,
                "native_draw_group": 1,
                "sprites": units[unit_id - 1]["capsule_sprites"],
            }
            for unit_id, weight in zip(range(1, 7), [20, 80, 250, 250, 250, 150])
        ],
    }
    custom = {
        "id": 1,
        "name": "Native Custom",
        "enabled": True,
        "owner": "native",
        "currency_type": 0,
        "price": 2000,
        "draw_policy": "native_mileage",
        "slot_rules": [
            {
                "slot": index,
                "rank_max": 4,
                "subgrades": [2, 3, 4, 5],
                "extension": index >= 7,
            }
            for index in range(1, 9)
        ],
        "candidates": [
            {
                "unit_id": 7,
                "basic_weight": 100,
                "mileage": 0.5,
                "extra_price": 100,
                "reset_mileage": False,
            }
        ],
    }
    return {
        "schema_version": 1,
        "units": units,
        "fixed_machines": [fixed],
        "custom_machines": [custom],
    }


class CapsuleUiHttpTests(unittest.TestCase):
    def test_picker_exposes_sr_and_no_recipe_ss_policy_with_native_restore(self):
        catalog = fixture_catalog()
        catalog["custom_candidate_policy"] = {"version": 1, "native_candidate_unit_ids": [7]}
        for unit, subgrade, has_recipe in zip(catalog["units"][:4], [2, 4, 3, 3], [False, True, False, True]):
            unit.update(rank=4, subgrade=subgrade, has_blueprint_recipe=has_recipe)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "catalog.json"
            catalog_lib.write_json(path, catalog)
            store = capsule_ui.CatalogStore(path)
            app = capsule_ui.CapsuleApplication(store, Path(__file__).resolve().parents[1] / "ui/capsules")
            for output in (app.bootstrap(), app.units("", 100)):
                units = {unit["id"]: unit for unit in output["units"]}
                self.assertFalse(units[1]["can_add_custom"])
                self.assertTrue(units[2]["can_add_custom"])
                self.assertTrue(units[3]["can_add_custom"])
                self.assertFalse(units[4]["can_add_custom"])
                self.assertTrue(units[7]["can_add_custom"])

    def test_import_of_old_json_cannot_restore_forbidden_s_candidates(self):
        catalog = fixture_catalog()
        catalog["custom_candidate_policy"] = {"version": 1, "native_candidate_unit_ids": [7]}
        catalog["units"][0].update(rank=4, subgrade=2, grade="S", has_blueprint_recipe=False)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "catalog.json"
            catalog_lib.write_json(path, catalog)
            before = path.read_bytes()
            store = capsule_ui.CatalogStore(path)
            incoming = copy.deepcopy(catalog)
            incoming.pop("custom_candidate_policy")
            incoming["custom_machines"][0]["candidates"].append({
                "unit_id": 1, "basic_weight": 1, "mileage": 0,
                "extra_price": 0, "reset_mileage": False,
            })
            with self.assertRaises(ValueError):
                store.replace(incoming, catalog_lib.digest(catalog))
            self.assertEqual(path.read_bytes(), before)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.catalog_path = self.root / "catalog.json"
        catalog_lib.write_json(self.catalog_path, fixture_catalog())
        self.assets_path = self.root / "assets"
        (self.assets_path / "units").mkdir(parents=True)
        (self.assets_path / "units" / "1.png").write_bytes(
            b"\x89PNG\r\n\x1a\nfixture"
        )
        self.store = capsule_ui.CatalogStore(
            self.catalog_path,
            assets_path=self.assets_path,
            bundle_path=self.root / "bundle",
        )
        self.server = capsule_ui.create_server(self.store, port=0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.host = f"127.0.0.1:{self.server.server_address[1]}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        self.temp.cleanup()

    def request(
        self,
        method,
        path,
        payload=None,
        *,
        host=None,
        origin=True,
        token=True,
        content_type="application/json",
    ):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_address[1], timeout=5
        )
        headers = {"Host": host or self.host, "Accept": "application/json"}
        body = None
        if payload is not None:
            body = json.dumps(payload).encode("utf-8")
            headers["Content-Type"] = content_type
            if origin:
                headers["Origin"] = f"http://{host or self.host}"
            if token:
                headers["X-Capsule-Token"] = self.server.app.token
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        data = response.read()
        result = {
            "status": response.status,
            "headers": dict(response.getheaders()),
            "body": data,
        }
        if response.headers.get_content_type() == "application/json" and data:
            result["json"] = json.loads(data)
        connection.close()
        return result

    def bootstrap(self):
        response = self.request("GET", "/api/bootstrap")
        self.assertEqual(response["status"], 200)
        return response["json"]

    def post(self, path, payload, **kwargs):
        return self.request("POST", path, payload, **kwargs)

    def test_bootstrap_html_and_bounded_asset_mapping(self):
        bootstrap = self.bootstrap()
        self.assertEqual(bootstrap["counts"]["fixed_machines"], 1)
        self.assertFalse(bootstrap["capabilities"]["apply"])
        self.assertEqual(
            bootstrap["machines"]["fixed_machines"][0]["location"],
            {"position": 1, "page": 1, "slot": 1, "slots_per_page": 8},
        )
        self.assertTrue(bootstrap["units"][0]["has_asset"])
        self.assertEqual(bootstrap["units"][0]["capsule_sprites"][0], "sprs\\2.spr")

        page = self.request("GET", "/")
        self.assertEqual(page["status"], 200)
        self.assertNotIn(b"{{CSRF_TOKEN}}", page["body"])
        self.assertIn(self.server.app.token.encode("ascii"), page["body"])
        self.assertIn("frame-ancestors 'none'", page["headers"]["Content-Security-Policy"])
        self.assertIn(b'id="apply-button"', page["body"])
        self.assertIn(b'id="field-owner-label"', page["body"])
        self.assertIn(b'id="field-policy-label"', page["body"])
        self.assertNotIn(b'id="field-owner"', page["body"])
        self.assertNotIn(b'id="field-policy"', page["body"])

        disabled_apply = self.post(
            "/api/apply",
            {"expected_revision": bootstrap["catalog_revision"]},
        )
        self.assertEqual(disabled_apply["status"], 400)
        self.assertIn(
            "configured at server startup", disabled_apply["json"]["error"]
        )

        asset = self.request("GET", "/assets/unit/1.png")
        self.assertEqual(asset["status"], 200)
        self.assertTrue(asset["body"].startswith(b"\x89PNG"))
        self.assertEqual(self.request("GET", "/assets/unit/../catalog.json")["status"], 404)
        self.assertEqual(self.request("GET", "/assets/unit/not-a-number.png")["status"], 404)

    def test_dns_rebinding_and_csrf_controls(self):
        self.assertEqual(
            self.request("GET", "/api/bootstrap", host="attacker.invalid")["status"],
            400,
        )
        revision = self.bootstrap()["catalog_revision"]
        payload = {"expected_revision": revision}
        self.assertEqual(
            self.post("/api/export", payload, token=False)["status"], 400
        )
        self.assertEqual(
            self.post("/api/export", payload, origin=False)["status"], 400
        )
        self.assertEqual(
            self.post(
                "/api/export", payload, content_type="text/plain"
            )["status"],
            400,
        )
        exported = self.post("/api/export", payload)
        self.assertEqual(exported["status"], 200)
        self.assertEqual(
            exported["json"]["manifest"]["catalog_revision"], revision
        )
        self.assertTrue((self.root / "bundle" / "client.json").is_file())

    def test_crud_position_backup_conflict_retirement_and_restore(self):
        bootstrap = self.bootstrap()
        revision = bootstrap["catalog_revision"]
        machine = copy.deepcopy(fixture_catalog()["fixed_machines"][0])
        machine.update(
            {
                "id": 201,
                "name": "Created",
                "owner": "mod",
                "draw_policy": "independent_weighted",
            }
        )
        created = self.post(
            "/api/machines/create",
            {
                "expected_revision": revision,
                "kind": "fixed_machines",
                "machine": machine,
                "position": 1,
            },
        )
        self.assertEqual(created["status"], 200, created.get("json"))
        result = created["json"]
        self.assertEqual(result["machine"]["position"], 1)
        self.assertEqual(result["machine"]["location"]["page"], 1)
        self.assertEqual(result["machine"]["location"]["slot"], 2)
        self.assertTrue(Path(result["backup_path"]).is_file())
        saved = catalog_lib.read_json(self.catalog_path)
        self.assertEqual([item["id"] for item in saved["fixed_machines"]], [201, 101])

        stale = self.post(
            "/api/machines/update",
            {
                "expected_revision": revision,
                "kind": "fixed_machines",
                "id": 201,
                "machine": {**machine, "price": 500},
                "position": 1,
            },
        )
        self.assertEqual(stale["status"], 409)

        detail = self.request(
            "GET", "/api/machine?kind=fixed_machines&id=201"
        )["json"]
        updated_machine = copy.deepcopy(detail["machine"])
        updated_machine["price"] = 2500
        updated = self.post(
            "/api/machines/update",
            {
                "expected_revision": detail["catalog_revision"],
                "kind": "fixed_machines",
                "id": 201,
                "machine": updated_machine,
                "position": 2,
            },
        )
        self.assertEqual(updated["status"], 200)
        self.assertEqual(updated["json"]["machine"]["position"], 2)

        retired = self.post(
            "/api/machines/retire",
            {
                "expected_revision": updated["json"]["catalog_revision"],
                "kind": "fixed_machines",
                "id": 201,
            },
        )
        self.assertEqual(retired["status"], 200)
        self.assertFalse(retired["json"]["machine"]["value"]["enabled"])
        self.assertTrue(retired["json"]["machine"]["value"]["retired"])
        self.assertIsNone(retired["json"]["machine"]["native_location"])

        restored = self.post(
            "/api/machines/restore",
            {
                "expected_revision": retired["json"]["catalog_revision"],
                "kind": "fixed_machines",
                "id": 201,
            },
        )
        self.assertEqual(restored["status"], 200)
        self.assertTrue(restored["json"]["machine"]["value"]["enabled"])
        self.assertFalse(restored["json"]["machine"]["value"]["retired"])
        self.assertEqual(restored["json"]["machine"]["native_location"]["position"], 1)

    def test_invalid_write_and_unsupported_fixed_currency_do_not_change_catalog(self):
        bootstrap = self.bootstrap()
        original = self.catalog_path.read_bytes()
        machine = copy.deepcopy(fixture_catalog()["fixed_machines"][0])
        machine["slots"][0]["weight"] += 1
        invalid = self.post(
            "/api/machines/update",
            {
                "expected_revision": bootstrap["catalog_revision"],
                "kind": "fixed_machines",
                "id": 101,
                "machine": machine,
                "position": 1,
            },
        )
        self.assertEqual(invalid["status"], 400)
        self.assertEqual(self.catalog_path.read_bytes(), original)

        incoming = fixture_catalog()
        incoming["fixed_machines"][0]["currency"] = "MB"
        unsupported = self.post(
            "/api/catalog/import",
            {
                "expected_revision": bootstrap["catalog_revision"],
                "catalog": incoming,
            },
        )
        self.assertEqual(unsupported["status"], 400)
        self.assertIn("GP only", unsupported["json"]["error"])
        self.assertEqual(self.catalog_path.read_bytes(), original)

    def test_import_reports_new_and_moved_positions(self):
        bootstrap = self.bootstrap()
        incoming = fixture_catalog()
        second = copy.deepcopy(incoming["fixed_machines"][0])
        second["id"] = 201
        second["name"] = "Imported"
        second["owner"] = "mod"
        second["draw_policy"] = "independent_weighted"
        incoming["fixed_machines"].insert(0, second)
        response = self.post(
            "/api/catalog/import",
            {
                "expected_revision": bootstrap["catalog_revision"],
                "catalog": incoming,
            },
        )
        self.assertEqual(response["status"], 200, response.get("json"))
        changes = {
            (item["kind"], item["id"]): item
            for item in response["json"]["position_changes"]
        }
        self.assertEqual(changes[("fixed_machines", 201)]["position"], 1)
        self.assertEqual(changes[("fixed_machines", 101)]["position"], 2)
        self.assertEqual(
            response["json"]["counts"],
            {"fixed_machines": 2, "custom_machines": 1},
        )

    def test_fixed_display_location_uses_eight_slots_per_page(self):
        self.assertEqual(
            capsule_ui._display_location("fixed_machines", 79),
            {"position": 79, "page": 10, "slot": 7, "slots_per_page": 8},
        )
        self.assertEqual(
            capsule_ui._display_location("fixed_machines", 81),
            {"position": 81, "page": 11, "slot": 1, "slots_per_page": 8},
        )

    def test_native_fixed_order_matches_observed_103_on_page_one_slot_two(self):
        catalog = fixture_catalog()
        model = catalog["fixed_machines"][0]
        catalog["fixed_machines"] = [
            {**copy.deepcopy(model), "id": 101 + index * 100}
            for index in range(104)
        ]
        positions = capsule_ui._native_positions(catalog)
        self.assertEqual(positions[("fixed_machines", 10401)], 1)
        self.assertEqual(positions[("fixed_machines", 10301)], 2)
        self.assertEqual(positions[("fixed_machines", 7901)], 26)
        self.assertEqual(capsule_ui._display_location("fixed_machines", 26)["page"], 4)

    def test_retirement_preserves_management_order_and_reflows_native_position(self):
        catalog = fixture_catalog()
        model = catalog["fixed_machines"][0]
        catalog["fixed_machines"] = []
        for index in range(79):
            machine = copy.deepcopy(model)
            machine["id"] = 101 + index * 100
            machine["name"] = f"Machine {index + 1}"
            catalog["fixed_machines"].append(machine)
        catalog_lib.write_json(self.catalog_path, catalog)
        store = capsule_ui.CatalogStore(self.catalog_path)
        app = capsule_ui.CapsuleApplication(
            store, Path(__file__).resolve().parents[1] / "ui" / "capsules"
        )
        before = app.bootstrap()
        last = before["machines"]["fixed_machines"][-1]
        self.assertEqual(last["position"], 79)
        self.assertEqual(last["native_location"]["page"], 1)
        self.assertEqual(last["native_location"]["slot"], 1)

        revision = before["catalog_revision"]
        retired = store.mutate_machine(
            "delete", "fixed_machines", 7901, revision
        )
        self.assertIsNone(retired["machine"]["native_location"])
        after = app.bootstrap()
        first = after["machines"]["fixed_machines"][0]
        last = after["machines"]["fixed_machines"][-1]
        self.assertEqual(first["position"], 1)
        self.assertEqual(first["native_location"]["position"], 78)
        self.assertEqual(first["native_location"]["page"], 10)
        self.assertEqual(first["native_location"]["slot"], 6)
        self.assertEqual(last["position"], 79)
        self.assertIsNone(last["native_location"])

    def test_versioned_client_compile_and_verified_reuse(self):
        client = self.root / "client"
        client.mkdir()
        lookup = self.root / "lookup.json"
        lookup.write_text("{}", encoding="utf-8")
        store = capsule_ui.CatalogStore(
            self.catalog_path,
            client_path=client,
            lookup_path=lookup,
            bundle_path=self.root / "compiled-bundle",
        )
        catalog, _ = store.read()
        revision = catalog_lib.digest(catalog)

        def fake_compile(value, client_path, lookup_path, output):
            output = Path(output)
            output.mkdir(parents=True, exist_ok=False)
            payload = b"compiled"
            (output / "payload.bin").write_bytes(payload)
            catalog_lib.write_json(
                output / "placements.json",
                {
                    "catalog_revision": revision,
                    "machines": [
                        {
                            "kind": "fixed_machines",
                            "id": 101,
                            "position": 1,
                            "page": 1,
                            "page_slot": 1,
                            "grid_id": 155001,
                        }
                    ],
                },
            )
            catalog_lib.write_json(
                output / "manifest.json",
                {
                    "catalog_revision": revision,
                    "compiler_version": COMPILER_VERSION,
                    "counts": {
                        "fixed": 1,
                        "coin": 0,
                        "custom": 1,
                        "candidates": 1,
                    },
                    "files": {
                        "payload.bin": __import__("hashlib").sha256(payload).hexdigest()
                    },
                },
            )
            return {
                "catalog_revision": revision,
                "counts": {"fixed": 1},
                "generated_cards": 2,
                "output": str(output),
            }

        with patch.object(
            capsule_ui, "_compile_catalog", side_effect=fake_compile
        ) as compiler:
            first = store.compile(
                revision, {"kind": "fixed_machines", "id": 101}
            )
            second = store.compile(
                revision, {"kind": "fixed_machines", "id": 101}
            )
        self.assertEqual(compiler.call_count, 1)
        self.assertFalse(first["reused"])
        self.assertTrue(second["reused"])
        self.assertEqual(first["selected_placement"]["page_slot"], 1)
        self.assertEqual(
            Path(first["output"]),
            self.root
            / "compiled-bundle"
            / "runtime"
            / revision[:16]
            / f"client-v{COMPILER_VERSION}",
        )
        self.assertFalse(list((Path(first["output"]).parent).glob("*.pending")))

        (Path(first["output"]) / "payload.bin").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            store.compile(revision)

    def test_apply_callback_capability_security_failure_and_success(self):
        client = self.root / "apply-client"
        client.mkdir()
        lookup = self.root / "apply-lookup.json"
        lookup.write_text("{}", encoding="utf-8")
        bundle = self.root / "apply-bundle"
        catalog = catalog_lib.read_json(self.catalog_path)
        revision = catalog_lib.digest(catalog)
        deployment_calls = []

        def deploy(value, runtime_output, bundle_root):
            deployment_calls.append((value, runtime_output, bundle_root))
            return {
                "applied": True,
                "message": "Applied configured deployment target",
                "client_target": str(self.root / "installed-client"),
            }

        store = capsule_ui.CatalogStore(
            self.catalog_path,
            client_path=client,
            lookup_path=lookup,
            bundle_path=bundle,
            apply_callback=deploy,
        )
        app = capsule_ui.CapsuleApplication(
            store, Path(__file__).resolve().parents[1] / "ui" / "capsules"
        )
        self.assertTrue(app.bootstrap()["capabilities"]["apply"])

        def fake_compile(value, client_path, lookup_path, output):
            output = Path(output)
            output.mkdir(parents=True, exist_ok=False)
            payload = b"compiled-for-apply"
            (output / "payload.bin").write_bytes(payload)
            catalog_lib.write_json(
                output / "placements.json",
                {
                    "catalog_revision": revision,
                    "machines": [
                        {
                            "kind": "fixed_machines",
                            "id": 101,
                            "position": 1,
                            "page": 1,
                            "page_slot": 1,
                            "grid_id": 155001,
                        }
                    ],
                },
            )
            catalog_lib.write_json(
                output / "manifest.json",
                {
                    "catalog_revision": revision,
                    "compiler_version": COMPILER_VERSION,
                    "counts": {
                        "fixed": 1,
                        "coin": 0,
                        "custom": 1,
                        "candidates": 1,
                    },
                    "files": {
                        "payload.bin": __import__("hashlib")
                        .sha256(payload)
                        .hexdigest()
                    },
                },
            )
            return {"generated_cards": 2}

        with patch.object(
            capsule_ui, "_compile_catalog", side_effect=fake_compile
        ) as compiler:
            result = app.dispatch_post(
                "/api/apply",
                {
                    "expected_revision": revision,
                    "selected": {"kind": "fixed_machines", "id": 101},
                },
            )
        self.assertEqual(compiler.call_count, 1)
        self.assertTrue(result["applied"])
        self.assertEqual(result["catalog_revision"], revision)
        self.assertEqual(result["selected_placement"]["grid_id"], 155001)
        self.assertEqual(len(result["placements"]), 1)
        self.assertEqual(len(deployment_calls), 1)
        deployed_catalog, runtime_output, bundle_root = deployment_calls[0]
        self.assertEqual(catalog_lib.digest(deployed_catalog), revision)
        self.assertEqual(runtime_output, Path(result["runtime_output"]))
        self.assertEqual(bundle_root, bundle.resolve())

        with self.assertRaisesRegex(
            capsule_ui.RequestError, "configured at server startup"
        ):
            app.dispatch_post(
                "/api/apply",
                {
                    "expected_revision": revision,
                    "client_target": "C:\\browser-controlled",
                },
            )

        store.apply_callback = lambda *_: {
            "applied": False,
            "message": "Target client is running; close it first",
        }
        refused = store.apply(revision)
        self.assertFalse(refused["applied"])
        self.assertEqual(len(refused["placements"]), 1)

        def fail(*_):
            raise RuntimeError("deployment failed")

        store.apply_callback = fail
        with self.assertRaisesRegex(capsule_ui.RequestError, "deployment failed"):
            store.apply(revision)


if __name__ == "__main__":
    unittest.main()
