param(
    [Parameter(Mandatory=$true)][string]$ServerIni,
    [Parameter(Mandatory=$true)][string]$OriginalBackup,
    [string]$BuildDir='build',
    [string]$Config='Release'
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$build=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($BuildDir)){$BuildDir}else{Join-Path $repo $BuildDir}))
$work=Join-Path $build ('rank_mod_sql_test\'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$ini=@{};$section=''
foreach($line in [IO.File]::ReadAllLines([IO.Path]::GetFullPath($ServerIni))){
    if($line -match '^\[(.+)\]'){$section=$Matches[1]}
    elseif($section -eq 'DB Game' -and $line -match '^\s*([^=]+)=(.*)$'){$ini[$Matches[1].Trim()]=$Matches[2].Trim()}
}
$builder=New-Object System.Data.SqlClient.SqlConnectionStringBuilder
$builder['Data Source']=$ini['DBServerName'];$builder['Initial Catalog']=$ini['DBName'];$builder['User ID']=$ini['ID'];$builder['Password']=$ini['PWD'];$builder['Connect Timeout']=5
$connection=New-Object System.Data.SqlClient.SqlConnection $builder.ConnectionString
$connection.Open()
function Exec([string]$sql){$cmd=$connection.CreateCommand();$cmd.CommandTimeout=30;$cmd.CommandText=$sql;[void]$cmd.ExecuteNonQuery();$cmd.Dispose()}
function Rows([string]$sql){$cmd=$connection.CreateCommand();$cmd.CommandText=$sql;$table=New-Object Data.DataTable;$reader=$cmd.ExecuteReader();$table.Load($reader);$reader.Close();$cmd.Dispose();return ,$table}
function Q([string]$name){'['+$name.Replace(']',']]')+']'}
$source=$ini['DBName'];$name='SDGO_RankModTest_'+[guid]::NewGuid().ToString('N').Substring(0,12);$owner=[guid]::NewGuid().ToString('N')
$backup=Get-Content -LiteralPath $OriginalBackup -Raw -Encoding UTF8 | ConvertFrom-Json
if($backup.fix -ne 'rank_progression_fix' -or @($backup.procedures).Count -ne 2){throw 'Need the original two-procedure recovery backup for an unpatched fixture'}
$before=Rows "SELECT o.name,m.definition FROM sys.sql_modules m JOIN sys.objects o ON o.object_id=m.object_id WHERE o.name IN ('sp_SetBattleExp_V4','sp_SetGridBattleExp_V2')"
$passed=$false;$created=$false
$priorCommonFiles=$env:CommonProgramFiles
try {
    Exec ('CREATE DATABASE '+(Q $name));$created=$true;Exec ('USE '+(Q $name))
    Exec "EXEC sys.sp_addextendedproperty @name=N'sdgo_rank_mod_test_owner',@value=N'$owner'"
    $tables=@('tbl_Users','tbl_Levelname','tbl_UserUnits','tbl_UserBattleShips','tbl_UserOperators','tbl_OperatorData','tbl_UnitComposeInfo','tbl_ShipSkillData','tbl_UserShipSkillPartItems')
    foreach($table in $tables){Exec ('SELECT TOP (0) src.* INTO dbo.'+(Q $table)+' FROM '+(Q $source)+'.dbo.'+(Q $table)+' src')}
    Exec ('INSERT dbo.tbl_Levelname SELECT * FROM '+(Q $source)+'.dbo.tbl_Levelname')
    $dependencies=Rows ('SELECT DISTINCT referenced_entity_name AS name FROM '+(Q $source)+".sys.sql_expression_dependencies WHERE referencing_id IN (OBJECT_ID(N'"+$source+".dbo.sp_SetBattleExp_V4'),OBJECT_ID(N'"+$source+".dbo.sp_SetGridBattleExp_V2')) AND referenced_entity_name LIKE 'sp%' ")
    foreach($dependency in $dependencies.Rows){
        $proc=[string]$dependency.name
        if($proc -notmatch '^sp[a-zA-Z0-9_]+$'){throw 'Unsupported fixture procedure name'}
        $params=Rows ('SELECT p.name,t.name AS type,p.max_length,p.precision,p.scale,p.is_output FROM '+(Q $source)+'.sys.parameters p JOIN '+(Q $source)+'.sys.types t ON p.user_type_id=t.user_type_id WHERE p.object_id=OBJECT_ID(N'''+$source+'.dbo.'+$proc+''') AND p.parameter_id>0 ORDER BY p.parameter_id')
        $definitions=@();$assignments=@()
        foreach($param in $params.Rows){
            $type=[string]$param.type
            if($type -in @('varchar','char','binary','varbinary')){$type+='('+[string]$param.max_length+')'}
            elseif($type -in @('nvarchar','nchar')){$type+='('+[string]([int]$param.max_length/2)+')'}
            elseif($type -in @('numeric','decimal')){$type+='('+[string]$param.precision+','+[string]$param.scale+')'}
            $definitions+=([string]$param.name+' '+$type+$(if($param.is_output){' OUTPUT'}else{''}))
            if($param.is_output){$assignments+=('SET '+[string]$param.name+'=0;')}
        }
        $body=if($proc -eq 'sp_UseHaro'){($assignments -join '')+' RETURN 0;'}else{"RAISERROR('Unexpected fixture dependency',16,1); RETURN 999;"}
        Exec ('CREATE PROCEDURE dbo.'+(Q $proc)+' '+($definitions -join ',')+' AS '+$body)
    }
    foreach($entry in $backup.procedures){
        if($entry.name -notin @('sp_SetBattleExp_V4','sp_SetGridBattleExp_V2')){throw 'Original backup scope differs'}
        Exec ('SET ANSI_NULLS '+$(if($entry.ansi_nulls){'ON'}else{'OFF'}));Exec ('SET QUOTED_IDENTIFIER '+$(if($entry.quoted_identifier){'ON'}else{'OFF'}))
        $definition=[string]$entry.definition
        $definition=$definition -replace '\[GOnlineGame\]\.\[dbo\]\.\[tbl_ShipSkillData\]','dbo.tbl_ShipSkillData'
        Exec $definition
    }
    foreach($table in @('tbl_Users','tbl_UserBattleShips')){
        $columns=Rows "SELECT c.name,t.name AS type,c.is_identity,c.is_computed FROM sys.columns c JOIN sys.types t ON c.user_type_id=t.user_type_id WHERE c.object_id=OBJECT_ID(N'dbo.$table') ORDER BY c.column_id"
        $names=@();$values=@();$identity=$false
        foreach($column in $columns.Rows){
            if($column.is_computed -or $column.type -in @('timestamp','rowversion')){continue}
            $names+=(Q ([string]$column.name));$identity=$identity -or [bool]$column.is_identity
            $value=if($column.name -eq 'AccountNumber'){'900000001'}elseif($column.name -eq 'UserBattleShipNumber'){'1'}elseif($column.name -eq 'UserRating'){'1000'}elseif($column.name -eq 'PilotName'){"N'rank-fixture'"}elseif($column.type -in @('char','varchar','nvarchar','nchar')){"N'0'"}elseif($column.type -in @('datetime','smalldatetime','datetime2','date')){"'2020-01-01'"}elseif($column.type -in @('binary','varbinary','image')){'0x00'}else{'0'}
            $values+=$value
        }
        if($identity){Exec "SET IDENTITY_INSERT dbo.$table ON"}
        Exec ('INSERT dbo.'+(Q $table)+'('+($names -join ',')+') VALUES('+($values -join ',')+')')
        if($identity){Exec "SET IDENTITY_INSERT dbo.$table OFF"}
    }
    $calls=@();$rpc=@();$boundaries=Rows 'SELECT levelID,LevelStep,levelExp,levelExpMax FROM dbo.tbl_Levelname ORDER BY levelExp'
    if($boundaries.Rows.Count -lt 1){throw 'No rank boundaries in fixture configuration'}
    foreach($proc in @('sp_SetBattleExp_V4','sp_SetGridBattleExp_V2')){
        $parameters=Rows "SELECT p.name,t.name AS type,p.max_length,p.is_output FROM sys.parameters p JOIN sys.types t ON p.user_type_id=t.user_type_id WHERE p.object_id=OBJECT_ID(N'dbo.$proc') AND p.parameter_id>0 ORDER BY p.parameter_id"
        $declare=@();$arguments=@();$bound=@('int:2:0');$index=0
        foreach($parameter in $parameters.Rows){
            $param=[string]$parameter.name
            if($parameter.is_output){$local='@test'+$index;$declare+=('DECLARE '+$local+' '+[string]$parameter.type+';');$arguments+=($param+'='+$local+' OUTPUT')}
            else{$v=if($param -eq '@v_AccountNum'){'900000001'}elseif($param -eq '@v_ExpVal'){'@AWARD@'}elseif($param -in @('@v_UnitLevel','@v_UserBattleShipNum','@v_BattleCnt','@v_WinCnt')){'1'}else{'0'};$arguments+=($param+'='+$v)}
            if($parameter.type -notin @('int','tinyint','smallint','bigint','float','real','bit','varchar')){throw "Unsupported RPC fixture parameter type $($parameter.type) ($param)"}
            if($parameter.type -eq 'varchar' -and -not $parameter.is_output){throw 'Unsupported text input in RPC fixture'}
            $bound+=([string]$parameter.type+':'+$(if($parameter.is_output){'2:0'}else{'1:'+$v.Replace('@AWARD@','20')}))
            ++$index
        }
        $rpc+=('{? = call '+$proc+'('+((@('?')*$parameters.Rows.Count) -join ',')+')}|'+($bound -join ','))
        $sql='SET NOCOUNT ON;'+($declare -join '')+'DECLARE @rc int; EXEC @rc=@SCHEMA@.'+(Q $proc)+' '+($arguments -join ',')+'; IF @rc<>0 RAISERROR(''Native fixture call failed'',16,1);'
        $reset="UPDATE dbo.tbl_Users SET UserLevelNumber=15,UserLevelStep=1,UserExp=30090,UserLvlNm='fixture',UserBattle=0,UserWin=0 WHERE AccountNumber=900000001"
        $calls+=('0|5,2,30110,1,1|'+$reset+'|'+$sql.Replace('@AWARD@','20'))
        $calls+=('1|15,1,30110,1,1|'+$reset+'|'+$sql.Replace('@AWARD@','20'))
        $cases=@($boundaries.Rows | ForEach-Object {,@(15,1,[int]$_.levelExp)})
        $cases+=@(@(5,1,30100),@(5,5,48600),@(7,5,103400),@(8,5,180000),@(15,1,1000),@(15,4,30100),@(15,5,3500000),@(16,1,8000000),@(20,1,30000))
        foreach($case in $cases){
            $major=[int]$case[0];$step=[int]$case[1];$exp=[int]$case[2]
            $level=@($boundaries.Rows | Where-Object {[int]$_.levelExp -le $exp -and $exp -lt [int]$_.levelExpMax})
            if($level.Count -ne 1){throw 'Ambiguous rank EXP interval'}
            $wantMajor=$major;$wantStep=$step
            if($major -lt 16 -and -not ($major -eq 15 -and $step -eq 5) -and ([int]$level[0].levelID -gt $major -or ([int]$level[0].levelID -eq $major -and [int]$level[0].LevelStep -gt $step))){$wantMajor=[int]$level[0].levelID;$wantStep=[int]$level[0].LevelStep}
            $initialExp=[Math]::Max($exp-1,0);$award=$exp-$initialExp
            $reset="UPDATE dbo.tbl_Users SET UserLevelNumber=$major,UserLevelStep=$step,UserExp=$initialExp,UserLvlNm='fixture',UserBattle=0,UserWin=0 WHERE AccountNumber=900000001"
            $calls+=('1|'+"$wantMajor,$wantStep,$exp,1,1|"+$reset+'|'+$sql.Replace('@AWARD@',[string]$award))
        }
    }
    $callFile=Join-Path $work 'calls.txt';[IO.File]::WriteAllLines($callFile,$calls,[Text.Encoding]::ASCII)
    $rpcFile=Join-Path $work 'rpc.txt';[IO.File]::WriteAllLines($rpcFile,$rpc,[Text.Encoding]::ASCII)
    $fixtureIni=Join-Path $work 'fixture.ini';$lines=@('[DB Game]',('DBServerName='+$ini['DBServerName']),('DBName='+$name),('ID='+$ini['ID']),('PWD='+$ini['PWD']))
    [IO.File]::WriteAllLines($fixtureIni,$lines,[Text.UTF8Encoding]::new($false))
    if(${env:ProgramFiles(x86)}){$env:CommonProgramFiles=Join-Path ${env:ProgramFiles(x86)} 'Common Files'}
    & (Join-Path $build "$Config\rank_progression_db_test.exe") $fixtureIni $callFile $rpcFile
    if($LASTEXITCODE -ne 0){throw 'Native OLE DB Mod integration test failed'}
    $passed=$true
} finally {
    $env:CommonProgramFiles=$priorCommonFiles
    if($fixtureIni -and (Test-Path -LiteralPath $fixtureIni)){Remove-Item -LiteralPath $fixtureIni -Force}
    if($created){
        $after=Rows ('SELECT o.name,m.definition FROM '+(Q $source)+'.sys.sql_modules m JOIN '+(Q $source)+".sys.objects o ON o.object_id=m.object_id WHERE o.name IN ('sp_SetBattleExp_V4','sp_SetGridBattleExp_V2') AND SCHEMA_NAME(o.schema_id)=N'dbo'")
        if($after.Rows.Count -ne 2 -or $before.Rows.Count -ne 2){throw 'Source definition count differs'}
        foreach($row in $before.Rows){
            $match=@($after.Rows | Where-Object {$_.name -eq $row.name})
            if($match.Count -ne 1 -or [string]$match[0].definition -cne [string]$row.definition){throw 'Source procedure changed during the fixture'}
        }
    }
    if($created -and $passed){
        if($name -notmatch '^SDGO_RankModTest_[0-9a-f]{12}$'){throw 'Invalid owned fixture database'}
        $marker=Rows "SELECT value FROM sys.extended_properties WHERE class=0 AND name=N'sdgo_rank_mod_test_owner'"
        if($marker.Rows.Count -ne 1 -or [string]$marker.Rows[0].value -ne $owner){throw 'Fixture ownership differs'}
        Exec ('USE '+(Q $source));Exec ('DROP DATABASE '+(Q $name))
    }
    $connection.Close();$builder.Clear();$ini.Clear()
    [IO.File]::WriteAllText((Join-Path $work 'result.json'),(@{passed=$passed;database=$name;sourcePlayerRowsCopied=0;scratchRemoved=$passed} | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
}
