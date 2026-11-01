<# Isolated file-only installation test. No game, service, login or SQL runs. #>
param([Parameter(Mandatory=$true)][string]$NativeExe,[string]$BuildDir='build')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$build=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($BuildDir)){$BuildDir}else{Join-Path $repo $BuildDir}))
$work=Join-Path $build ('room_random_install_test_run\'+[guid]::NewGuid().ToString('N'))
$target=Join-Path $work '随机服务器'
$nativeIni=Join-Path $work '数据库配置\WorldServer.ini'
$source=Join-Path $build 'stage\Release\server'
New-Item -ItemType Directory -Force -Path (Join-Path $target 'sdgo\symbols'),(Join-Path $target 'sdgo\mods\other_agent'),(Split-Path -Parent $nativeIni)|Out-Null
Copy-Item -LiteralPath $NativeExe -Destination (Join-Path $target 'BattleServer.exe')
[IO.File]::WriteAllText((Join-Path $target 'sdgo\core.dll'),'file-only fixture')
[IO.File]::WriteAllText((Join-Path $target 'winmm.dll'),'file-only fixture')
$ai=Join-Path $target 'sdgo\mods\pvp_ai'
New-Item -ItemType Directory -Path $ai|Out-Null
[IO.File]::WriteAllText((Join-Path $ai 'pvp_ai.dll'),'old file-only AI fixture')
[IO.File]::WriteAllText((Join-Path $ai 'mod.ini'),"[mod]`r`nid=pvp_ai`r`ndll=pvp_ai.dll`r`n[settings]`r`nDefaultLevel=4`r`n; owner settings`r`n")
$aiManifestHash=(Get-FileHash -LiteralPath (Join-Path $ai 'mod.ini')).Hash
$other=Join-Path $target 'sdgo\mods\other_agent\config.ini'
[IO.File]::WriteAllText($other,'preserve this unrelated mod')
[IO.File]::WriteAllText($nativeIni,"[DB Game]`r`nDBServerName=example.invalid`r`nDBName=fixture`r`nID=fixture`r`nPWD=fixture-only`r`n",[Text.Encoding]::Unicode)
$symbol=Join-Path $target 'sdgo\symbols\BattleServer-548A891D-0046C000.ini'
[IO.File]::WriteAllText($symbol,"[host]`r`nimage=BattleServer.exe`r`ntimestamp=0x548A891D`r`nsize_of_image=0x0046C000`r`n`r`n; 原有注释`r`n[other_agent.fixture]`r`nkind=data`r`nrva=0x00001000`r`n",(New-Object Text.UTF8Encoding $false))
$beforeExe=(Get-FileHash -LiteralPath (Join-Path $target 'BattleServer.exe')).Hash
$beforeOther=(Get-FileHash -LiteralPath $other).Hash
function Assert($test,$message){if(-not $test){throw "FAIL: $message"};Write-Host "ok $message"}
function Value($file,$section,$key){
    $buffer=New-Object Text.StringBuilder 2048
    [void][RoomRandomInstall.Ini]::GetPrivateProfileString($section,$key,'',$buffer,2048,$file)
    $buffer.ToString()
}
& (Join-Path $repo 'tools\install_room_random.ps1') -ServerRoot $target -DatabaseIni $nativeIni -Source $source
Assert ((Get-FileHash -LiteralPath (Join-Path $target 'BattleServer.exe')).Hash -eq $beforeExe) 'native executable not changed'
Assert ((Get-FileHash -LiteralPath $other).Hash -eq $beforeOther) 'unrelated mod preserved'
Assert ((Get-FileHash -LiteralPath (Join-Path $target 'sdgo\core.dll')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $source 'sdgo\core.dll')).Hash) 'core updated with shared command API'
Assert ((Get-FileHash -LiteralPath (Join-Path $ai 'pvp_ai.dll')).Hash -eq (Get-FileHash -LiteralPath (Join-Path $source 'sdgo\mods\pvp_ai\pvp_ai.dll')).Hash) 'installed PvP AI updated'
Assert ((Get-FileHash -LiteralPath (Join-Path $ai 'mod.ini')).Hash -eq $aiManifestHash) 'AI manifest and owner settings preserved'
Assert (-not (Test-Path -LiteralPath (Join-Path $target 'sdgo\mods\coop_ai'))) 'absent AI mod not installed'
Assert ([IO.File]::ReadAllText($symbol).Contains('[other_agent.fixture]')) 'unrelated symbol preserved'
Assert ([IO.File]::ReadAllText($symbol).Contains('原有注释')) 'Unicode symbol comments preserved'
Assert ([IO.File]::ReadAllText($symbol).Contains('[battle.build_human_unit]')) 'new symbols added'
Assert ([IO.File]::ReadAllText($symbol).Contains('[battle.room_random_broadcast]')) 'native loadout notification symbol added'
Assert (Test-Path -LiteralPath (Join-Path $target 'sdgo\mods\room_random\manager.json')) 'Manager metadata installed'
$config=Join-Path $target 'sdgo\mods\room_random\config.ini'
Assert ((Value $config 'settings' 'DatabaseIni') -eq $nativeIni) 'Unicode database reference round trips'
Assert (-not [IO.File]::ReadAllText($config).Contains('fixture-only')) 'database password was not copied'
[void][RoomRandomInstall.Ini]::WritePrivateProfileString('settings','QueryTimeoutSeconds','5',$config)
& (Join-Path $repo 'tools\install_room_random.ps1') -ServerRoot $target -DatabaseIni $nativeIni -Source $source
Assert ((Value $config 'settings' 'QueryTimeoutSeconds') -eq '5') 'owner settings preserved on upgrade'
Assert ((Get-FileHash -LiteralPath $other).Hash -eq $beforeOther) 'repeat install still preserves other mods'
Assert ((Get-ChildItem -LiteralPath (Join-Path $target 'sdgo\backups\room_random') -Directory).Count -eq 2) 'both installations retain backups'
$locked=[IO.File]::Open((Join-Path $target 'sdgo\core.dll'),[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
$refused=$false
try{& (Join-Path $repo 'tools\install_room_random.ps1') -ServerRoot $target -DatabaseIni $nativeIni -Source $source}
catch{$refused=$_.Exception.Message.Contains('is in use')}
finally{$locked.Close()}
Assert $refused 'in-use core refused without stopping any process'
Assert ((Get-ChildItem -LiteralPath (Join-Path $target 'sdgo\backups\room_random') -Directory).Count -eq 2) 'refused install did not start writes'
Write-Host 'ROOM RANDOM INSTALLATION CHECKS PASSED'
