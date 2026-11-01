import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import capsule_catalog
import capsule_deployment as deployment
from capsule_client_compile import COMPILER_VERSION


def catalog_fixture():
    units = [
        {
            "id": index,
            "name": f"Unit {index}",
            "grade": "C",
            "rank": 1,
            "subgrade": 2,
            "eligible": True,
            "transform_to": 0,
            "collection": False,
            "has_inventory_category": True,
            "capsule_sprites": [],
            "review_placeholder": False,
        }
        for index in range(1, 13)
    ]
    fixed = {
        "id": 7901,
        "grid_id": 155079,
        "name": "Weighted",
        "enabled": True,
        "owner": "initialization",
        "currency": "GP",
        "price": 1200,
        "draw_policy": "independent_weighted",
        "slots": [
            {
                "unit_id": index,
                "weight": weight,
                "native_draw_group": group,
                "sprites": [],
            }
            for index, weight, group in zip(
                range(1, 7),
                (20, 100, 245, 245, 250, 140),
                (1, 3, 5, 5, 5, 5),
            )
        ],
    }
    custom = {
        "id": 1,
        "name": "Custom",
        "enabled": True,
        "owner": "native",
        "currency_type": 0,
        "price": 2000,
        "draw_policy": "native_mileage",
        "slot_rules": [
            {
                "slot": index,
                "rank_max": 4 if index <= 3 or index >= 7 else 1,
                "subgrades": [2, 5, 4, 3] if index <= 3 or index >= 7 else [2],
                "extension": index >= 7,
            }
            for index in range(1, 9)
        ],
        "candidates": [
            {
                "unit_id": 7,
                "basic_weight": 800,
                "mileage": 0.25,
                "extra_price": 50,
                "reset_mileage": False,
            }
        ],
    }
    return {
        "schema_version": 1,
        "status": "authoring-only",
        "source_identity": {},
        "units": units,
        "fixed_machines": [fixed],
        "custom_machines": [custom],
    }


def write_pe(path, timestamp=deployment.HOST_TIMESTAMP,
             size_of_image=deployment.HOST_SIZE_OF_IMAGE, machine=0x14C):
    data = bytearray(0x300)
    data[:2] = b"MZ"
    pe = 0x80
    struct.pack_into("<I", data, 0x3C, pe)
    data[pe:pe + 4] = b"PE\0\0"
    struct.pack_into("<H", data, pe + 4, machine)
    struct.pack_into("<H", data, pe + 6, 3)
    struct.pack_into("<I", data, pe + 8, timestamp)
    struct.pack_into("<H", data, pe + 20, 0xE0)
    struct.pack_into("<H", data, pe + 24, 0x10B)
    struct.pack_into("<I", data, pe + 24 + 0x38, size_of_image)
    Path(path).write_bytes(data)


def symbol_fragment():
    sections = {
        "capsule.main.open_file": ("0x007E5600", "55 8B EC"),
        "capsule.main.file_ctor": ("0x007E95E0", "55 8B EC"),
        "capsule.main.operator_new": ("0x006F481C", "8B FF 55"),
        "capsule.main.wfopen": ("0x00887D78", "8B FF 55"),
        "capsule.grid_verify_checksum": ("0x00014980", "55 8B EC"),
        "capsule.grid_load_all": ("0x00031E50", "56 8B F1"),
    }
    return "\n".join(
        f"[{name}]\nkind=code\nrva={rva}\nexpect={expect}\n"
        for name, (rva, expect) in sections.items()
    )


def make_stage(root):
    stage = root / "stage"
    (stage / "sdgo/mods/capsule_catalog").mkdir(parents=True)
    (stage / "sdgo/symbols").mkdir(parents=True)
    (stage / "d3d9.dll").write_bytes(b"FRAMEWORK loader v2")
    (stage / "sdgo/core.dll").write_bytes(b"FRAMEWORK core v2")
    (stage / "sdgo/sdgo.ini").write_text(
        "[core]\nlog_level=1\n\n[loader]\n; chain_d3d9=d3d9_chain.dll\n",
        encoding="utf-8",
    )
    (stage / "sdgo/mods/capsule_catalog/capsule_catalog.dll").write_bytes(
        b"CAPSULE MOD DLL"
    )
    (stage / "sdgo/mods/capsule_catalog/mod.ini").write_text(
        "[mod]\nid=capsule_catalog\nname=Capsule catalog runtime\n"
        "version=1.0.0\napi=1\ndll=capsule_catalog.dll\n"
        "order=30\nhosts=GOnline-54BE1568-00C97000\n"
        "requires=capsule.main.open_file,capsule.main.file_ctor,"
        "capsule.main.operator_new,capsule.main.wfopen,"
        "capsule.grid_verify_checksum,capsule.grid_load_all,grid.data,grid.find\n",
        encoding="utf-8",
    )
    (stage / "sdgo/symbols" / deployment.HOST_SYMBOL_NAME).write_text(
        "[host]\nimage=GOnline.exe\ntimestamp=0x54BE1568\n"
        "size_of_image=0x00C97000\n\n"
        "[unrelated.stage]\nkind=code\nrva=0x1\nexpect=90\n\n"
        "[grid.data]\nkind=code\nrva=0x00001D40\nexpect=55 8B EC\n\n"
        "[grid.find]\nkind=code\nrva=0x0019D2B0\nexpect=55 8B EC\n",
        encoding="utf-8",
    )
    fragment = root / "symbol-proposal.ini"
    fragment.write_text(symbol_fragment(), encoding="utf-8")
    return stage, fragment


def make_runtime(root, catalog, runtime_revision=None, placements_revision=None):
    revision = capsule_catalog.digest(catalog)
    runtime = root / f"client-v{COMPILER_VERSION}"
    (runtime / "data").mkdir(parents=True)
    (runtime / "assets/sprs").mkdir(parents=True)
    files = {}
    payloads = {
        "data/0033.gdt": b"fixed",
        "data/0101.gdt": b"coin",
        "data/0154.gdt": b"custom",
        "data/0155.gdt": b"candidates",
        "ui/1161.uir": b"capsule-price-layout",
        "overlay.zdx": b"overlay-index-native",
        "overlay.zpk": b"overlay-pack-native",
        "assets/sprs/60000.spr": b"sprite",
    }
    for relative, data in payloads.items():
        path = runtime / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        files[relative] = hashlib.sha256(data).hexdigest()
    actual_runtime_revision = runtime_revision or revision
    (runtime / "runtime.ini").write_text(
        "[runtime]\nschema=1\n"
        f"catalog_revision={actual_runtime_revision}\n"
        "asset_root=assets\noverlay=overlay\n"
        f"overlay_zdx_sha256={files['overlay.zdx']}\n"
        f"overlay_zpk_sha256={files['overlay.zpk']}\n\n"
        "[gdt]\n"
        + "\n".join(
            (
                f"{table}=data\\{table}.gdt\n"
                f"{table}_sha256={files[f'data/{table}.gdt']}"
            )
            for table in ("0033", "0101", "0154", "0155")
        )
        + f"\n\n[ui]\n1161=ui\\1161.uir\n1161_sha256={files['ui/1161.uir']}"
        + "\n\n[counts]\nfixed=1\ncoin=0\ncustom=1\ncandidates=1\n",
        encoding="ascii",
    )
    (runtime / "placements.json").write_text(
        json.dumps(
            {
                "catalog_revision": placements_revision or revision,
                "machines": [
                    {
                        "kind": "fixed_machines",
                        "id": 7901,
                        "position": 1,
                        "page": 1,
                        "page_slot": 1,
                        "grid_id": 155079,
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    (runtime / "overlay-index.json").write_text(
        json.dumps({"chunk_size": 65536, "entries": []}), encoding="utf-8"
    )
    (runtime / "manifest.json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "compiler_version": COMPILER_VERSION,
                "catalog_revision": revision,
                "counts": {"fixed": 1, "coin": 0, "custom": 1, "candidates": 1},
                "files": files,
                "generated_cards": [],
                "source_gdt_extras": {},
                "source_index_sha256": "0" * 64,
            }
        ),
        encoding="utf-8",
    )
    return runtime


def make_client(root, *, own_loader=False, foreign_loader=False, own_core=False,
                old_mod=False):
    client = root / "client"
    client.mkdir(parents=True)
    write_pe(client / "GOnline.exe")
    if own_loader:
        (client / "d3d9.dll").write_bytes(b"FRAMEWORK loader old")
    elif foreign_loader:
        (client / "d3d9.dll").write_bytes(b"FOREIGN wrapper")
    if own_core:
        (client / "sdgo").mkdir()
        (client / "sdgo/core.dll").write_bytes(b"FRAMEWORK core old")
    (client / "sdgo/mods/unrelated").mkdir(parents=True, exist_ok=True)
    (client / "sdgo/mods/unrelated/keep.bin").write_bytes(b"KEEP")
    (client / "sdgo/sdgo.ini").write_text(
        "; user config\n[core]\nlog_level=0\n[mods]\nunrelated=1\n",
        encoding="utf-8",
    )
    (client / "sdgo/symbols").mkdir(parents=True)
    (client / "sdgo/symbols" / deployment.HOST_SYMBOL_NAME).write_text(
        "; user symbols\n[host]\nimage=GOnline.exe\n"
        "timestamp=0x54BE1568\nsize_of_image=0x00C97000\n\n"
        "[grid.data]\nkind=code\nrva=0x00001D40\nexpect=55 8B EC\n\n"
        "[grid.find]\nkind=code\nrva=0x0019D2B0\nexpect=55 8B EC\n\n"
        "[unrelated.user]\nkind=code\nrva=0xABC\nexpect=90\n\n"
        "[capsule.main.open_file]\nkind=code\nrva=0xDEAD\nexpect=CC\n",
        encoding="utf-8",
    )
    if old_mod:
        mod = client / "sdgo/mods/capsule_catalog"
        mod.mkdir()
        (mod / "capsule_catalog.dll").write_bytes(b"OLD MOD")
        (mod / "mod.ini").write_text(
            "[mod]\nid=capsule_catalog\nenabled=0\nversion=0.8\n"
            "[settings]\npreview=1\n",
            encoding="utf-8",
        )
        (mod / "old-only.bin").write_bytes(b"OLD")
    return client


def product_name(path):
    data = Path(path).read_bytes()
    if data.startswith(b"FRAMEWORK"):
        return deployment.PRODUCT_NAME
    if data.startswith(b"FOREIGN"):
        return "Foreign graphics wrapper"
    return None


class FakeCursor:
    def __init__(self, connection):
        self.connection = connection
        self.description = None
        self.rows = []

    def execute(self, sql):
        self.connection.executed.append(sql)
        self.description = None
        self.rows = []
        if sql.strip() == "BEGIN TRANSACTION;":
            self.connection.transaction_depth += 1
        elif "@@TRANCOUNT AS OpenTransactions" in sql:
            self.description = [("OpenTransactions",)]
            self.rows = [(self.connection.transaction_depth,)]
        elif "HAS_PERMS_BY_NAME" in sql:
            columns = ["DatabaseName", *deployment.SERVER_PERMISSIONS]
            values = [self.connection.database, *self.connection.permissions]
            self.description = [(name,) for name in columns]
            self.rows = [tuple(values)]
        elif "AS Verified" in sql:
            self.description = [
                ("CatalogRevision",),
                ("WeightedMachines",),
                ("WeightedSlots",),
                ("EnabledCustomMachines",),
                ("EnabledCustomCandidates",),
                ("Verified",),
            ]
            self.rows = [
                (
                    self.connection.revision,
                    1,
                    6,
                    1,
                    1,
                    int(self.connection.verify_passes),
                )
            ]
        elif "InstalledUtc" in sql and "WeightedMachines" in sql:
            # Manual-commit ODBC opens an implicit outer transaction for the
            # install batch. Its internal BEGIN/COMMIT is balanced.
            self.connection.transaction_depth += 1
            self.description = [
                ("CatalogRevision",),
                ("InstalledUtc",),
                ("WeightedMachines",),
                ("CustomMachines",),
            ]
            self.rows = [(self.connection.revision, "now", 1, 1)]
        return self

    def fetchall(self):
        return list(self.rows)

    def nextset(self):
        return False


class FakeConnection:
    def __init__(self, revision, database="GOnlineGame", permissions=None,
                 verify_passes=True):
        self.revision = revision
        self.database = database
        self.permissions = permissions or [1] * len(deployment.SERVER_PERMISSIONS)
        self.verify_passes = verify_passes
        self.executed = []
        self.commits = 0
        self.rollbacks = 0
        self.closed = False
        self.transaction_depth = 0
        self.persisted = False
        self.leave_transaction_open = False

    def cursor(self):
        return FakeCursor(self)

    def commit(self):
        self.commits += 1
        if not self.leave_transaction_open:
            self.transaction_depth = max(0, self.transaction_depth - 1)
        self.persisted = self.transaction_depth == 0

    def rollback(self):
        self.rollbacks += 1
        self.transaction_depth = 0

    def close(self):
        self.closed = True


class DeploymentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.catalog = catalog_fixture()
        self.revision = capsule_catalog.digest(self.catalog)
        self.stage, self.fragment = make_stage(self.root)
        self.runtime = make_runtime(self.root, self.catalog)
        self.bundle = self.root / "bundle"

    def target(self, client, server_ini=None):
        return deployment.DeploymentTarget(
            client_target=client,
            server_ini=server_ini,
            framework_stage=self.stage,
            symbol_fragment=self.fragment,
        )

    def apply(self, client, server_ini=None, connection=None):
        patches = [
            patch.object(deployment, "_product_name", side_effect=product_name),
            patch.object(deployment, "_running_gonline_paths", return_value=[]),
        ]
        if connection is not None:
            patches.append(
                patch.object(deployment, "_sql_connect", return_value=connection)
            )
        for item in patches:
            item.start()
        self.addCleanup(lambda: [item.stop() for item in reversed(patches)])
        return self.target(client, server_ini).apply(
            copy.deepcopy(self.catalog), self.runtime, self.bundle
        )

    def test_client_only_apply_builds_safe_package_and_preserves_user_state(self):
        client = make_client(
            self.root, own_loader=True, own_core=True, old_mod=True
        )
        original_ini = (client / "sdgo/sdgo.ini").read_bytes()
        (client / "sdgo/sdgo.ini").write_bytes(original_ini + b"capsule_catalog=0\n")
        result = self.apply(client)
        self.assertTrue(result["applied"])
        self.assertFalse(result["server_applied"])
        self.assertFalse(result["needs_server_restart"])
        self.assertIn("Server configuration was not changed", result["message"])
        self.assertEqual((client / "d3d9.dll").read_bytes(), b"FRAMEWORK loader v2")
        self.assertEqual(
            (client / "sdgo/mods/unrelated/keep.bin").read_bytes(), b"KEEP"
        )
        installed_ini = (client / "sdgo/sdgo.ini").read_bytes()
        self.assertTrue(installed_ini.startswith(original_ini))
        self.assertIn(b"capsule_catalog=1", installed_ini)
        self.assertNotIn(b"capsule_catalog=0", installed_ini)
        symbols = (
            client / "sdgo/symbols" / deployment.HOST_SYMBOL_NAME
        ).read_text(encoding="utf-8")
        self.assertIn("[unrelated.user]", symbols)
        self.assertIn("rva=0x007E5600", symbols)
        mod_ini = (client / "sdgo/mods/capsule_catalog/mod.ini").read_text(
            encoding="utf-8"
        )
        self.assertIn("enabled=1", mod_ini)
        self.assertIn("[settings]", mod_ini)
        self.assertIn("preview=1", mod_ini)
        backup = Path(result["backup_path"])
        self.assertEqual(
            (backup / "previous/sdgo/mods/capsule_catalog/old-only.bin").read_bytes(),
            b"OLD",
        )

        package = Path(result["package_path"])
        package_manifest = json.loads(
            (package / "manifest.json").read_text(encoding="utf-8")
        )
        self.assertEqual(package_manifest["catalog_revision"], self.revision)
        self.assertTrue((package / "semantic/client.json").is_file())
        self.assertTrue((package / "semantic/server.json").is_file())
        self.assertTrue((package / "player/symbol-fragment.ini").is_file())
        self.assertTrue((package / "player/package-manifest.json").is_file())
        self.assertTrue((package / "player/Install-Client.ps1").is_file())
        self.assertTrue((package / "player/Install-Client.cmd").is_file())
        full_symbols = (
            package
            / "player/client/sdgo/symbols"
            / deployment.HOST_SYMBOL_NAME
        ).read_text(encoding="utf-8")
        friend_fragment = (
            package / "player/symbol-fragment.ini"
        ).read_text(encoding="utf-8")
        self.assertIn("[grid.data]", full_symbols)
        self.assertIn("[grid.find]", full_symbols)
        self.assertIn("[capsule.main.open_file]", full_symbols)
        self.assertIn("[host]", friend_fragment)
        self.assertIn("[capsule.main.open_file]", friend_fragment)
        self.assertNotIn("[grid.data]", friend_fragment)
        self.assertTrue((package / "server/install.sql").is_file())
        self.assertTrue(
            (package / "player/client/sdgo/mods/capsule_catalog/runtime.ini").is_file()
        )
        all_names = "\n".join(package_manifest["files"]).lower()
        self.assertNotIn("password", all_names)
        self.assertNotIn("capsule_local_test_env", all_names)
        self.assertNotIn("capsule_currency_fix", all_names)
        self.assertNotIn(str(self.root).lower(), (package / "manifest.json").read_text().lower())
        player_manifest = json.loads(
            (package / "player/package-manifest.json").read_text(encoding="utf-8")
        )
        client_files = {
            path.relative_to(package / "player/client").as_posix()
            for path in (package / "player/client").rglob("*")
            if path.is_file()
        }
        self.assertEqual(set(player_manifest["files"]), client_files)
        packaged_mod = (
            package / "player/client/sdgo/mods/capsule_catalog/mod.ini"
        ).read_text(encoding="utf-8")
        self.assertIn("enabled=1", packaged_mod)

    def test_foreign_loader_is_chained_and_sdgo_ini_is_minimally_updated(self):
        client = make_client(self.root, foreign_loader=True, own_core=True)
        result = self.apply(client)
        self.assertTrue(result["applied"])
        self.assertEqual((client / "d3d9_chain.dll").read_bytes(), b"FOREIGN wrapper")
        self.assertEqual((client / "d3d9.dll").read_bytes(), b"FRAMEWORK loader v2")
        ini = (client / "sdgo/sdgo.ini").read_text(encoding="utf-8")
        self.assertIn("; user config", ini)
        self.assertIn("unrelated=1", ini)
        self.assertIn("chain_d3d9=d3d9_chain.dll", ini)

    def test_foreign_loader_with_existing_chain_is_rejected_without_changes(self):
        client = make_client(self.root, foreign_loader=True, own_core=True)
        (client / "d3d9_chain.dll").write_bytes(b"EXISTING CHAIN")
        original = (client / "d3d9.dll").read_bytes()
        with self.assertRaisesRegex(ValueError, "both a foreign"):
            self.apply(client)
        self.assertEqual((client / "d3d9.dll").read_bytes(), original)
        self.assertFalse((client / "sdgo/mods/capsule_catalog").exists())

    def test_foreign_core_is_rejected(self):
        client = make_client(self.root, own_loader=True)
        (client / "sdgo/core.dll").write_bytes(b"FOREIGN core")
        with self.assertRaisesRegex(ValueError, "another product"):
            self.apply(client)

    def test_running_exact_target_refuses_without_connecting_or_writing(self):
        client = make_client(self.root, own_loader=True, own_core=True)
        original = (client / "d3d9.dll").read_bytes()
        target = self.target(client)
        with patch.object(deployment, "_product_name", side_effect=product_name), patch.object(
            deployment,
            "_running_gonline_paths",
            return_value=[str(client / "GOnline.exe")],
        ), patch.object(deployment, "_sql_connect") as connector:
            result = target.apply(self.catalog, self.runtime, self.bundle)
        self.assertFalse(result["applied"])
        self.assertIn("close that client", result["message"])
        self.assertFalse(connector.called)
        self.assertEqual((client / "d3d9.dll").read_bytes(), original)

    def test_wrong_pe_build_and_architecture_are_rejected(self):
        client = make_client(self.root, own_loader=True, own_core=True)
        write_pe(client / "GOnline.exe", timestamp=1)
        with self.assertRaisesRegex(ValueError, "Unsupported GOnline.exe build"):
            self.apply(client)
        write_pe(client / "GOnline.exe", machine=0x8664)
        with self.assertRaisesRegex(ValueError, "32-bit x86"):
            self.apply(client)

    def test_non_capsule_symbol_conflict_rejects_but_capsule_section_updates(self):
        client = make_client(self.root, own_loader=True, own_core=True)
        symbol = client / "sdgo/symbols" / deployment.HOST_SYMBOL_NAME
        text = symbol.read_text(encoding="utf-8").replace(
            "rva=0x00001D40", "rva=0x99999999"
        )
        symbol.write_text(text, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "non-capsule symbol section conflicts"):
            self.apply(client)

    def test_compiled_revision_and_manifest_traversal_are_fail_closed(self):
        client = make_client(self.root, own_loader=True, own_core=True)
        bad_runtime = make_runtime(
            self.root / "bad-revision",
            self.catalog,
            runtime_revision="0" * 64,
        )
        target = self.target(client)
        with patch.object(deployment, "_product_name", side_effect=product_name), patch.object(
            deployment, "_running_gonline_paths", return_value=[]
        ):
            with self.assertRaisesRegex(ValueError, "runtime.ini catalog revision differs"):
                target.apply(self.catalog, bad_runtime, self.root / "bad-bundle")

        manifest = json.loads((self.runtime / "manifest.json").read_text())
        manifest["files"]["../escape.bin"] = "0" * 64
        (self.runtime / "manifest.json").write_text(json.dumps(manifest))
        with patch.object(deployment, "_product_name", side_effect=product_name), patch.object(
            deployment, "_running_gonline_paths", return_value=[]
        ):
            with self.assertRaisesRegex(ValueError, "Unsafe relative path"):
                target.apply(self.catalog, self.runtime, self.root / "traversal-bundle")

    def test_server_apply_uses_explicit_ini_mock_sql_and_requests_manual_restart(self):
        client = make_client(self.root, own_loader=True, own_core=True)
        ini = self.root / "server.ini"
        ini.write_text(
            "[DB Game]\nDBServerName=127.0.0.1,63401\nDBName=GOnlineGame\n"
            "ID=private.app_user\nPWD=app_password\n",
            encoding="utf-8",
        )
        connection = FakeConnection(self.revision)
        result = self.apply(client, ini, connection)
        self.assertTrue(result["applied"])
        self.assertTrue(result["server_applied"])
        self.assertTrue(result["needs_server_restart"])
        self.assertEqual(connection.commits, 1)
        self.assertTrue(connection.persisted)
        self.assertEqual(connection.transaction_depth, 0)
        self.assertGreaterEqual(connection.rollbacks, 1)
        self.assertTrue(connection.closed)
        executed = "\n".join(connection.executed)
        self.assertIn("HAS_PERMS_BY_NAME", executed)
        self.assertIn("BEGIN TRANSACTION", executed)
        self.assertIn("CREATE PROCEDURE dbo.sp_GetNextCapsule1", executed)
        rendered = json.dumps(result)
        self.assertNotIn("app_password", rendered)
        self.assertNotIn("private.app_user", rendered)

    def test_permission_failure_occurs_before_client_write(self):
        client = make_client(self.root, own_loader=True, own_core=True)
        original = (client / "d3d9.dll").read_bytes()
        ini = self.root / "server.ini"
        ini.write_text(
            "[DB Game]\nDBServerName=local\nDBName=GOnlineGame\n"
            "ID=app\nPWD=secret\n",
            encoding="utf-8",
        )
        permissions = [1] * len(deployment.SERVER_PERMISSIONS)
        permissions[2] = 0
        connection = FakeConnection(self.revision, permissions=permissions)
        with self.assertRaisesRegex(
            deployment.ServerDeploymentError, "lacks required"
        ):
            self.apply(client, ini, connection)
        self.assertEqual((client / "d3d9.dll").read_bytes(), original)
        self.assertFalse((client / "sdgo/mods/capsule_catalog").exists())
        self.assertTrue(connection.closed)

    def test_unfinished_commit_is_rejected_and_client_restored(self):
        client = make_client(self.root, own_loader=True, own_core=True)
        original = (client / "d3d9.dll").read_bytes()
        ini = self.root / "server.ini"
        ini.write_text(
            "[DB Game]\nDBServerName=local\nDBName=GOnlineGame\n"
            "ID=app\nPWD=secret\n", encoding="utf-8",
        )
        connection = FakeConnection(self.revision)
        connection.leave_transaction_open = True
        with self.assertRaisesRegex(
            deployment.ServerDeploymentError, "client was restored"
        ):
            self.apply(client, ini, connection)
        self.assertFalse(connection.persisted)
        self.assertEqual((client / "d3d9.dll").read_bytes(), original)
        self.assertFalse((client / "sdgo/mods/capsule_catalog").exists())
        self.assertGreaterEqual(connection.rollbacks, 2)

    def test_failed_server_verify_restores_previous_client_and_retains_failed_new(self):
        client = make_client(
            self.root, own_loader=True, own_core=True, old_mod=True
        )
        original_loader = (client / "d3d9.dll").read_bytes()
        original_mod = (client / "sdgo/mods/capsule_catalog/old-only.bin").read_bytes()
        ini = self.root / "server.ini"
        ini.write_text(
            "[DB Game]\nDBServerName=local\nDBName=GOnlineGame\n"
            "ID=app\nPWD=secret\n",
            encoding="utf-8",
        )
        connection = FakeConnection(self.revision, verify_passes=False)
        with self.assertRaisesRegex(
            deployment.ServerDeploymentError, "client was restored"
        ):
            self.apply(client, ini, connection)
        self.assertEqual((client / "d3d9.dll").read_bytes(), original_loader)
        self.assertEqual(
            (client / "sdgo/mods/capsule_catalog/old-only.bin").read_bytes(),
            original_mod,
        )
        backups = list(
            (client / "sdgo/backups/capsule_catalog").glob("*/failed-new")
        )
        self.assertEqual(len(backups), 1)
        self.assertGreaterEqual(connection.rollbacks, 2)
        self.assertEqual(connection.commits, 0)

    def test_old_compiler_output_is_rejected_before_client_write(self):
        client = make_client(self.root, own_loader=True, own_core=True)
        original = (client / "d3d9.dll").read_bytes()
        manifest_path = self.runtime / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["compiler_version"] = COMPILER_VERSION - 1
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Recompile the client runtime"):
            self.apply(client)
        self.assertEqual((client / "d3d9.dll").read_bytes(), original)
        self.assertFalse((client / "sdgo/mods/capsule_catalog").exists())

    @unittest.skipUnless(os.name == "nt", "Windows image mapping check")
    def test_mapped_image_is_detected_without_process_inventory(self):
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetModuleFileNameW.argtypes = [wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
        path = ctypes.create_unicode_buffer(32768)
        self.assertGreater(kernel.GetModuleFileNameW(None, path, len(path)), 0)
        with self.assertRaises(deployment.ClientRunningError):
            deployment._assert_client_image_unmapped(Path(path.value))

    def test_package_reuse_is_hash_checked(self):
        client = make_client(self.root, own_loader=True, own_core=True)
        first = self.apply(client)
        package = Path(first["package_path"])
        (package / "server/install.sql").write_text("tampered", encoding="utf-8")
        second_client = make_client(self.root / "second", own_loader=True, own_core=True)
        with self.assertRaisesRegex(ValueError, "package hash mismatch"):
            self.apply(second_client)

    def test_reparse_runtime_is_rejected_when_supported(self):
        if not hasattr(os, "symlink"):
            self.skipTest("symlinks unavailable")
        client = make_client(self.root, own_loader=True, own_core=True)
        external = self.root / "external.bin"
        external.write_bytes(b"x")
        link = self.runtime / "assets/sprs/link.bin"
        try:
            os.symlink(external, link)
        except OSError:
            self.skipTest("symlink creation is unavailable")
        with self.assertRaisesRegex(ValueError, "reparse point"):
            self.apply(client)


if __name__ == "__main__":
    unittest.main()
