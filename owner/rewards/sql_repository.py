"""Exactly-once MB delivery through SDGO's native StoreBilling procedure."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import uuid
from datetime import datetime, timezone
from . import progression


SCHEMA_PATH = Path(__file__).with_name("sql") / "battle_reward_wallet.sql"
MATCH_ID = re.compile(r"[-0-9A-Za-z_.:]{1,128}\Z")
PLAN_HASH = re.compile(r"[0-9a-fA-F]{64}\Z")
CURRENT_MB_SQL = """
DECLARE @NativeRetVal int = -1;
DECLARE @CurrentMb int = -1;
EXEC dbo.sp_PocketGC_IO
    @type = 1,
    @s_idx = ?,
    @s_GC = 0,
    @s_from = 0,
    @s_productID = 0,
    @retVal = @NativeRetVal OUTPUT,
    @retGC = @CurrentMb OUTPUT;
SELECT @NativeRetVal AS NativeRetVal, @CurrentMb AS CurrentMb;
"""


@dataclass(frozen=True)
class MbCreditReceipt:
    match_id: str
    account_number: int
    plan_hash: str
    mb_delta: int
    mb_after: int
    native_retval: int
    already_applied: bool


def split_batches(script: str) -> list[str]:
    """Split SQLCMD-style GO batches without accepting inline directives."""
    batches: list[str] = []
    current: list[str] = []
    for line in script.splitlines():
        if re.fullmatch(r"\s*GO\s*", line, flags=re.IGNORECASE):
            batch = "\n".join(current).strip()
            if batch:
                batches.append(batch)
            current.clear()
        else:
            current.append(line)
    batch = "\n".join(current).strip()
    if batch:
        batches.append(batch)
    return batches


def validate_request(
    match_id: str,
    account_number: int,
    plan_hash: str,
    mb: int,
) -> tuple[str, int, str, int]:
    if not isinstance(match_id, str) or MATCH_ID.fullmatch(match_id) is None:
        raise ValueError("match_id must use 1-128 canonical ASCII identifier characters")
    if isinstance(account_number, bool) or not isinstance(account_number, int) or account_number <= 0:
        raise ValueError("account_number must be a positive integer")
    if not isinstance(plan_hash, str) or PLAN_HASH.fullmatch(plan_hash) is None:
        raise ValueError("plan_hash must be 64 hexadecimal characters")
    if isinstance(mb, bool) or not isinstance(mb, int) or mb <= 0:
        raise ValueError("mb must be a positive integer")
    return match_id, account_number, plan_hash.lower(), mb


class RewardWalletRepository:
    """Call the installed same-database ledger; GB remains native-only."""

    def __init__(self, connection):
        self.connection = connection

    def install(self, schema_path: Path | None = None) -> uuid.UUID:
        path = schema_path or SCHEMA_PATH
        script = path.read_text(encoding="utf-8-sig")
        cursor = self.connection.cursor()
        try:
            for batch in split_batches(script):
                cursor.execute(batch)
            progress_script = SCHEMA_PATH.with_name("battle_reward_progression.sql").read_text(encoding="utf-8-sig")
            for batch in split_batches(progress_script):
                cursor.execute(batch)
            scope = self._read_scope(cursor)
            self.connection.commit()
            return scope
        except Exception:
            self.connection.rollback()
            raise

    def scope_id(self) -> uuid.UUID:
        cursor = self.connection.cursor()
        try:
            return self._read_scope(cursor)
        finally:
            self.connection.rollback()

    def apply_progression(self, job, policy) -> int:
        progression.validate(policy)
        cursor = self.connection.cursor()
        try:
            cursor.execute(
                "EXEC dbo.SdgoBattleProgressApply @MatchId=?,@AccountNumber=?,@ExpectedScope=?,"
                "@SourceHash=?,@PolicyRevision=?,@CompletedUtc=?,@Qualified=?,@FirstMb=?,"
                "@DailyTarget=?,@DailyMb=?,@WeeklyTarget=?,@WeeklyMb=?",
                job["match_id"], job["account_number"], str(job["expected_scope"]), job["source_hash"],
                job["policy_revision"], datetime.fromtimestamp(job["completed_unix"], timezone.utc).replace(tzinfo=None),
                progression.qualifying(job, policy), policy["daily_first_mb"], policy["daily_matches"],
                policy["daily_mb"], policy["weekly_matches"], policy["weekly_mb"])
            bonus = None
            while True:
                if cursor.description and [c[0] for c in cursor.description] == ["BonusMb"]:
                    rows = cursor.fetchall()
                    if len(rows) != 1 or type(rows[0][0]) is not int or not 0 <= rows[0][0] <= 300000:
                        raise ValueError("Invalid progression receipt")
                    bonus = rows[0][0]
                if not cursor.nextset():
                    break
            if bonus is None:
                raise ValueError("Missing progression receipt")
            self.connection.commit()
            return bonus
        except Exception:
            self.connection.rollback()
            raise

    def current_mb(self, account_number: int) -> int:
        """Read the current MB balance through the game's native procedure."""
        if (
            isinstance(account_number, bool)
            or not isinstance(account_number, int)
            or account_number <= 0
            or account_number > 2147483647
        ):
            raise ValueError("account_number must be a positive SQL int")

        cursor = self.connection.cursor()
        try:
            cursor.execute(CURRENT_MB_SQL, account_number)
            row = None
            while True:
                if cursor.description:
                    names = [item[0] for item in cursor.description]
                    rows = cursor.fetchall()
                    if names == ["NativeRetVal", "CurrentMb"]:
                        if len(rows) != 1:
                            raise RuntimeError(
                                "native MB read returned an invalid result count"
                            )
                        row = rows[0]
                if not cursor.nextset():
                    break
            if row is None:
                raise RuntimeError("native MB read returned no result")
            native_retval = int(row[0])
            current_mb = int(row[1])
            if native_retval != 0:
                raise RuntimeError("native MB read rejected the account")
            if current_mb < 0:
                raise RuntimeError("native MB read returned an invalid balance")
            # This operation is deliberately read-only even if a future native
            # procedure revision accidentally starts a transaction or writes.
            self.connection.rollback()
            return current_mb
        except Exception:
            self.connection.rollback()
            raise

    @staticmethod
    def _read_scope(cursor) -> uuid.UUID:
        row = cursor.execute(
            "SELECT ScopeId FROM dbo.SdgoBattleRewardWalletMeta "
            "WHERE Singleton=1 AND SchemaVersion=1 "
            "AND OBJECT_ID(N'dbo.SdgoBattleProgressApply',N'P') IS NOT NULL"
        ).fetchone()
        if row is None:
            raise RuntimeError("battle reward wallet schema is not installed")
        return uuid.UUID(str(row[0]))

    def apply_mb(
        self,
        *,
        match_id: str,
        account_number: int,
        plan_hash: str,
        mb: int,
        expected_scope: uuid.UUID | str,
    ) -> MbCreditReceipt:
        match_id, account_number, plan_hash, mb = validate_request(
            match_id, account_number, plan_hash, mb
        )
        try:
            scope = uuid.UUID(str(expected_scope))
        except (AttributeError, TypeError, ValueError) as error:
            raise ValueError("expected_scope must be a UUID") from error

        cursor = self.connection.cursor()
        try:
            cursor.execute(
                "EXEC dbo.SdgoBattleRewardApplyMb "
                "@MatchId=?,@AccountNumber=?,@ExpectedScope=?,@PlanHash=?,@Mb=?",
                match_id,
                account_number,
                str(scope),
                plan_hash,
                mb,
            )
            row = None
            columns = None
            while True:
                if cursor.description:
                    names = [item[0] for item in cursor.description]
                    rows = cursor.fetchall()
                    if names == [
                        "MatchId",
                        "AccountNumber",
                        "PlanHash",
                        "MbDelta",
                        "MbAfter",
                        "NativeRetVal",
                        "AlreadyApplied",
                    ]:
                        if len(rows) != 1:
                            raise RuntimeError("wallet procedure returned an invalid receipt count")
                        row = rows[0]
                        columns = names
                if not cursor.nextset():
                    break
            if row is None or columns is None:
                raise RuntimeError("wallet procedure returned no receipt")
            receipt = dict(zip(columns, row))
            result = MbCreditReceipt(
                match_id=str(receipt["MatchId"]),
                account_number=int(receipt["AccountNumber"]),
                plan_hash=str(receipt["PlanHash"]).lower(),
                mb_delta=int(receipt["MbDelta"]),
                mb_after=int(receipt["MbAfter"]),
                native_retval=int(receipt["NativeRetVal"]),
                already_applied=bool(receipt["AlreadyApplied"]),
            )
            if (
                result.match_id != match_id
                or result.account_number != account_number
                or result.plan_hash != plan_hash
                or result.mb_delta != mb
                or result.native_retval != 0
            ):
                raise RuntimeError("wallet procedure returned a mismatched receipt")
            self.connection.commit()
            return result
        except Exception:
            self.connection.rollback()
            raise
