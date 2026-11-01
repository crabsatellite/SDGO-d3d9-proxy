from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "collection_capsule_cli", ROOT / "tools" / "collection_capsule.py"
)
CLI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CLI)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def symbol_file(*, pick_rva: str = "0x10") -> bytes:
    return (
        "[host]\n"
        "timestamp=0x1\n"
        "size_of_image=0x2\n"
        "\n"
        "[collection.activity_pick]\n"
        "kind=code\n"
        f"rva={pick_rva}\n"
        "expect=AA BB\n"
        "\n"
        "[collection.connection_command]\n"
        "kind=site\n"
        "rva=0x20\n"
        "expect=CC DD\n"
    ).encode("utf-8")


class FakeCursor:
    def __init__(self, model, events):
        self.model = model
        self.events = events
        self.commands = []
        self.last = None
        self.closed = False

    def execute(self, statement, *parameters):
        self.events.append("sql:" + statement)
        self.commands.append((statement, parameters))
        self.last = statement
        return self

    def fetchone(self):
        if self.last == "SELECT DB_NAME()":
            return ("CollectionFixture",)
        if self.last.startswith(
            "SELECT s.MachineID,s.PriceGP,s.Enabled,s.PoolHash,"
        ):
            return (
                self.model["machine_id"],
                100000,
                True,
                CLI.pool_hash(self.model["pool"]),
                2, 0, 100000, 0,
            )
        raise AssertionError(f"Unexpected fixture fetch: {self.last}")

    def close(self):
        self.closed = True


class FakeConnection:
    def __init__(self, model):
        self.events = []
        self.fixture_cursor = FakeCursor(model, self.events)
        self.closed = False

    def cursor(self):
        return self.fixture_cursor

    def close(self):
        self.closed = True


class CollectionDeploymentTests(unittest.TestCase):
    def server_package(self, root: Path, unexpected: str | None = None):
        package = root / "package"
        model = {
            "schema_version": 1,
            "feature": "collection-capsule",
            "machine_id": 990001,
            "price_gp": 100000,
            "machine_kind": "activity",
            "source_units": [],
            "pool": {"units": [{"unit_id": 1}], "form_aliases": []},
        }
        package.mkdir()
        (package / "model.json").write_text(
            json.dumps(model), encoding="utf-8"
        )
        (package / "install.sql").write_text(
            "fixture install SQL", encoding="utf-8"
        )
        (package / "disable.sql").write_text(
            "fixture disable SQL", encoding="utf-8"
        )
        for relative in sorted(CLI.SERVER_FILES):
            path = package / "server" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(("fixture " + relative).encode("ascii"))
        if unexpected:
            path = package / "server" / unexpected
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"private host payload")
        files = {
            path.relative_to(package).as_posix(): sha256(path)
            for path in package.rglob("*")
            if path.is_file()
        }
        manifest = {
            "schema_version": 1,
            "feature": "collection-capsule",
            "machine_id": 990001,
            "price_gp": 100000,
            "machine_kind": "activity",
            "pool_revision": CLI.pool_hash(model["pool"]),
            "files": files,
        }
        (package / "manifest.json").write_text(
            json.dumps(manifest), encoding="utf-8"
        )
        return package, model

    def fixture(self, root: Path):
        package = root / "package"
        payload = package / "server"
        target = root / "world"
        (payload / "sdgo/symbols").mkdir(parents=True)
        (payload / "sdgo/mods/collection_capsule").mkdir(parents=True)
        (target / "sdgo").mkdir(parents=True)

        (payload / "winmm.dll").write_bytes(b"new loader")
        (payload / "sdgo/core.dll").write_bytes(b"new core")
        (payload / "sdgo/sdgo.ini").write_bytes(b"[core]\n")
        (payload / "sdgo/symbols" / CLI.HOST_SYMBOLS).write_bytes(symbol_file())
        for name, data in {
            "collection_capsule.dll": b"new mod",
            "mod.ini": b"[mod]\n",
            "manager.json": b'{}\n',
            "config.example.ini": b"[settings]\n",
            "config.ini": b"[settings]\nMachineID=990001\n",
        }.items():
            (payload / "sdgo/mods/collection_capsule" / name).write_bytes(data)

        executable = target / "WorldServer.exe"
        executable.write_bytes(b"verified fixture world")
        (target / "winmm.dll").write_bytes(b"old loader")
        (target / "sdgo/core.dll").write_bytes(b"old core")
        server_ini = target / "WorldServer.ini"
        server_ini.write_text("[DB Game]\n", encoding="ascii")
        model = {
            "machine_id": 990001,
            "price_gp": 100000,
            "pool": {"units": [{"unit_id": 1}], "form_aliases": []},
        }
        return package, payload, target, server_ini, executable, model

    def install(
        self,
        package: Path,
        target: Path,
        server_ini: Path,
        executable: Path,
        model,
        *,
        returncode: int,
        check_only: bool = False,
    ):
        database = FakeConnection(model)
        calls = []

        def installer(arguments, **kwargs):
            database.events.append("installer")
            calls.append((arguments, kwargs))
            source = Path(arguments[arguments.index("-Source") + 1])
            # Model a standard installer that has already copied its complete
            # payload before returning either success or a late failure.
            if target.joinpath("winmm.dll").is_file():
                shutil.copy2(target / "winmm.dll", target / "winmm_chain.dll")
            shutil.copytree(source, target, dirs_exist_ok=True)
            return SimpleNamespace(returncode=returncode, stdout="", stderr="")

        with (
            patch.object(CLI, "checked_model", return_value=(model, "fixture install SQL")),
            patch.object(CLI, "connection", return_value=database),
            patch.object(CLI, "registration_baseline", return_value=2),
            patch.object(CLI, "_world_is_running", return_value=False) as running,
            patch.object(CLI.subprocess, "run", side_effect=installer),
            patch.object(CLI, "HOST_SHA256", sha256(executable)),
        ):
            if returncode and not check_only:
                with self.assertRaisesRegex(RuntimeError, "World file install failed"):
                    CLI.install_server(package, server_ini, check_only=False)
                result = None
            else:
                result = CLI.install_server(
                    package, server_ini, check_only=check_only
                )
        return result, database, calls, running

    def test_symbol_merge_preserves_unrelated_sections_and_is_idempotent(self):
        existing = (
            "[host]\n"
            "timestamp=0x1\n"
            "size_of_image=0x2\n"
            "\n"
            "[unrelated.fixture]\n"
            "kind=data\n"
            "rva=0x30\n"
            "expect=EE\n"
        ).encode("utf-8")
        merged = CLI.merged_world_symbols(existing, symbol_file())
        text = merged.decode("utf-8")
        self.assertIn("[unrelated.fixture]", text)
        self.assertIn("[collection.activity_pick]", text)
        self.assertIn("[collection.connection_command]", text)
        self.assertEqual(CLI.merged_world_symbols(merged, symbol_file()), merged)
        with self.assertRaisesRegex(ValueError, "conflicts"):
            CLI.merged_world_symbols(merged, symbol_file(pick_rva="0x11"))

    def test_server_package_rejects_private_host_or_undeclared_scope_files(self):
        for index, unexpected in enumerate(
            ("WorldServer.ini", "WorldServer.exe", "sdgo/private-owner.ini")
        ):
            with self.subTest(unexpected=unexpected):
                with tempfile.TemporaryDirectory(
                    prefix=f"collection package scope {index} "
                ) as raw:
                    package, _ = self.server_package(Path(raw), unexpected)
                    with (
                        patch.object(
                            CLI, "installation", return_value="fixture install SQL"
                        ),
                        self.assertRaisesRegex(
                            ValueError, "outside this Mod/framework payload"
                        ),
                    ):
                        CLI.checked_model(package)

    def test_migration_retires_only_the_verified_ordinary_hook_symbol(self):
        existing = ("[host]\ntimestamp=0x1\nsize_of_image=0x2\n\n"
                    "[collection.pick]\nkind=code\nrva=0xE510\nexpect=55 8B EC 81 EC 60 07 00 00\n\n"
                    "[unrelated.fixture]\nkind=data\nrva=0x30\nexpect=EE\n").encode('utf-8')
        merged = CLI.merged_world_symbols(existing, symbol_file()).decode('utf-8')
        self.assertNotIn('[collection.pick]', merged)
        self.assertIn('[collection.activity_pick]', merged)
        self.assertIn('[unrelated.fixture]', merged)
        with self.assertRaisesRegex(ValueError, 'externally edited'):
            CLI.merged_world_symbols(existing.replace(b'rva=0xE510', b'rva=0xE511'), symbol_file())

    def test_scoped_restore_removes_generated_chain_and_restores_old_files(self):
        with tempfile.TemporaryDirectory(prefix="collection server files ") as raw:
            root = Path(raw)
            target, source = root / "world", root / "payload"
            (target / "sdgo").mkdir(parents=True)
            (source / "sdgo").mkdir(parents=True)
            (target / "winmm.dll").write_bytes(b"old loader")
            (target / "sdgo/core.dll").write_bytes(b"old core")
            (source / "winmm.dll").write_bytes(b"new loader")
            (source / "sdgo/core.dll").write_bytes(b"new core")
            files = CLI.ServerFiles(target, source)
            files.capture()
            (target / "winmm_chain.dll").write_bytes(b"old loader")
            (target / "winmm.dll").write_bytes(b"new loader")
            (target / "sdgo/core.dll").write_bytes(b"new core")
            files.observe()
            files.restore()
            self.assertEqual((target / "winmm.dll").read_bytes(), b"old loader")
            self.assertEqual((target / "sdgo/core.dll").read_bytes(), b"old core")
            self.assertFalse((target / "winmm_chain.dll").exists())
            self.assertTrue((files.backup / "before.json").is_file())

    def test_scoped_restore_retains_a_concurrent_external_edit(self):
        with tempfile.TemporaryDirectory(prefix="collection server conflict ") as raw:
            root = Path(raw)
            target, source = root / "world", root / "payload"
            target.mkdir()
            source.mkdir()
            (target / "winmm.dll").write_bytes(b"old")
            (source / "winmm.dll").write_bytes(b"new")
            files = CLI.ServerFiles(target, source)
            files.capture()
            (target / "winmm.dll").write_bytes(b"external")
            with self.assertRaisesRegex(RuntimeError, "Concurrent World file edits"):
                files.restore()
            self.assertEqual((target / "winmm.dll").read_bytes(), b"external")

    def test_check_only_connects_and_validates_without_installing_files(self):
        with tempfile.TemporaryDirectory(prefix="collection check only ") as raw:
            package, _, target, server_ini, executable, model = self.fixture(
                Path(raw)
            )
            before = {
                path.relative_to(target).as_posix(): sha256(path)
                for path in target.rglob("*")
                if path.is_file()
            }
            result, database, calls, running = self.install(
                package,
                target,
                server_ini,
                executable,
                model,
                returncode=0,
                check_only=True,
            )
            after = {
                path.relative_to(target).as_posix(): sha256(path)
                for path in target.rglob("*")
                if path.is_file()
            }
            self.assertEqual(before, after)
            self.assertEqual(result["database"], "CollectionFixture")
            self.assertFalse(result["applied"])
            self.assertEqual(calls, [])
            running.assert_not_called()
            self.assertTrue(database.closed)

    def test_success_commits_only_after_file_install(self):
        with tempfile.TemporaryDirectory(prefix="collection install success ") as raw:
            package, _, target, server_ini, executable, model = self.fixture(
                Path(raw)
            )
            result, database, calls, running = self.install(
                package,
                target,
                server_ini,
                executable,
                model,
                returncode=0,
            )
            commands = [statement for statement, _ in database.fixture_cursor.commands]
            self.assertEqual(commands[-1], "COMMIT TRANSACTION;")
            self.assertLess(
                database.events.index("installer"),
                database.events.index("sql:COMMIT TRANSACTION;"),
            )
            self.assertEqual((target / "winmm.dll").read_bytes(), b"new loader")
            self.assertEqual((target / "sdgo/core.dll").read_bytes(), b"new core")
            self.assertEqual(
                (target / "sdgo/mods/collection_capsule/collection_capsule.dll").read_bytes(),
                b"new mod",
            )
            self.assertEqual(result["price_gp"], 100000)
            self.assertTrue(Path(result["backup"]).is_dir())
            self.assertEqual(len(calls), 1)
            running.assert_called_once_with(target)
            self.assertTrue(database.closed)

    def test_late_file_installer_failure_rolls_back_database_and_files(self):
        with tempfile.TemporaryDirectory(prefix="collection install rollback ") as raw:
            package, _, target, server_ini, executable, model = self.fixture(
                Path(raw)
            )
            _, database, calls, running = self.install(
                package,
                target,
                server_ini,
                executable,
                model,
                returncode=1,
            )
            commands = [statement for statement, _ in database.fixture_cursor.commands]
            self.assertIn("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;", commands)
            self.assertNotIn("COMMIT TRANSACTION;", commands)
            self.assertLess(
                database.events.index("installer"),
                database.events.index(
                    "sql:IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;"
                ),
            )
            self.assertEqual((target / "winmm.dll").read_bytes(), b"old loader")
            self.assertEqual((target / "sdgo/core.dll").read_bytes(), b"old core")
            self.assertFalse((target / "winmm_chain.dll").exists())
            self.assertFalse(
                (target / "sdgo/mods/collection_capsule/collection_capsule.dll").exists()
            )
            backups = list((target / "sdgo-install-backups").glob("collection-*"))
            self.assertEqual(len(backups), 1)
            self.assertTrue((backups[0] / "before.json").is_file())
            self.assertEqual(len(calls), 1)
            running.assert_called_once_with(target)
            self.assertTrue(database.closed)


if __name__ == "__main__":
    unittest.main()
