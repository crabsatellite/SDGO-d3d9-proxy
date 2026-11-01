from __future__ import annotations

import configparser
import os
from pathlib import Path
import re
import sys
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from owner.rewards.sql_repository import (
    CURRENT_MB_SQL,
    PLAN_HASH,
    RewardWalletRepository,
    SCHEMA_PATH,
    split_batches,
    validate_request,
)


def quote_identifier(value: str) -> str:
    return "[" + value.replace("]", "]]") + "]"


class RewardRepositoryContractTests(unittest.TestCase):
    def test_schema_uses_native_gc_and_never_updates_wallet_directly(self):
        sql = SCHEMA_PATH.read_text(encoding="utf-8")
        self.assertIn("EXEC dbo.sp_PocketGC_IO", sql)
        self.assertNotRegex(sql, r"(?is)\bUPDATE\s+(?:dbo\.)?tbls_AccountPocket\b")
        self.assertIn("PRIMARY KEY (MatchId, AccountNumber)", sql)
        self.assertIn("Battle reward retry changed plan or MB amount", sql)
        self.assertNotIn("sp_PocketGP_IO", sql)
        self.assertIn("EXEC dbo.sp_PocketGC_IO", CURRENT_MB_SQL)
        self.assertRegex(CURRENT_MB_SQL, r"(?i)@type\s*=\s*1")
        self.assertNotIn("tbls_AccountPocket", CURRENT_MB_SQL)

    def test_sqlcmd_batches_are_stable(self):
        batches = split_batches("SELECT 1;\nGO\n\nselect 'go';\n go \nSELECT 2;")
        self.assertEqual(batches, ["SELECT 1;", "select 'go';", "SELECT 2;"])
        self.assertGreaterEqual(len(split_batches(SCHEMA_PATH.read_text(encoding="utf-8"))), 3)

    def test_request_validation_is_fail_closed(self):
        plan = "a" * 64
        self.assertEqual(validate_request("realm.2:match-9", 12, plan.upper(), 6),
                         ("realm.2:match-9", 12, plan, 6))
        for match in ("", "space is invalid", "x" * 129, "中文"):
            with self.assertRaises(ValueError):
                validate_request(match, 12, plan, 6)
        for account in (0, -1, True, "12"):
            with self.assertRaises(ValueError):
                validate_request("match-1", account, plan, 6)
        for digest in ("a" * 63, "g" * 64, None):
            with self.assertRaises(ValueError):
                validate_request("match-1", 12, digest, 6)
        for amount in (0, -1, True, 1.5):
            with self.assertRaises(ValueError):
                validate_request("match-1", 12, plan, amount)
        self.assertIsNotNone(PLAN_HASH.fullmatch(plan))


@unittest.skipUnless(
    os.environ.get("SDGO_REWARD_SQL_INTEGRATION") == "1",
    "set SDGO_REWARD_SQL_INTEGRATION=1 for the isolated SQL Server test",
)
class RewardRepositorySqlIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            import pyodbc
        except ImportError as error:
            raise unittest.SkipTest("pyodbc is unavailable") from error
        cls.pyodbc = pyodbc
        ini_path = Path(os.environ["SDGO_REWARD_SERVER_INI"])
        cfg = configparser.ConfigParser(interpolation=None)
        cfg.optionxform = str
        cfg.read(ini_path, encoding="utf-8-sig")
        section = cfg["DB StoreBilling"]
        cls.base = {
            "server": section["DBServerName"],
            "user": section["id"],
            "password": section["pwd"],
            "source_database": section["DBName"],
        }
        cls.database = "SDGO_RewardWalletTest_" + uuid.uuid4().hex[:12]
        cls.owner = uuid.uuid4().hex
        cls.admin = cls._connect("master", autocommit=True)
        source = cls._connect(cls.base["source_database"], autocommit=False)
        try:
            row = source.cursor().execute(
                "SELECT OBJECT_DEFINITION(OBJECT_ID(N'dbo.sp_PocketGC_IO'))"
            ).fetchone()
            if row is None or not row[0]:
                raise unittest.SkipTest("native sp_PocketGC_IO definition is unavailable")
            cls.native_definition = row[0]
            source.rollback()
        finally:
            source.close()
        cls.admin.cursor().execute("CREATE DATABASE " + quote_identifier(cls.database))
        cls.connection = cls._connect(cls.database, autocommit=False)
        try:
            cursor = cls.connection.cursor()
            cursor.execute(
                "CREATE TABLE dbo.tbls_AccountPocket("
                "f_idx int NOT NULL PRIMARY KEY,f_GC int NOT NULL,f_GP int NOT NULL)"
            )
            cursor.execute(
                "CREATE TABLE dbo.tbls_LogGCInOut("
                "f_no bigint IDENTITY(1,1) NOT NULL PRIMARY KEY,"
                "f_idx int NOT NULL,f_GC int NOT NULL,f_from smallint NOT NULL,"
                "f_productID int NOT NULL,f_retGC int NOT NULL,"
                "f_regdate datetime NOT NULL DEFAULT GETDATE())"
            )
            cursor.execute(cls.native_definition)
            cursor.execute(
                "EXEC sys.sp_addextendedproperty "
                "@name=N'sdgo_reward_wallet_test_owner',@value=?",
                cls.owner,
            )
            cursor.execute(
                "INSERT dbo.tbls_AccountPocket(f_idx,f_GC,f_GP) VALUES(900000001,1000,0)"
            )
            cls.connection.commit()
            cls.repository = RewardWalletRepository(cls.connection)
            cls.scope = cls.repository.install()
        except Exception:
            cls.connection.close()
            cls._drop_database()
            raise

    @classmethod
    def _connect(cls, database, *, autocommit):
        value = (
            "DRIVER={SQL Server};SERVER=%s;DATABASE=%s;UID=%s;PWD=%s;"
            % (cls.base["server"], database, cls.base["user"], cls.base["password"])
        )
        connection = cls.pyodbc.connect(value, timeout=15, autocommit=autocommit)
        connection.timeout = 15
        return connection

    @classmethod
    def _drop_database(cls):
        if not re.fullmatch(r"SDGO_RewardWalletTest_[0-9a-f]{12}", cls.database):
            raise AssertionError("refusing to remove an unexpected database")
        cls.admin.cursor().execute(
            "ALTER DATABASE " + quote_identifier(cls.database)
            + " SET SINGLE_USER WITH ROLLBACK IMMEDIATE"
        )
        cls.admin.cursor().execute("DROP DATABASE " + quote_identifier(cls.database))

    @classmethod
    def tearDownClass(cls):
        try:
            cls.connection.close()
            verify = cls._connect(cls.database, autocommit=False)
            try:
                row = verify.cursor().execute(
                    "SELECT CONVERT(varchar(64),value) FROM sys.extended_properties "
                    "WHERE class=0 AND name=N'sdgo_reward_wallet_test_owner'"
                ).fetchone()
                if row is None or row[0] != cls.owner:
                    raise AssertionError("scratch database ownership differs")
                verify.rollback()
            finally:
                verify.close()
            cls._drop_database()
        finally:
            cls.admin.close()

    def setUp(self):
        cursor = self.connection.cursor()
        cursor.execute("DELETE dbo.SdgoBattleRewardMbReceipt")
        cursor.execute("DELETE dbo.tbls_LogGCInOut")
        cursor.execute(
            "UPDATE dbo.tbls_AccountPocket SET f_GC=1000 WHERE f_idx=900000001"
        )
        self.connection.commit()

    def test_native_credit_retry_and_crash_after_commit(self):
        plan = "1" * 64
        first = self.repository.apply_mb(
            match_id="realm1:match-001",
            account_number=900000001,
            plan_hash=plan,
            mb=50,
            expected_scope=self.scope,
        )
        self.assertFalse(first.already_applied)
        self.assertEqual(first.mb_after, 1050)

        # Simulate a caller losing the response after commit. A new connection
        # retries the same identity and must only read the durable receipt.
        retry_connection = self._connect(self.database, autocommit=False)
        try:
            retry = RewardWalletRepository(retry_connection).apply_mb(
                match_id="realm1:match-001",
                account_number=900000001,
                plan_hash=plan,
                mb=50,
                expected_scope=self.scope,
            )
        finally:
            retry_connection.close()
        self.assertTrue(retry.already_applied)
        self.assertEqual(retry.mb_after, 1050)

        cursor = self.connection.cursor()
        self.assertEqual(cursor.execute(
            "SELECT f_GC FROM dbo.tbls_AccountPocket WHERE f_idx=900000001"
        ).fetchone()[0], 1050)
        self.assertEqual(cursor.execute(
            "SELECT COUNT(*) FROM dbo.tbls_LogGCInOut WHERE f_idx=900000001"
        ).fetchone()[0], 1)
        self.assertEqual(cursor.execute(
            "SELECT COUNT(*) FROM dbo.SdgoBattleRewardMbReceipt "
            "WHERE MatchId=N'realm1:match-001' AND AccountNumber=900000001"
        ).fetchone()[0], 1)
        self.connection.rollback()

    def test_current_mb_is_fresh_native_balance_not_historical_receipt(self):
        first = self.repository.apply_mb(
            match_id="realm1:match-current-1",
            account_number=900000001,
            plan_hash="5" * 64,
            mb=50,
            expected_scope=self.scope,
        )
        self.assertEqual(first.mb_after, 1050)

        # A later independent wallet change makes the first durable receipt's
        # MbAfter historical. Client notification must read the native current
        # balance instead of replaying that old absolute value.
        second = self.repository.apply_mb(
            match_id="realm1:match-current-2",
            account_number=900000001,
            plan_hash="6" * 64,
            mb=7,
            expected_scope=self.scope,
        )
        self.assertEqual(second.mb_after, 1057)
        self.assertEqual(self.repository.current_mb(900000001), 1057)

    def test_changed_retry_is_rejected_without_second_credit(self):
        plan = "2" * 64
        self.repository.apply_mb(
            match_id="realm1:match-002",
            account_number=900000001,
            plan_hash=plan,
            mb=25,
            expected_scope=self.scope,
        )
        for changed_plan, changed_mb in (("3" * 64, 25), (plan, 26)):
            with self.assertRaises(self.pyodbc.Error):
                self.repository.apply_mb(
                    match_id="realm1:match-002",
                    account_number=900000001,
                    plan_hash=changed_plan,
                    mb=changed_mb,
                    expected_scope=self.scope,
                )
        cursor = self.connection.cursor()
        self.assertEqual(cursor.execute(
            "SELECT f_GC FROM dbo.tbls_AccountPocket WHERE f_idx=900000001"
        ).fetchone()[0], 1025)
        self.assertEqual(cursor.execute(
            "SELECT COUNT(*) FROM dbo.tbls_LogGCInOut WHERE f_idx=900000001"
        ).fetchone()[0], 1)
        self.connection.rollback()

    def test_scope_and_missing_wallet_fail_without_receipt(self):
        with self.assertRaises(self.pyodbc.Error):
            self.repository.apply_mb(
                match_id="realm1:match-003",
                account_number=900000001,
                plan_hash="4" * 64,
                mb=10,
                expected_scope=uuid.uuid4(),
            )
        with self.assertRaises(self.pyodbc.Error):
            self.repository.apply_mb(
                match_id="realm1:match-004",
                account_number=900000002,
                plan_hash="4" * 64,
                mb=10,
                expected_scope=self.scope,
            )
        cursor = self.connection.cursor()
        self.assertEqual(cursor.execute(
            "SELECT COUNT(*) FROM dbo.SdgoBattleRewardMbReceipt"
        ).fetchone()[0], 0)
        self.assertEqual(cursor.execute(
            "SELECT COUNT(*) FROM dbo.tbls_LogGCInOut"
        ).fetchone()[0], 0)
        self.connection.rollback()


if __name__ == "__main__":
    unittest.main()
