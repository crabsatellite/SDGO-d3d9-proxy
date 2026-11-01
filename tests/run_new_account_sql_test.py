"""Actual-core/OLE DB checks in uniquely named databases inside an existing SQL test container.

Never touches the container's existing databases and never executes a game.
Pass a local JSON credential file containing sa_password; no secret enters logs or argv.
"""
from __future__ import annotations
import argparse,contextlib,json,os,secrets,socket,socketserver,subprocess,threading,uuid
from pathlib import Path

class Bridge(socketserver.ThreadingTCPServer):
    allow_reuse_address=False
    daemon_threads=True
class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        process=subprocess.Popen(['docker','exec','-i',self.server.container,'bash','-c',
            'exec 3<>/dev/tcp/127.0.0.1/1433; cat <&3 & reader=$!; cat >&3; kill "$reader" 2>/dev/null || true'],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        def receive():
            try:
                while True:
                    data=os.read(process.stdout.fileno(),65536)
                    if not data:break
                    self.request.sendall(data)
            except OSError:pass
            finally:
                with contextlib.suppress(OSError):self.request.shutdown(socket.SHUT_WR)
        reader=threading.Thread(target=receive,daemon=True);reader.start()
        try:
            while True:
                data=self.request.recv(65536)
                if not data:break
                process.stdin.write(data);process.stdin.flush()
        except (OSError,BrokenPipeError):pass
        finally:
            with contextlib.suppress(OSError):process.stdin.close()
            reader.join(timeout=3)
            with contextlib.suppress(subprocess.TimeoutExpired):process.wait(timeout=3)
            if process.poll() is None:process.kill()

def run(container:str,credentials:Path,work:Path):
    repo=Path(__file__).resolve().parents[1]
    work.mkdir(parents=True,exist_ok=False)
    private=json.loads(credentials.read_text(encoding='utf-8-sig'))
    tag=uuid.uuid4().hex[:8];token=uuid.uuid4().hex
    common,game,billing=('StarterC_'+tag,'StarterG_'+tag,'StarterB_'+tag)
    login='StarterL_'+tag;password='Test_'+secrets.token_hex(10)+'!7'
    environment=dict(os.environ,SQLCMDPASSWORD=private['sa_password'],STARTER_TEST_PASSWORD=password)
    def sql(text:str,quiet=False):
        result=subprocess.run(['docker','exec','-i','-e','SQLCMDPASSWORD','-e','STARTER_TEST_PASSWORD',container,
            '/opt/mssql-tools18/bin/sqlcmd','-S','localhost','-U','sa','-C','-b','-h','-1','-W'],
            input=text,text=True,capture_output=True,env=environment)
        if result.returncode:raise RuntimeError('Isolated SQL fixture command failed: '+result.stdout+result.stderr)
        return result.stdout.strip()
    databases=(common,game,billing)
    setup='\n'.join(f"CREATE DATABASE [{db}];\nGO\nUSE [{db}];\nCREATE TABLE dbo.starter_test_marker(token varchar(32));INSERT dbo.starter_test_marker VALUES('{token}');\nGO" for db in databases)
    setup+=f"\nUSE master;\nCREATE LOGIN [{login}] WITH PASSWORD='$(STARTER_TEST_PASSWORD)',CHECK_POLICY=OFF;\nGO\n"
    for db in databases:setup+=f"USE [{db}];CREATE USER [{login}] FOR LOGIN [{login}];ALTER ROLE db_owner ADD MEMBER [{login}];\nGO\n"
    setup+=f"""
USE [{common}];
CREATE TABLE dbo.tbl_Account(f_idx int IDENTITY(1,1) PRIMARY KEY,f_loginId varchar(14) NOT NULL UNIQUE);
GO
CREATE PROCEDURE dbo.sp_TestRegister @id varchar(14) AS
SET NOCOUNT ON;SET XACT_ABORT ON;
IF EXISTS(SELECT 1 FROM dbo.tbl_Account WHERE f_loginId=@id) RETURN;
BEGIN TRAN;
INSERT dbo.tbl_Account(f_loginId) VALUES(@id);
DECLARE @idx int=SCOPE_IDENTITY();
INSERT [{billing}].dbo.tbls_AccountPocket VALUES(@idx,9999999,9999999);
COMMIT;
GO
USE [{billing}];
CREATE TABLE dbo.tbls_AccountPocket(f_idx int PRIMARY KEY,f_GC int NOT NULL,f_GP int NOT NULL);
GO
USE [{game}];
CREATE TABLE dbo.tbl_Users(AccountNumber int PRIMARY KEY,UserLevelNumber tinyint,UserLevelStep int,UserExp int);
CREATE TABLE dbo.tbl_Levelname(levelID int,LevelStep int,levelExp int,PRIMARY KEY(levelID,LevelStep));
INSERT dbo.tbl_Levelname VALUES(5,1,25700),(6,2,32000);
CREATE TABLE dbo.starter_grants(AccountNumber int,Items int);
GO
CREATE PROCEDURE dbo.sp_TestCreateProfile @id int AS
SET NOCOUNT ON;
IF NOT EXISTS(SELECT 1 FROM dbo.tbl_Users WHERE AccountNumber=@id)
BEGIN
 INSERT dbo.tbl_Users VALUES(@id,15,1,0);
 INSERT dbo.starter_grants VALUES(@id,3);
END
ELSE UPDATE dbo.tbl_Users SET UserExp=UserExp+100 WHERE AccountNumber=@id;
GO
"""
    created=False;bridge=None
    try:
        sql(setup);created=True
        bridge=Bridge(('127.0.0.1',0),Handler);bridge.container=container
        thread=threading.Thread(target=bridge.serve_forever,daemon=True);thread.start();port=bridge.server_address[1]
        cases=[]
        for mode in ('enabled','configured','disabled'):
            sql(f"USE [{common}];TRUNCATE TABLE dbo.tbl_Account;USE [{billing}];TRUNCATE TABLE dbo.tbls_AccountPocket;USE [{game}];TRUNCATE TABLE dbo.tbl_Users;TRUNCATE TABLE dbo.starter_grants;\nGO\n")
            target=work/mode;mod=target/'sdgo/mods/new_account_profile';mod.mkdir(parents=True)
            (target/'sdgo/symbols').mkdir()
            for name,destination in (('new_account_sql_test.exe',target),('core.dll',target/'sdgo'),('new_account_profile.dll',mod)):
                import shutil
                shutil.copy2(repo/'build/Release'/name,destination/name)
            manifest=(repo/'mods/new_account_profile/mod.ini').read_text(encoding='utf-8-sig')
            import re
            manifest=re.sub(r'(?m)^hosts=.*$','hosts=*',manifest)
            (mod/'mod.ini').write_text(manifest,encoding='ascii')
            if mode=='configured':
                (mod/'config.ini').write_text('[settings]\nInitialGP=1200\nInitialCash=0\nRankNumber=6\nRankStep=2\n',encoding='ascii')
            enabled=mode!='disabled'
            (target/'sdgo/sdgo.ini').write_text('[core]\nlog_level=0\n[mods]\nnew_account_profile='+('1' if enabled else '0')+'\n',encoding='ascii')
            env=dict(os.environ,SDGO_STARTER_BILLING_NAME=billing,CommonProgramFiles=r'C:\Program Files (x86)\Common Files')
            for key,db in (('SDGO_STARTER_COMMON',common),('SDGO_STARTER_GAME',game)):
                env[key]=f'Provider=SQLOLEDB;Data Source=127.0.0.1,{port};Initial Catalog={db};User ID={login};Password={password};'
            result=subprocess.run([str(target/'new_account_sql_test.exe'),mode],capture_output=True,text=True,errors='replace',env=env,timeout=120)
            (target/'stdout.txt').write_text(result.stdout+result.stderr,encoding='utf-8')
            if result.returncode:raise RuntimeError(f'{mode} fixture failed ({result.returncode}): '+result.stdout+result.stderr)
            rows=sql(f"SET NOCOUNT ON;SELECT a.f_loginId,p.f_GP,p.f_GC,u.UserLevelNumber,u.UserLevelStep,u.UserExp FROM [{common}].dbo.tbl_Account a LEFT JOIN [{billing}].dbo.tbls_AccountPocket p ON p.f_idx=a.f_idx LEFT JOIN [{game}].dbo.tbl_Users u ON u.AccountNumber=a.f_idx ORDER BY a.f_idx;\nGO\n")
            (target/'sql-readback.txt').write_text(rows,encoding='utf-8')
            lines=[line for line in rows.splitlines() if line.strip()]
            primary=next(line.split() for line in lines if line.startswith('starter-new'))
            expected=('7000','321','6','2' if mode=='configured' else '1','30100') if enabled else ('9999999','9999999','15','1','0')
            assert tuple(primary[1:])==expected,(primary,expected)
            if enabled:
                assert not any(line.startswith('fail-wallet') for line in lines)
                failed_profile=next(line.split() for line in lines if line.startswith('fail-profile'))
                assert tuple(failed_profile[1:])==('1200' if mode=='configured' else '5000','0','NULL','NULL','NULL'),rows
                assert sql(f'SET NOCOUNT ON;SELECT COUNT(*) FROM [{game}].dbo.starter_grants;\nGO\n').strip()=='1'
            cases.append(dict(mode=mode,passed=True,rows=len(lines)))
        receipt=dict(cases=cases,isolated_databases=list(databases),no_original_database_changes=True,no_game_execution=True)
        (work/'result.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
        print(json.dumps(receipt,indent=2))
    finally:
        if bridge:bridge.shutdown();bridge.server_close()
        if created:
            # Only our exact names with our ownership marker may be dropped.
            for db in databases:
                assert db.startswith(('StarterC_','StarterG_','StarterB_')) and db.endswith(tag)
                marker=sql(f"SET NOCOUNT ON;SELECT token FROM [{db}].dbo.starter_test_marker;\nGO\n")
                if marker.strip()!=token:raise RuntimeError('Fixture ownership changed; database retained')
                sql(f'USE master;ALTER DATABASE [{db}] SET SINGLE_USER WITH ROLLBACK IMMEDIATE;DROP DATABASE [{db}];\nGO\n')
            sql(f'USE master;DROP LOGIN [{login}];\nGO\n')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--container',required=True);parser.add_argument('--credential-file',type=Path,required=True);parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();run(args.container,args.credential_file,args.out.resolve())
