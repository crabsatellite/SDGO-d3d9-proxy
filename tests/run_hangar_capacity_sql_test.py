"""Run isolated hangar-capacity SQL and actual-core/OLE DB fixtures.

The runner creates a fresh, un-published SQL Server container from an explicitly
named local image. It never reads WorldServer.ini or any existing SQL service.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import json
import os
import re
import secrets
import shutil
import subprocess
import threading
import time
import uuid
from pathlib import Path

try:
    from run_new_account_sql_test import Bridge, Handler
except ModuleNotFoundError:
    from tests.run_new_account_sql_test import Bridge, Handler


OWNER_LABEL = "sdgo.hangar_capacity.sql_test_owner"
SQLCMD = "/opt/mssql-tools18/bin/sqlcmd"
OUTPUT_LIMIT = 6000


class FixtureError(RuntimeError):
    pass


def _redact(text: str, private_values: tuple[str, ...] = ()) -> str:
    result = text
    for value in private_values:
        if value:
            result = result.replace(value, "<redacted>")
    result = re.sub(
        r"(?i)(password\s*=)\s*[^;\r\n]*",
        r"\1<redacted>",
        result,
    )
    if len(result) > OUTPUT_LIMIT:
        result = result[:OUTPUT_LIMIT] + "\n[output truncated]"
    return result


def _safe_name(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9_]+", value):
        raise FixtureError("internal SQL identifier was not fixture-safe")
    return value


class Harness:
    def __init__(
        self,
        repo: Path,
        artifacts: Path,
        image: str,
        work: Path,
        receipt: dict[str, object],
    ) -> None:
        self.repo = repo
        self.artifacts = artifacts
        self.image = image
        self.work = work
        self.receipt = receipt
        self.owner_token = uuid.uuid4().hex
        self.container_name = "sdgo-hangar-sql-" + str(uuid.uuid4())
        self.container_id: str | None = None
        self.password = "HcA1!" + secrets.token_hex(18)
        self.env_file = work / ("private-sql-env-" + uuid.uuid4().hex + ".env")
        self.database = _safe_name("HangarCapacity_" + uuid.uuid4().hex[:16])
        self.generated = work / "generated-sql"
        self.failures = work / "expected-sql-failures"
        self.generated.mkdir()
        self.failures.mkdir()
        self._emit_counter = 0
        self._emit_lock = threading.Lock()

    @property
    def private_values(self) -> tuple[str, ...]:
        return (self.password,)

    def docker(
        self,
        args: list[str],
        *,
        input_text: str | None = None,
        timeout: float = 30,
        check: bool = True,
        env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        try:
            result = subprocess.run(
                ["docker", *args],
                input=input_text,
                text=True,
                encoding="utf-8",
                errors="replace",
                capture_output=True,
                timeout=timeout,
                env=env,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise FixtureError(
                _redact("Docker fixture command could not complete: " + str(exc), self.private_values)
            ) from None
        if check and result.returncode:
            output = _redact(result.stdout + result.stderr, self.private_values)
            raise FixtureError(
                f"Docker fixture command failed ({result.returncode}): {output}"
            )
        return result

    def create_container(self) -> None:
        self.env_file.write_text(
            "ACCEPT_EULA=Y\n"
            "MSSQL_PID=Developer\n"
            f"MSSQL_SA_PASSWORD={self.password}\n",
            encoding="ascii",
        )
        with contextlib.suppress(OSError):
            os.chmod(self.env_file, 0o600)
        result = self.docker(
            [
                "run",
                "--detach",
                "--pull",
                "never",
                "--name",
                self.container_name,
                "--label",
                f"{OWNER_LABEL}={self.owner_token}",
                "--memory",
                "2g",
                "--cpus",
                "2",
                "--env-file",
                str(self.env_file),
                self.image,
            ],
            timeout=60,
        )
        lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
        if not lines or not re.fullmatch(r"[0-9a-f]{64}", lines[-1]):
            raise FixtureError("Docker did not return an exact container ID; fixture retained")
        self.container_id = lines[-1]
        self.assert_owned()
        self.receipt["container"] = {
            "name": self.container_name,
            "id": self.container_id,
            "unpublished": True,
            "memory": "2g",
            "cpus": "2",
        }

    def inspect_value(self, template: str) -> str:
        if not self.container_id:
            raise FixtureError("fixture container was not created")
        result = self.docker(
            ["inspect", "--format", template, self.container_id],
            timeout=15,
        )
        return result.stdout.strip()

    def assert_owned(self) -> None:
        if not self.container_id:
            raise FixtureError("fixture container was not created")
        actual_id = self.inspect_value("{{.Id}}")
        actual_label = self.inspect_value(
            '{{index .Config.Labels "' + OWNER_LABEL + '"}}'
        )
        if actual_id != self.container_id or actual_label != self.owner_token:
            raise FixtureError("fixture container ownership verification failed; container retained")

    def sql_command(self) -> list[str]:
        if not self.container_id:
            raise FixtureError("fixture container was not created")
        return [
            "docker",
            "exec",
            "-i",
            "-e",
            "SQLCMDPASSWORD",
            self.container_id,
            SQLCMD,
            "-S",
            "localhost",
            "-U",
            "sa",
            "-C",
            "-b",
            "-f65001",
            "-h-1",
            "-W",
        ]

    def sql(
        self,
        text: str,
        *,
        timeout: float = 30,
        check: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        environment = dict(os.environ, SQLCMDPASSWORD=self.password)
        try:
            result = subprocess.run(
                self.sql_command(),
                input=text,
                text=True,
                encoding="utf-8",
                errors="replace",
                capture_output=True,
                timeout=timeout,
                env=environment,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise FixtureError(
                _redact("Isolated SQL command could not complete: " + str(exc), self.private_values)
            ) from None
        if check and result.returncode:
            output = _redact(result.stdout + result.stderr, self.private_values)
            raise FixtureError(
                f"Isolated SQL fixture failed ({result.returncode}): {output}"
            )
        return result

    def wait_ready(self) -> None:
        if not self.container_id:
            raise FixtureError("fixture container was not created")
        deadline = time.monotonic() + 120
        last_output = ""
        while time.monotonic() < deadline:
            state = self.inspect_value("{{.State.Running}}")
            if state != "true":
                raise FixtureError("owned SQL fixture container stopped before becoming ready")
            try:
                result = self.sql(
                    "SET NOCOUNT ON; SELECT 1;\nGO\n",
                    timeout=8,
                    check=False,
                )
            except FixtureError as exc:
                last_output = str(exc)
                time.sleep(1)
                continue
            if result.returncode == 0 and any(
                line.strip() == "1" for line in result.stdout.splitlines()
            ):
                return
            last_output = result.stdout + result.stderr
            time.sleep(1)
        raise FixtureError(
            "Owned SQL fixture was not ready within 120 seconds: "
            + _redact(last_output, self.private_values)
        )

    def create_database(self) -> None:
        database = self.database
        self.sql(
            f"USE master;\n"
            f"IF DB_ID(N'{database}') IS NULL EXEC(N'CREATE DATABASE [{database}]');\n"
            "GO\n",
            timeout=60,
        )
        db_id = self.sql(
            f"SET NOCOUNT ON; SELECT DB_ID(N'{database}');\nGO\n"
        ).stdout.strip()
        if not db_id.isdigit() or int(db_id) <= 4:
            raise FixtureError("isolated fixture database did not receive a user database ID")
        self.receipt["database"] = database

    def reset_fixture(self) -> None:
        database = self.database
        self.sql(
            f"""USE [{database}];
DROP PROCEDURE IF EXISTS dbo.fixture_execute_policy;
DROP PROCEDURE IF EXISTS dbo.sp_GetCountUserUnits;
DROP TABLE IF EXISTS dbo.fixture_lock_control;
DROP TABLE IF EXISTS dbo.fixture_wallet;
DROP TABLE IF EXISTS dbo.tbl_UserUnits;
DROP TABLE IF EXISTS dbo.tbl_UserUnitSlotNumberList;
DROP TABLE IF EXISTS dbo.tbl_Users;
GO
CREATE TABLE dbo.tbl_Users(
    AccountNumber int NOT NULL PRIMARY KEY,
    ExpandSlotCount int NOT NULL
);
CREATE TABLE dbo.tbl_UserUnitSlotNumberList(
    UserUnitSlotNumber int NOT NULL PRIMARY KEY
);
CREATE TABLE dbo.tbl_UserUnits(
    AccountNumber int,
    UserUnitNumber bigint,
    UserUnitSlotNumber int,
    paint int
);
CREATE TABLE dbo.fixture_wallet(amount int NOT NULL);
INSERT dbo.tbl_Users(AccountNumber,ExpandSlotCount)
VALUES(1,420),(2,900),(3,1800),(4,0),(5,-1),(6,40000);
;WITH slots(UserUnitSlotNumber) AS (
    SELECT 1
    UNION ALL
    SELECT UserUnitSlotNumber+1 FROM slots WHERE UserUnitSlotNumber<1099
)
INSERT dbo.tbl_UserUnitSlotNumberList(UserUnitSlotNumber)
SELECT UserUnitSlotNumber FROM slots OPTION(MAXRECURSION 1100);
INSERT dbo.tbl_UserUnitSlotNumberList VALUES(1300);
INSERT dbo.tbl_UserUnits(AccountNumber,UserUnitNumber,UserUnitSlotNumber,paint)
VALUES(1,2147483658,10,77),(2,2147483660,11,88);
INSERT dbo.fixture_wallet VALUES(123);
GO
CREATE PROCEDURE dbo.sp_GetCountUserUnits
    @account int,
    @extra smallint OUTPUT,
    @ret int OUTPUT
AS
BEGIN
    SET NOCOUNT ON;
    SET @extra=NULL;
    SELECT @extra=TRY_CONVERT(smallint,ExpandSlotCount)
    FROM dbo.tbl_Users
    WHERE AccountNumber=@account;
    IF @@ROWCOUNT=1 AND @extra IS NOT NULL SET @ret=0;
    ELSE SET @ret=1;
    RETURN @ret;
END;
GO
""",
            timeout=60,
        )

    def emit(self, expected_database: str, account: int, lock_timeout_ms: int) -> str:
        with self._emit_lock:
            self._emit_counter += 1
            output = self.generated / f"policy-{self._emit_counter:03d}.sql"
        executable = self.artifacts / "hangar_capacity_sql_emit.exe"
        try:
            result = subprocess.run(
                [
                    str(executable),
                    expected_database,
                    str(account),
                    str(lock_timeout_ms),
                    str(output),
                ],
                text=True,
                encoding="utf-8",
                errors="replace",
                capture_output=True,
                timeout=30,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise FixtureError("SQL emitter could not complete: " + str(exc)) from None
        if result.returncode or not output.is_file():
            detail = _redact(result.stdout + result.stderr, self.private_values)
            raise FixtureError(
                f"SQL emitter failed ({result.returncode}): {detail}"
            )
        return output.read_text(encoding="utf-8")

    def run_policy(
        self,
        account: int,
        *,
        expected_database: str | None = None,
        lock_timeout_ms: int = 1000,
        check: bool = True,
        timeout: float = 30,
    ) -> subprocess.CompletedProcess[str]:
        expected = expected_database or self.database
        policy = self.emit(expected, account, lock_timeout_ms)
        return self.sql(
            f"USE [{self.database}];\n{policy}",
            check=check,
            timeout=timeout,
        )

    def expect_policy_failure(
        self,
        case_name: str,
        account: int,
        *,
        expected_database: str | None = None,
        lock_timeout_ms: int = 1000,
        timeout: float = 30,
    ) -> str:
        result = self.run_policy(
            account,
            expected_database=expected_database,
            lock_timeout_ms=lock_timeout_ms,
            check=False,
            timeout=timeout,
        )
        output = _redact(result.stdout + result.stderr, self.private_values)
        (self.failures / f"{case_name}.txt").write_text(
            output,
            encoding="utf-8",
        )
        if result.returncode == 0:
            raise FixtureError(f"{case_name} unexpectedly accepted invalid fixture state")
        return output

    def assert_database(self, statements: str) -> str:
        return self.sql(
            f"USE [{self.database}];\nSET NOCOUNT ON;\n{statements}\n",
            timeout=30,
        ).stdout.strip()

    def install_policy_procedure(self, account: int) -> None:
        policy = self.emit(self.database, account, 1000)
        self.sql(
            f"USE [{self.database}];\nGO\n"
            "CREATE OR ALTER PROCEDURE dbo.fixture_execute_policy AS\n"
            "BEGIN\n"
            f"{policy}\n"
            "END;\n"
            "GO\n",
            timeout=30,
        )

    def start_lock_holder(self) -> tuple[subprocess.Popen[str], threading.Event, list[str]]:
        environment = dict(os.environ, SQLCMDPASSWORD=self.password)
        process = subprocess.Popen(
            self.sql_command(),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            env=environment,
        )
        script = f"""USE [{self.database}];
SET NOCOUNT ON;
DECLARE @result int;
EXEC @result=sys.sp_getapplock
    @Resource=N'sdgo.hangar_capacity.ensure.v1',
    @LockMode=N'Exclusive',
    @LockOwner=N'Session',
    @LockTimeout=0,
    @DbPrincipal=N'public';
IF @result<0 THROW 51000,'fixture lock holder failed',1;
UPDATE dbo.fixture_lock_control SET holder_session_id=@@SPID;
RAISERROR(N'HANGAR_LOCK_READY',10,1) WITH NOWAIT;
WHILE (SELECT release_requested FROM dbo.fixture_lock_control)=0
    WAITFOR DELAY '00:00:00.100';
EXEC sys.sp_releaseapplock
    @Resource=N'sdgo.hangar_capacity.ensure.v1',
    @LockOwner=N'Session',
    @DbPrincipal=N'public';
GO
"""
        assert process.stdin is not None
        process.stdin.write(script)
        process.stdin.close()
        process.stdin = None
        ready = threading.Event()
        lines: list[str] = []

        def receive() -> None:
            assert process.stdout is not None
            for line in process.stdout:
                lines.append(line)
                if "HANGAR_LOCK_READY" in line:
                    ready.set()

        threading.Thread(target=receive, daemon=True).start()
        return process, ready, lines

    def run_native_case(self, mode: str) -> dict[str, object]:
        enabled = mode == "enabled"
        self.reset_fixture()
        host = self.work / f"native-{mode}-{uuid.uuid4().hex[:8]}"
        mod = host / "sdgo" / "mods" / "hangar_capacity"
        symbols = host / "sdgo" / "symbols"
        mod.mkdir(parents=True)
        symbols.mkdir()
        shutil.copy2(self.artifacts / "hangar_capacity_sql_test.exe", host)
        shutil.copy2(self.artifacts / "core.dll", host / "sdgo" / "core.dll")
        shutil.copy2(
            self.artifacts / "hangar_capacity.dll",
            mod / "hangar_capacity.dll",
        )
        manifest = (self.repo / "mods" / "hangar_capacity" / "mod.ini").read_text(
            encoding="utf-8-sig"
        )
        manifest, hosts_changed = re.subn(
            r"(?m)^hosts=.*$",
            "hosts=*",
            manifest,
        )
        manifest, enabled_changed = re.subn(
            r"(?m)^enabled=.*$",
            "enabled=" + ("1" if enabled else "0"),
            manifest,
        )
        if hosts_changed != 1 or enabled_changed != 1:
            raise FixtureError("hangar_capacity mod.ini no longer has the expected controls")
        (mod / "mod.ini").write_text(manifest, encoding="ascii")
        (host / "sdgo" / "sdgo.ini").write_text(
            "[core]\nlog_level=0\n[mods]\nhangar_capacity="
            + ("1" if enabled else "0")
            + "\n",
            encoding="ascii",
        )
        executable = host / "hangar_capacity_sql_test.exe"
        executable.with_suffix(".ini").write_text(
            f"[DB Game]\nDBName={self.database}\n",
            encoding="ascii",
        )
        bridge = Bridge(("127.0.0.1", 0), Handler)
        bridge.container = self.container_id
        thread = threading.Thread(target=bridge.serve_forever, daemon=True)
        thread.start()
        try:
            port = bridge.server_address[1]
            environment = dict(os.environ)
            environment["CommonProgramFiles"] = environment.get(
                "CommonProgramFiles(x86)",
                r"C:\Program Files (x86)\Common Files",
            )
            environment["SDGO_HANGAR_TEST_CONNECTION"] = (
                "Provider=SQLOLEDB;"
                f"Data Source=127.0.0.1,{port};"
                f"Initial Catalog={self.database};"
                "User ID=sa;"
                f"Password={self.password};"
            )
            powershell = (
                Path(os.environ.get("WINDIR", r"C:\Windows"))
                / "SysWOW64"
                / "WindowsPowerShell"
                / "v1.0"
                / "powershell.exe"
            )
            preflight = subprocess.run(
                [
                    str(powershell),
                    "-NoLogo",
                    "-NoProfile",
                    "-NonInteractive",
                    "-Command",
                    "$ErrorActionPreference='Stop';"
                    "$c=New-Object System.Data.OleDb.OleDbConnection("
                    "$env:SDGO_HANGAR_TEST_CONNECTION);"
                    "try{$c.Open();[Console]::Out.WriteLine('OLEDB BRIDGE READY')}"
                    "catch{[Console]::Error.WriteLine($_.Exception.ToString());exit 1}"
                    "finally{if($c.State-ne'Closed'){$c.Close()};$c.Dispose()}",
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=45,
                env=environment,
            )
            preflight_output = _redact(
                preflight.stdout + preflight.stderr,
                self.private_values,
            )
            (host / "oledb-preflight.txt").write_text(
                preflight_output,
                encoding="utf-8",
            )
            if preflight.returncode or "OLEDB BRIDGE READY" not in preflight.stdout:
                raise FixtureError(
                    f"32-bit SQLOLEDB bridge preflight failed ({preflight.returncode}): "
                    + preflight_output
                )
            result = subprocess.run(
                [str(executable), mode],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=120,
                env=environment,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise FixtureError(f"native {mode} fixture could not complete: {exc}") from None
        finally:
            bridge.shutdown()
            bridge.server_close()
            thread.join(timeout=5)
        output = _redact(result.stdout + result.stderr, self.private_values)
        (host / "stdout.txt").write_text(output, encoding="utf-8")
        if result.returncode != 0 or "ALL HANGAR SQL CHECKS PASSED" not in result.stdout:
            raise FixtureError(
                f"native {mode} fixture failed ({result.returncode}): {output}"
            )
        if enabled:
            checks = """
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>1176 THROW 51000,'native target account mismatch',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=2)<>900 THROW 51000,'native non-target account changed',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=3)<>1800 THROW 51000,'native larger account shrank',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=4)<>1176 THROW 51000,'native new account mismatch',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1200 THROW 51000,'native slot pool mismatch',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber IN(1099,1100,1200))<>3 THROW 51000,'native allocatable edge slots missing',1;
IF EXISTS(SELECT 1 FROM dbo.tbl_UserUnits WHERE UserUnitSlotNumber IN(1099,1100,1200)) THROW 51000,'native edge slots are already occupied',1;
IF EXISTS(SELECT 1 FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber=1201) THROW 51000,'native first policy generated slot 1201',1;
"""
        else:
            checks = """
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>420 THROW 51000,'disabled account changed',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1099)<>1099 THROW 51000,'disabled base pool changed',1;
IF EXISTS(SELECT 1 FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1100 AND 1200) THROW 51000,'disabled policy extended slots',1;
"""
        checks += """
IF NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber=1300) THROW 51000,'outside slot was removed',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnits)<>2 THROW 51000,'native unit count changed',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE UserUnitNumber=2147483658 AND paint=77)<>1 THROW 51000,'native 64-bit unit or paint changed',1;
IF (SELECT amount FROM dbo.fixture_wallet)<>123 THROW 51000,'native wallet changed',1;
SELECT AccountNumber,ExpandSlotCount FROM dbo.tbl_Users ORDER BY AccountNumber;
SELECT UserUnitSlotNumber FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber IN(1099,1100,1200,1201,1300) ORDER BY UserUnitSlotNumber;
"""
        readback = self.assert_database(checks)
        (host / "sql-readback.txt").write_text(readback + "\n", encoding="utf-8")
        return {
            "name": "native_" + mode,
            "passed": True,
            "preflight": str((host / "oledb-preflight.txt").relative_to(self.work)),
            "stdout": str((host / "stdout.txt").relative_to(self.work)),
            "readback": str((host / "sql-readback.txt").relative_to(self.work)),
        }

    def cleanup(self) -> dict[str, object]:
        result: dict[str, object] = {"env_file_deleted": False}
        with contextlib.suppress(OSError):
            self.env_file.unlink()
        result["env_file_deleted"] = not self.env_file.exists()
        if not self.container_id:
            result.update(container_removed=False, reason="no returned container ID")
            return result
        try:
            self.assert_owned()
        except Exception as exc:
            result.update(
                container_removed=False,
                retained=True,
                reason=_redact(str(exc), self.private_values),
            )
            return result
        try:
            removal = self.docker(
                ["rm", "-f", self.container_id],
                timeout=30,
                check=False,
            )
        except Exception as exc:
            result.update(
                container_removed=False,
                retained=True,
                reason=_redact(str(exc), self.private_values),
            )
            return result
        if removal.returncode:
            result.update(
                container_removed=False,
                retained=True,
                reason=_redact(removal.stdout + removal.stderr, self.private_values),
            )
        else:
            result.update(container_removed=True, retained=False)
        return result


def _case(
    receipt: dict[str, object],
    name: str,
    action,
) -> None:
    started = time.monotonic()
    try:
        action()
    except Exception:
        receipt["cases"].append(
            {"name": name, "passed": False, "seconds": round(time.monotonic() - started, 3)}
        )
        raise
    receipt["cases"].append(
        {"name": name, "passed": True, "seconds": round(time.monotonic() - started, 3)}
    )


def run(build_dir: Path, config: str, work_root: Path, image: str) -> Path:
    if not build_dir.is_absolute() or not work_root.is_absolute():
        raise FixtureError("--build-dir and --work-root must be absolute paths")
    if not image or any(character.isspace() for character in image):
        raise FixtureError("--image must be one explicit cached image reference")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", config):
        raise FixtureError("--config contains unsupported characters")
    repo = Path(__file__).resolve().parents[1]
    artifacts = build_dir / config
    required = (
        "hangar_capacity_sql_emit.exe",
        "hangar_capacity_sql_test.exe",
        "core.dll",
        "hangar_capacity.dll",
    )
    missing = [name for name in required if not (artifacts / name).is_file()]
    if missing:
        raise FixtureError(
            "missing Release fixture artifacts: " + ", ".join(missing)
        )
    work_root.mkdir(parents=True, exist_ok=True)
    work = work_root / ("hangar-capacity-sql-" + str(uuid.uuid4()))
    work.mkdir(exist_ok=False)
    receipt: dict[str, object] = {
        "status": "running",
        "work": str(work),
        "build_dir": str(build_dir),
        "config": config,
        "image": image,
        "cases": [],
        "boundaries": {
            "fresh_owned_container": True,
            "existing_containers_accessed": False,
            "published_ports": False,
            "worldserver_ini_read": False,
            "game_executed": False,
        },
    }
    harness = Harness(repo, artifacts, image, work, receipt)
    failure: Exception | None = None
    try:
        harness.create_container()
        harness.wait_ready()
        harness.create_database()

        def idempotent_targets() -> None:
            harness.reset_fixture()
            harness.run_policy(1)
            harness.run_policy(1)
            harness.run_policy(4)
            harness.run_policy(3)
            harness.assert_database(
                """
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>1176 THROW 51000,'idempotent target mismatch',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=4)<>1176 THROW 51000,'new account target mismatch',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=3)<>1800 THROW 51000,'larger saved target shrank',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=2)<>900 THROW 51000,'other account changed',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1200 THROW 51000,'slot fill mismatch',1;
IF NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber=1300) THROW 51000,'outside slot removed',1;
"""
            )

        _case(receipt, "idempotency_new_old_larger", idempotent_targets)

        started = time.monotonic()
        native_enabled = harness.run_native_case("enabled")
        native_enabled["seconds"] = round(time.monotonic() - started, 3)
        receipt["cases"].append(native_enabled)
        started = time.monotonic()
        native_disabled = harness.run_native_case("disabled")
        native_disabled["seconds"] = round(time.monotonic() - started, 3)
        receipt["cases"].append(native_disabled)

        def holes_and_outside() -> None:
            harness.reset_fixture()
            harness.assert_database(
                "DELETE dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber IN(17,509,1099);"
            )
            harness.run_policy(1)
            harness.assert_database(
                """
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1200 THROW 51000,'holes were not filled',1;
IF NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber=1300) THROW 51000,'outside slot removed while filling holes',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=2)<>900 THROW 51000,'non-target account changed while filling holes',1;
"""
            )

        _case(receipt, "holes_fill_and_outside_retention", holes_and_outside)

        def invalid_raw_values() -> None:
            for name, account in (("negative", 5), ("over_smallint", 6)):
                harness.reset_fixture()
                harness.expect_policy_failure(name, account)
                harness.assert_database(
                    """
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1099 THROW 51000,'invalid raw inserted slots',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>420 THROW 51000,'invalid raw changed target',1;
"""
                )
            harness.reset_fixture()
            harness.assert_database(
                "ALTER TABLE dbo.tbl_Users ALTER COLUMN ExpandSlotCount int NULL;"
                "UPDATE dbo.tbl_Users SET ExpandSlotCount=NULL WHERE AccountNumber=5;"
            )
            harness.expect_policy_failure("null", 5)
            harness.assert_database(
                """
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1099 THROW 51000,'null raw inserted slots',1;
IF EXISTS(SELECT 1 FROM dbo.tbl_Users WHERE AccountNumber=5 AND ExpandSlotCount IS NOT NULL) THROW 51000,'null raw was changed',1;
"""
            )

        _case(receipt, "invalid_raw_null_negative_large", invalid_raw_values)

        def missing_and_database_identity() -> None:
            for name, account, expected in (
                ("missing_account", 999, harness.database),
                ("mismatched_database", 1, "master"),
                ("nonexistent_database", 1, "HangarCapacityMissing_" + uuid.uuid4().hex[:8]),
            ):
                harness.reset_fixture()
                harness.expect_policy_failure(
                    name,
                    account,
                    expected_database=expected,
                )
                harness.assert_database(
                    """
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>420 THROW 51000,'identity failure changed account',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1099 THROW 51000,'identity failure inserted slots',1;
"""
                )

        _case(receipt, "missing_account_and_database_identity", missing_and_database_identity)

        def changed_schema_and_getter() -> None:
            harness.reset_fixture()
            harness.assert_database(
                """
DROP TABLE dbo.tbl_UserUnitSlotNumberList;
CREATE TABLE dbo.tbl_UserUnitSlotNumberList(UserUnitSlotNumber bigint NOT NULL PRIMARY KEY);
;WITH slots(UserUnitSlotNumber) AS (SELECT 1 UNION ALL SELECT UserUnitSlotNumber+1 FROM slots WHERE UserUnitSlotNumber<1099)
INSERT dbo.tbl_UserUnitSlotNumberList SELECT UserUnitSlotNumber FROM slots OPTION(MAXRECURSION 1100);
INSERT dbo.tbl_UserUnitSlotNumberList VALUES(1300);
"""
            )
            harness.expect_policy_failure("changed_schema", 1)
            harness.assert_database(
                "IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>420 THROW 51000,'changed schema wrote account',1;"
            )
            harness.reset_fixture()
            harness.assert_database(
                """
DROP TABLE dbo.tbl_Users;
CREATE TABLE dbo.tbl_Users(
    AccountNumber int NOT NULL PRIMARY KEY,
    ExpandSlotCount int IDENTITY(1,1) NOT NULL
);
SET IDENTITY_INSERT dbo.tbl_Users ON;
INSERT dbo.tbl_Users(AccountNumber,ExpandSlotCount)
VALUES(1,420),(2,900),(3,1800),(4,0),(5,-1),(6,40000);
SET IDENTITY_INSERT dbo.tbl_Users OFF;
"""
            )
            harness.expect_policy_failure("identity_expand_schema", 1)
            harness.assert_database(
                """
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>420 THROW 51000,'identity expand schema wrote account',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList)<>1100 THROW 51000,'identity expand schema changed slots',1;
"""
            )
            harness.reset_fixture()
            harness.assert_database(
                """
DROP TABLE dbo.tbl_UserUnitSlotNumberList;
CREATE TABLE dbo.tbl_UserUnitSlotNumberList(
    UserUnitSlotNumber int IDENTITY(1,1) NOT NULL PRIMARY KEY
);
SET IDENTITY_INSERT dbo.tbl_UserUnitSlotNumberList ON;
;WITH slots(UserUnitSlotNumber) AS (
    SELECT 1
    UNION ALL
    SELECT UserUnitSlotNumber+1 FROM slots WHERE UserUnitSlotNumber<1099
)
INSERT dbo.tbl_UserUnitSlotNumberList(UserUnitSlotNumber)
SELECT UserUnitSlotNumber FROM slots OPTION(MAXRECURSION 1100);
INSERT dbo.tbl_UserUnitSlotNumberList(UserUnitSlotNumber) VALUES(1300);
SET IDENTITY_INSERT dbo.tbl_UserUnitSlotNumberList OFF;
"""
            )
            harness.expect_policy_failure("identity_slot_schema", 1)
            harness.assert_database(
                """
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>420 THROW 51000,'identity schema wrote account',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList)<>1100 THROW 51000,'identity schema changed slots',1;
"""
            )
            harness.reset_fixture()
            harness.assert_database(
                """
DROP PROCEDURE dbo.sp_GetCountUserUnits;
EXEC(N'CREATE PROCEDURE dbo.sp_GetCountUserUnits @account int,@extra int OUTPUT,@ret int OUTPUT AS SET @ret=0');
"""
            )
            harness.expect_policy_failure("changed_getter", 1)
            harness.assert_database(
                """
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>420 THROW 51000,'changed getter wrote account',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1099 THROW 51000,'changed getter inserted slots',1;
"""
            )

        _case(receipt, "changed_schema_and_getter_rejected", changed_schema_and_getter)

        def constraints_and_foreign_trigger() -> None:
            harness.reset_fixture()
            harness.assert_database(
                """
ALTER TABLE dbo.tbl_UserUnitSlotNumberList
ADD CONSTRAINT fixture_fail_slots CHECK(UserUnitSlotNumber<>1180);
"""
            )
            harness.expect_policy_failure("insert_constraint_rollback", 1)
            harness.assert_database(
                """
IF @@TRANCOUNT<>0 THROW 51000,'insert constraint leaked transaction',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>420 THROW 51000,'insert constraint changed account',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1099 THROW 51000,'insert constraint left slots',1;
"""
            )
            harness.reset_fixture()
            harness.assert_database(
                """
ALTER TABLE dbo.tbl_Users
ADD CONSTRAINT fixture_fail_users
CHECK(AccountNumber<>1 OR ExpandSlotCount<1000);
"""
            )
            harness.expect_policy_failure("update_constraint_rollback", 1)
            harness.assert_database(
                """
IF @@TRANCOUNT<>0 THROW 51000,'update constraint leaked transaction',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>420 THROW 51000,'update constraint changed account',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1099 THROW 51000,'update constraint left slots',1;
"""
            )
            harness.reset_fixture()
            harness.sql(
                f"""USE [{harness.database}];
GO
CREATE TRIGGER dbo.fixture_foreign_slots ON dbo.tbl_UserUnitSlotNumberList AFTER INSERT
AS RETURN;
GO
"""
            )
            harness.expect_policy_failure("foreign_trigger_rejected", 1)
            harness.assert_database(
                """
IF @@TRANCOUNT<>0 THROW 51000,'foreign trigger rejection leaked transaction',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>420 THROW 51000,'foreign trigger rejection changed account',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1099 THROW 51000,'foreign trigger rejection left slots',1;
"""
            )

        _case(
            receipt,
            "constraint_rollbacks_and_foreign_trigger_rejection",
            constraints_and_foreign_trigger,
        )

        def ambient_and_options() -> None:
            harness.reset_fixture()
            harness.install_policy_procedure(1)
            harness.assert_database(
                """
BEGIN TRANSACTION;
UPDATE dbo.fixture_wallet SET amount=amount+1;
BEGIN TRY
    EXEC dbo.fixture_execute_policy;
    THROW 51000,'ambient policy unexpectedly succeeded',1;
END TRY
BEGIN CATCH
    IF ERROR_MESSAGE() NOT LIKE N'%requires no ambient transaction%' THROW;
END CATCH;
IF @@TRANCOUNT<>1 THROW 51000,'ambient transaction ownership changed',1;
IF (SELECT amount FROM dbo.fixture_wallet)<>124 THROW 51000,'ambient caller work changed',1;
ROLLBACK TRANSACTION;
IF (SELECT amount FROM dbo.fixture_wallet)<>123 THROW 51000,'ambient rollback mismatch',1;

SET NOCOUNT OFF;
SET XACT_ABORT OFF;
DECLARE @options_off int=@@OPTIONS;
EXEC dbo.fixture_execute_policy;
IF @@OPTIONS<>@options_off OR @@TRANCOUNT<>0 THROW 51000,'off session options were not preserved',1;

SET NOCOUNT ON;
SET XACT_ABORT ON;
DECLARE @options_on int=@@OPTIONS;
EXEC dbo.fixture_execute_policy;
IF @@OPTIONS<>@options_on OR @@TRANCOUNT<>0 THROW 51000,'on session options were not preserved',1;
SET NOCOUNT OFF;
SET XACT_ABORT OFF;

SET IMPLICIT_TRANSACTIONS ON;
DECLARE @options_implicit int=@@OPTIONS;
BEGIN TRY
    EXEC dbo.fixture_execute_policy;
    THROW 51000,'implicit policy unexpectedly succeeded',1;
END TRY
BEGIN CATCH
    IF ERROR_MESSAGE() NOT LIKE N'%does not support IMPLICIT_TRANSACTIONS%' THROW;
END CATCH;
IF (@@OPTIONS & 2)=0 OR @options_implicit<>@@OPTIONS THROW 51000,'implicit option changed',1;
IF @@TRANCOUNT<>0 THROW 51000,'implicit rejection created a transaction',1;
SET IMPLICIT_TRANSACTIONS OFF;
"""
            )

        _case(receipt, "ambient_transaction_and_session_options", ambient_and_options)

        def application_lock() -> None:
            harness.reset_fixture()
            harness.assert_database(
                """
CREATE TABLE dbo.fixture_lock_control(
    release_requested bit NOT NULL,
    holder_session_id int NULL
);
INSERT dbo.fixture_lock_control VALUES(0,NULL);
"""
            )
            holder, _ready, lines = harness.start_lock_holder()
            try:
                ready_deadline = time.monotonic() + 45
                lock_ready = False
                last_probe = ""
                while time.monotonic() < ready_deadline:
                    if holder.poll() is not None:
                        raise FixtureError(
                            "fixture application-lock holder exited before ready "
                            f"({holder.returncode}): "
                            + _redact("".join(lines), harness.private_values)
                        )
                    try:
                        probe = harness.sql(
                            f"""USE [{harness.database}];
SET NOCOUNT ON;
SELECT CASE WHEN EXISTS(
    SELECT 1
    FROM sys.dm_tran_locks AS locks
    CROSS JOIN dbo.fixture_lock_control AS control
    WHERE locks.request_session_id=control.holder_session_id
      AND locks.resource_database_id=DB_ID()
      AND locks.resource_type=N'APPLICATION'
      AND locks.request_status=N'GRANT'
      AND locks.request_mode=N'X'
) THEN 1 ELSE 0 END;
GO
""",
                            timeout=8,
                            check=False,
                        )
                        last_probe = probe.stdout + probe.stderr
                        if probe.returncode == 0 and any(
                            line.strip() == "1"
                            for line in probe.stdout.splitlines()
                        ):
                            lock_ready = True
                            break
                    except FixtureError as exc:
                        last_probe = str(exc)
                    time.sleep(0.25)
                if not lock_ready:
                    raise FixtureError(
                        "fixture application-lock holder was not verified within 45 seconds: "
                        + _redact(
                            "".join(lines) + "\n" + last_probe,
                            harness.private_values,
                        )
                    )
                started = time.monotonic()
                harness.expect_policy_failure(
                    "application_lock_timeout",
                    1,
                    lock_timeout_ms=500,
                    timeout=8,
                )
                elapsed = time.monotonic() - started
                if elapsed > 5:
                    raise FixtureError(
                        f"application-lock rejection was not bounded ({elapsed:.3f}s)"
                    )
                harness.assert_database(
                    """
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>420 THROW 51000,'lock failure changed account',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1099 THROW 51000,'lock failure inserted slots',1;
UPDATE dbo.fixture_lock_control SET release_requested=1;
"""
                )
                try:
                    holder.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    holder.kill()
                    raise FixtureError("fixture application-lock holder did not exit") from None
                if holder.returncode:
                    raise FixtureError(
                        "fixture application-lock holder failed: "
                        + _redact("".join(lines), harness.private_values)
                    )
                harness.run_policy(1)
                harness.assert_database(
                    "IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>1176 THROW 51000,'post-lock retry failed',1;"
                )
            finally:
                if holder.poll() is None:
                    with contextlib.suppress(Exception):
                        harness.assert_database(
                            "UPDATE dbo.fixture_lock_control SET release_requested=1;"
                        )
                    with contextlib.suppress(Exception):
                        holder.wait(timeout=5)
                if holder.poll() is None:
                    with contextlib.suppress(Exception):
                        holder.kill()
                        holder.wait(timeout=3)

        _case(receipt, "application_lock_contention_and_retry", application_lock)

        def concurrent_calls() -> None:
            harness.reset_fixture()
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                futures = [executor.submit(harness.run_policy, 1) for _ in range(2)]
                for future in futures:
                    future.result(timeout=45)
            harness.assert_database(
                """
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>1176 THROW 51000,'same-account concurrency mismatch',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1200 THROW 51000,'same-account slot mismatch',1;
"""
            )
            harness.reset_fixture()
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                futures = [
                    executor.submit(harness.run_policy, account)
                    for account in (1, 4)
                ]
                for future in futures:
                    future.result(timeout=45)
            harness.assert_database(
                """
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)<>1176 THROW 51000,'different-account first mismatch',1;
IF (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=4)<>1176 THROW 51000,'different-account second mismatch',1;
IF (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)<>1200 THROW 51000,'different-account slot mismatch',1;
"""
            )

        _case(receipt, "concurrent_same_and_different_accounts", concurrent_calls)

        receipt["status"] = "passed"
    except Exception as exc:
        failure = exc
        receipt["status"] = "failed"
        receipt["error"] = _redact(str(exc), harness.private_values)
    finally:
        cleanup = harness.cleanup()
        receipt["cleanup"] = cleanup
        if not cleanup.get("env_file_deleted"):
            receipt["status"] = "failed"
            receipt["error"] = "private SQL environment file could not be deleted"
        if harness.container_id and not cleanup.get("container_removed"):
            receipt["status"] = "failed"
            receipt["error"] = str(cleanup.get("reason", "owned container was retained"))
        (work / "result.json").write_text(
            json.dumps(receipt, indent=2) + "\n",
            encoding="utf-8",
        )
    if receipt["status"] != "passed":
        raise FixtureError(str(receipt.get("error", failure or "fixture failed")))
    print(json.dumps(receipt, indent=2))
    return work


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--work-root", required=True, type=Path)
    parser.add_argument("--image", required=True)
    args = parser.parse_args()
    try:
        run(args.build_dir, args.config, args.work_root, args.image)
    except Exception as exc:
        print("HANGAR CAPACITY SQL FIXTURE FAILED: " + _redact(str(exc)))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
