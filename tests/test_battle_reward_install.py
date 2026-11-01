from __future__ import annotations

import configparser
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from owner.rewards.deployment import (
    DeploymentApplyError,
    DeploymentError,
    apply_plan,
    build_plan,
    pe_host_key,
    sha256_file,
)


VERSION = "1.1.4"
TIMESTAMP = 0x548A891D
IMAGE_SIZE = 0x0046C000
HOST_KEY = "BattleServer-548A891D-0046C000"
WALLET_SCOPE = uuid.UUID("12345678-1234-4abc-8def-123456789abc")


def write(path: Path, data: bytes | str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str):
        path.write_text(data, encoding="utf-8", newline="\n")
    else:
        path.write_bytes(data)
    return path


def fake_pe(path: Path, *, timestamp=TIMESTAMP, image_size=IMAGE_SIZE) -> Path:
    data = bytearray(512)
    data[:2] = b"MZ"
    nt = 0x80
    struct.pack_into("<I", data, 0x3C, nt)
    data[nt:nt + 4] = b"PE\0\0"
    struct.pack_into("<I", data, nt + 8, timestamp)
    struct.pack_into("<I", data, nt + 0x18 + 0x38, image_size)
    data[-32:] = b"synthetic-host-for-tests-only!!"
    return write(path, bytes(data))


def server_ini(path: Path) -> Path:
    return write(
        path,
        """
[DB Game]
DBServerName=synthetic
DBName=game
ID=test
PWD=not-used

[DB StoreBilling]
DBServerName=synthetic
DBName=billing
ID=test
PWD=not-used
""".lstrip(),
    )


def package(root: Path, *, host=HOST_KEY) -> Path:
    files: dict[str, bytes] = {
        "VERSION": (VERSION + "\n").encode("ascii"),
        "install_server.ps1": b"# synthetic installer\n",
        "install_common.ps1": b"# synthetic shared installer\n",
        "server/winmm.dll": b"latest-loader-" + VERSION.encode("ascii"),
        "server/sdgo/core.dll": b"latest-core-" + VERSION.encode("ascii"),
        "server/sdgo/sdgo.ini": (
            b"[core]\nlog_level=1\n[categories]\n[features]\n[mods]\n"
        ),
        "server/sdgo/mods/battle_rewards/battle_rewards.dll":
            b"latest-battle-rewards-" + VERSION.encode("ascii"),
        "server/sdgo/mods/battle_rewards/config.example.ini":
            b"[settings]\nRuntimeDirectory=\nInstanceId=\nTrace=0\n",
        "tools/install_battle_rewards.py": b"# immutable installer entry\n",
        "owner/rewards/deployment.py": b"# immutable deployment module\n",
        "owner/rewards/sql_repository.py": b"# immutable repository module\n",
        "owner/rewards/sql/battle_reward_wallet.sql":
            b"-- immutable owned ledger\n",
    }
    manifest = f"""
[mod]
id=battle_rewards
dll=battle_rewards.dll
hosts={host}
requires=battle.rewards.net_send,battle.rewards.net_frame_proc
files=config.example.ini
config_files=config.ini
enabled=0

[settings]
RuntimeDirectory=
InstanceId=
Trace=0
""".lstrip().encode("utf-8")
    symbols = f"""
[host]
name=BattleServer

[battle.rewards.net_send]
rva=0x00324020

[battle.rewards.net_frame_proc]
rva=0x00001234
""".lstrip().encode("utf-8")
    files["server/sdgo/mods/battle_rewards/mod.ini"] = manifest
    files[f"server/sdgo/symbols/{host}.ini"] = symbols
    for relative, data in files.items():
        write(root / relative, data)
    checksums = {
        relative: hashlib.sha256(data).hexdigest()
        for relative, data in files.items()
    }
    write(
        root / "SHA256.json",
        json.dumps(checksums, indent=2, sort_keys=True) + "\n",
    )
    return root


def installed_server(root: Path) -> Path:
    battle = root / "战斗 服务端"
    fake_pe(battle / "BattleServer.exe")
    write(battle / "winmm.dll", b"foreign-loader")
    write(battle / "sdgo/core.dll", b"old-core")
    write(
        battle / "sdgo/sdgo.ini",
        """
[core]
log_level=3
[categories]
gameplay=0
[custom]
keep=yes
""".lstrip(),
    )
    write(
        battle / f"sdgo/symbols/{HOST_KEY}.ini",
        """
[host]
name=BattleServer

[unrelated.symbol]
rva=0x0000ABCD
""".lstrip(),
    )
    write(
        battle / "sdgo/mods/other_mod/mod.ini",
        "[mod]\nid=other_mod\nenabled=1\n",
    )
    write(
        battle / "sdgo/mods/other_mod/preserve.bin",
        b"unrelated-mod-must-survive",
    )
    write(
        battle / "sdgo/mods/battle_rewards/mod.ini",
        """
[mod]
id=battle_rewards
dll=battle_rewards.dll
enabled=0

[settings]
OwnerSetting=keep-me
""".lstrip(),
    )
    write(
        battle / "sdgo/mods/battle_rewards/config.ini",
        "[settings]\nOwnerNote=保留\nTrace=1\n",
    )
    write(
        battle / "sdgo/mods/battle_rewards/old-owner-note.txt",
        "do not delete\n",
    )
    return battle


class FakeCursor:
    def __init__(self, connection):
        self.connection = connection
        self.rows = []

    def execute(self, sql, *parameters):
        self.connection.queries.append((sql, parameters))
        if "tbl_MissionRankBossData" in sql:
            self.rows = [(101, 201, 202, None, 0, None)]
        elif "tbl_NpcData" in sql:
            self.rows = [(201,), (202,), (999,)]
        else:
            raise AssertionError("unexpected DB Game statement: " + sql)
        return self

    def fetchall(self):
        return list(self.rows)


class FakeGameConnection:
    def __init__(self):
        self.queries = []
        self.rolled_back = False
        self.closed = False

    def cursor(self):
        return FakeCursor(self)

    def rollback(self):
        self.rolled_back = True

    def close(self):
        self.closed = True


class FakeBillingConnection:
    def __init__(self):
        self.closed = False
        self.install_calls = 0

    def close(self):
        self.closed = True


class FakeRepository:
    def __init__(self, connection):
        self.connection = connection

    def install(self):
        self.connection.install_calls += 1
        return WALLET_SCOPE


class SyntheticInstaller:
    def __init__(self):
        self.source_files: set[str] = set()

    @staticmethod
    def _relative(path: Path, root: Path) -> str:
        return path.relative_to(root).as_posix()

    def __call__(self, install_script: Path, battle: Path, source: Path) -> str:
        self.source_files = {
            self._relative(path, source)
            for path in source.rglob("*")
            if path.is_file()
        }
        expected = {
            "winmm.dll",
            "sdgo/core.dll",
            "sdgo/sdgo.ini",
            f"sdgo/symbols/{HOST_KEY}.ini",
            "sdgo/mods/battle_rewards/battle_rewards.dll",
            "sdgo/mods/battle_rewards/mod.ini",
            "sdgo/mods/battle_rewards/config.example.ini",
        }
        if self.source_files != expected:
            raise AssertionError(
                f"installer source was not allowlisted: {self.source_files}"
            )

        loader = battle / "winmm.dll"
        chain = battle / "winmm_chain.dll"
        if loader.is_file() and loader.read_bytes() == b"foreign-loader":
            if chain.exists():
                raise RuntimeError("synthetic foreign chain collision")
            loader.replace(chain)

        old_mod = battle / "sdgo/mods/battle_rewards/mod.ini"
        preserved: dict[str, dict[str, str]] = {}
        if old_mod.is_file():
            parser = configparser.ConfigParser(interpolation=None)
            parser.optionxform = str
            parser.read(old_mod, encoding="utf-8-sig")
            for section in ("mod", "settings"):
                if parser.has_section(section):
                    preserved[section] = dict(parser[section])

        for source_file in source.rglob("*"):
            if not source_file.is_file():
                continue
            relative = source_file.relative_to(source)
            destination = battle / relative
            if (
                relative.as_posix() == "sdgo/sdgo.ini"
                and destination.is_file()
            ):
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source_file, destination)

        if preserved:
            parser = configparser.ConfigParser(interpolation=None)
            parser.optionxform = str
            parser.read(old_mod, encoding="utf-8-sig")
            if "enabled" in preserved.get("mod", {}):
                parser["mod"]["enabled"] = preserved["mod"]["enabled"]
            if "settings" in preserved:
                if not parser.has_section("settings"):
                    parser.add_section("settings")
                for key, value in preserved["settings"].items():
                    parser["settings"][key] = value
            with old_mod.open("w", encoding="utf-8", newline="\n") as stream:
                parser.write(stream, space_around_delimiters=False)
        return "synthetic scoped install complete"


class InstallFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(
            prefix="sdgo rewards 中文 space "
        )
        self.root = Path(self.temp.name)
        self.package = package(self.root / "分发 包")
        self.battle = installed_server(self.root)
        self.workspace = self.root / "腐竹 工作区"
        self.ini = server_ini(self.root / "owner 配置.ini")
        self.host_sha = sha256_file(self.battle / "BattleServer.exe")
        self.supported = {HOST_KEY: self.host_sha}

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def version_probe(_path):
        return VERSION

    def plan(self, **overrides):
        values = {
            "battle_dir": self.battle,
            "workspace": self.workspace,
            "package_root": self.package,
            "server_ini": self.ini,
            "supported_hosts": self.supported,
            "process_probe": lambda: [],
            "version_probe": self.version_probe,
        }
        values.update(overrides)
        return build_plan(**values)

    def apply(self, plan, **overrides):
        game = FakeGameConnection()
        billing = FakeBillingConnection()
        runner = SyntheticInstaller()
        values = {
            "process_probe": lambda: [],
            "version_probe": self.version_probe,
            "lock_probe": lambda _path: True,
            "game_connection_factory": lambda _path: game,
            "billing_connection_factory": lambda _path: billing,
            "repository_factory": FakeRepository,
            "server_runner": runner,
        }
        values.update(overrides)
        result = apply_plan(plan, **values)
        return result, game, billing, runner


class BattleRewardInstallTests(InstallFixture):
    def test_plan_is_read_only_and_reports_actionable_gates(self):
        before = {
            path.relative_to(self.battle).as_posix(): sha256_file(path)
            for path in self.battle.rglob("*")
            if path.is_file()
        }
        plan = self.plan()
        after = {
            path.relative_to(self.battle).as_posix(): sha256_file(path)
            for path in self.battle.rglob("*")
            if path.is_file()
        }
        self.assertEqual(before, after)
        self.assertFalse(self.workspace.exists())
        public = plan.public()
        self.assertEqual(public["mode"], "plan")
        self.assertFalse(public["writes_performed"])
        self.assertEqual(public["host"]["key"], HOST_KEY)
        self.assertEqual(public["package_version"], VERSION)
        self.assertIn(
            "sdgo.ini disables category gameplay; Battle Rewards will stay inactive",
            public["warnings"],
        )

    def test_full_framework_parent_selects_embedded_rewards_kit(self):
        full = self.root / "完整 框架包"
        shutil.copytree(self.package, full / "rewards")
        write(
            full / "server/sdgo/mods/battle_rewards/mod.ini",
            "[mod]\nid=battle_rewards\n",
        )
        plan = self.plan(package_root=full)
        self.assertEqual(plan.package_root, (full / "rewards").resolve())
        self.assertEqual(plan.package_version, VERSION)

    def test_full_apply_is_scoped_latest_and_read_back(self):
        plan = self.plan()
        result, game, billing, runner = self.apply(plan)

        self.assertEqual(result["status"], "complete")
        self.assertEqual(result["package_version"], VERSION)
        self.assertEqual(result["wallet_scope"], str(WALLET_SCOPE))
        self.assertEqual(result["boss_registry_pairs"], 2)
        self.assertTrue(game.rolled_back)
        self.assertTrue(game.closed)
        self.assertTrue(billing.closed)
        self.assertEqual(billing.install_calls, 1)
        self.assertTrue(all(
            sql.lstrip().upper().startswith("SELECT")
            for sql, _ in game.queries
        ))

        self.assertEqual(
            (self.battle / "winmm.dll").read_bytes(),
            (self.package / "server/winmm.dll").read_bytes(),
        )
        self.assertEqual(
            (self.battle / "sdgo/core.dll").read_bytes(),
            (self.package / "server/sdgo/core.dll").read_bytes(),
        )
        self.assertEqual(
            (
                self.battle
                / "sdgo/mods/battle_rewards/battle_rewards.dll"
            ).read_bytes(),
            (
                self.package
                / "server/sdgo/mods/battle_rewards/battle_rewards.dll"
            ).read_bytes(),
        )
        self.assertEqual(
            (self.battle / "winmm_chain.dll").read_bytes(),
            b"foreign-loader",
        )
        self.assertEqual(
            (
                self.battle / "sdgo/mods/other_mod/preserve.bin"
            ).read_bytes(),
            b"unrelated-mod-must-survive",
        )
        self.assertTrue(
            (
                self.battle
                / "sdgo/mods/battle_rewards/old-owner-note.txt"
            ).is_file()
        )
        sdgo_ini = (self.battle / "sdgo/sdgo.ini").read_text(
            encoding="utf-8"
        )
        self.assertIn("gameplay=0", sdgo_ini)
        self.assertIn("keep=yes", sdgo_ini)

        symbols = (
            self.battle / f"sdgo/symbols/{HOST_KEY}.ini"
        ).read_text(encoding="utf-8")
        self.assertIn("[unrelated.symbol]", symbols)
        self.assertIn("[battle.rewards.net_send]", symbols)
        self.assertIn("[battle.rewards.net_frame_proc]", symbols)

        config_path = (
            self.battle / "sdgo/mods/battle_rewards/config.ini"
        )
        raw = config_path.read_bytes()
        self.assertTrue(raw.startswith(b"\xff\xfe"))
        config = configparser.ConfigParser(interpolation=None, strict=True)
        config.read(config_path, encoding="utf-16")
        self.assertEqual(config["settings"]["OwnerNote"], "保留")
        self.assertEqual(config["settings"]["Trace"], "0")
        self.assertEqual(
            config["settings"]["RuntimeDirectory"],
            str((self.workspace / "runtime").resolve()),
        )
        self.assertRegex(
            config["settings"]["InstanceId"],
            r"^battle-[0-9a-f]{16}$",
        )

        mod = configparser.ConfigParser(interpolation=None, strict=True)
        mod.optionxform = str
        mod.read(
            self.battle / "sdgo/mods/battle_rewards/mod.ini",
            encoding="utf-8-sig",
        )
        self.assertEqual(mod["mod"]["enabled"], "1")
        self.assertEqual(mod["settings"]["OwnerSetting"], "keep-me")
        self.assertNotIn("sdgo/mods/other_mod/mod.ini", runner.source_files)

        receipt = Path(result["receipt"])
        saved = json.loads(
            (receipt / "receipt.json").read_text(encoding="utf-8")
        )
        self.assertEqual(saved["status"], "complete")
        self.assertEqual(
            (receipt / "backup/winmm.dll").read_bytes(),
            b"foreign-loader",
        )
        self.assertTrue(
            (self.workspace / "runtime/reward-policy.ini").is_file()
        )
        boss_registry = (
            self.workspace / "runtime/reward-bosses.txt"
        ).read_text(encoding="ascii")
        self.assertIn("101,201", boss_registry)
        self.assertIn("101,202", boss_registry)

    def test_symbol_conflict_is_rejected_before_writes(self):
        write(
            self.battle / f"sdgo/symbols/{HOST_KEY}.ini",
            """
[host]
name=BattleServer
[battle.rewards.net_send]
rva=0xDEADBEEF
""".lstrip(),
        )
        with self.assertRaisesRegex(DeploymentError, "conflicts"):
            self.plan()
        self.assertFalse(self.workspace.exists())

    def test_unknown_host_and_running_or_locked_target_block_apply(self):
        unsupported = self.plan(supported_hosts={"other": "0" * 64})
        self.assertTrue(unsupported.blockers)
        with self.assertRaises(DeploymentError):
            self.apply(unsupported)
        self.assertFalse(self.workspace.exists())

        plan = self.plan()
        with self.assertRaisesRegex(DeploymentError, "started after planning"):
            self.apply(
                plan,
                process_probe=lambda: [{
                    "pid": 77,
                    "path": str(self.battle / "BattleServer.exe"),
                }],
            )
        self.assertFalse(self.workspace.exists())

        with self.assertRaisesRegex(DeploymentError, "target files are in use"):
            self.apply(
                plan,
                lock_probe=lambda path:
                    path.name.casefold() != "battleserver.exe",
            )
        self.assertFalse(self.workspace.exists())

    def test_package_checksum_and_binary_version_mismatch_are_rejected(self):
        extra = write(
            self.package / "owner/rewards/unlisted_code.py",
            "raise RuntimeError('must never be trusted')\n",
        )
        with self.assertRaisesRegex(
            DeploymentError, "omits immutable package files"
        ):
            self.plan()
        extra.unlink()

        target = (
            self.package
            / "server/sdgo/mods/battle_rewards/battle_rewards.dll"
        )
        target.write_bytes(target.read_bytes() + b"tamper")
        with self.assertRaisesRegex(DeploymentError, "checksum mismatch"):
            self.plan()

        shutil.rmtree(self.package)
        package(self.package)

        def wrong_version(path):
            return "1.1.3" if path.name == "core.dll" else VERSION

        with self.assertRaisesRegex(DeploymentError, "versions disagree"):
            self.plan(version_probe=wrong_version)

    def test_apply_rejects_target_and_package_drift_without_receipt(self):
        plan = self.plan()
        write(
            self.battle / "sdgo/mods/other_mod/extra.txt",
            "untracked is allowed\n",
        )
        # Unrelated files are intentionally outside the reviewed write set.
        self.assertFalse(self.workspace.exists())
        write(self.battle / "sdgo/core.dll", b"changed-after-plan")
        with self.assertRaisesRegex(DeploymentError, "target files changed"):
            self.apply(plan)
        self.assertFalse(self.workspace.exists())

        write(self.battle / "sdgo/core.dll", b"old-core")
        plan = self.plan()
        script = self.package / "install_server.ps1"
        script.write_bytes(script.read_bytes() + b"# drift\n")
        checksums = json.loads(
            (self.package / "SHA256.json").read_text(encoding="utf-8")
        )
        checksums["install_server.ps1"] = sha256_file(script)
        write(
            self.package / "SHA256.json",
            json.dumps(checksums, indent=2, sort_keys=True) + "\n",
        )
        with self.assertRaisesRegex(DeploymentError, "package changed"):
            self.apply(plan)
        self.assertFalse(self.workspace.exists())

    def test_failed_apply_has_receipt_and_leaves_mod_disabled(self):
        config_path = (
            self.battle / "sdgo/mods/battle_rewards/config.ini"
        )
        write(
            config_path,
            "[settings]\nTrace=1\nTrace=2\nOwnerNote=preserve\n",
        )
        with self.assertRaisesRegex(DeploymentError, "duplicate key"):
            self.plan()
        self.assertFalse(self.workspace.exists())

        write(
            config_path,
            "[settings]\nTrace=1\nOwnerNote=preserve\n",
        )
        plan = self.plan()
        scoped = SyntheticInstaller()

        def corrupt_after_install(script, battle, source):
            output = scoped(script, battle, source)
            write(
                battle / "sdgo/mods/battle_rewards/config.ini",
                "[settings]\nTrace=1\nTrace=2\nOwnerNote=preserve\n",
            )
            return output

        with self.assertRaises(DeploymentApplyError) as caught:
            self.apply(plan, server_runner=corrupt_after_install)
        error = caught.exception
        self.assertTrue(error.writes_performed)
        saved = json.loads(
            (error.receipt / "receipt.json").read_text(encoding="utf-8")
        )
        self.assertEqual(saved["status"], "failed")
        self.assertEqual(saved["phase"], "runtime configuration")
        self.assertTrue(saved["target_touched"])
        mod = configparser.ConfigParser(interpolation=None, strict=True)
        mod.read(
            self.battle / "sdgo/mods/battle_rewards/mod.ini",
            encoding="utf-8-sig",
        )
        self.assertEqual(mod["mod"]["enabled"], "0")

    def test_uninstall_archives_only_mod_and_preserves_runtime_and_framework(self):
        runtime_job = write(
            self.workspace / "runtime/jobs/pending.job",
            "preserve wallet job\n",
        )
        ledger_marker = write(
            self.workspace / "ledger-installed.marker",
            "preserve ledger\n",
        )
        core_hash = sha256_file(self.battle / "sdgo/core.dll")
        symbol_hash = sha256_file(
            self.battle / f"sdgo/symbols/{HOST_KEY}.ini"
        )
        other_hash = sha256_file(
            self.battle / "sdgo/mods/other_mod/preserve.bin"
        )
        plan = build_plan(
            battle_dir=self.battle,
            workspace=self.workspace,
            uninstall=True,
            supported_hosts=self.supported,
            process_probe=lambda: [],
        )
        result = apply_plan(
            plan,
            process_probe=lambda: [],
            lock_probe=lambda _path: True,
        )
        self.assertEqual(result["status"], "complete")
        self.assertTrue(result["battle_rewards_moved"])
        self.assertFalse(
            (self.battle / "sdgo/mods/battle_rewards").exists()
        )
        self.assertEqual(
            sha256_file(self.battle / "sdgo/core.dll"), core_hash
        )
        self.assertEqual(
            sha256_file(self.battle / f"sdgo/symbols/{HOST_KEY}.ini"),
            symbol_hash,
        )
        self.assertEqual(
            sha256_file(
                self.battle / "sdgo/mods/other_mod/preserve.bin"
            ),
            other_hash,
        )
        self.assertTrue(runtime_job.is_file())
        self.assertTrue(ledger_marker.is_file())
        self.assertTrue(
            (
                Path(result["receipt"])
                / "disabled/battle_rewards/config.ini"
            ).is_file()
        )

    def test_manifest_host_and_unsafe_payload_are_rejected(self):
        mod = (
            self.package / "server/sdgo/mods/battle_rewards/mod.ini"
        )
        text = mod.read_text(encoding="utf-8")
        mod.write_text(
            text.replace(f"hosts={HOST_KEY}", "hosts=OtherHost"),
            encoding="utf-8",
        )
        checksums = json.loads(
            (self.package / "SHA256.json").read_text(encoding="utf-8")
        )
        key = "server/sdgo/mods/battle_rewards/mod.ini"
        checksums[key] = sha256_file(mod)
        write(
            self.package / "SHA256.json",
            json.dumps(checksums, indent=2, sort_keys=True) + "\n",
        )
        with self.assertRaisesRegex(DeploymentError, "does not support"):
            self.plan()

        shutil.rmtree(self.package)
        package(self.package)
        mod = (
            self.package / "server/sdgo/mods/battle_rewards/mod.ini"
        )
        mod.write_text(
            mod.read_text(encoding="utf-8").replace(
                "files=config.example.ini", "files=../outside.ini"
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(DeploymentError, "unsafe"):
            self.plan()


class BattleRewardHostParsingTests(unittest.TestCase):
    def test_synthetic_host_has_exact_timestamp_and_image_size(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = fake_pe(Path(temporary) / "BattleServer.exe")
            self.assertEqual(pe_host_key(path), HOST_KEY)


if __name__ == "__main__":
    unittest.main()
