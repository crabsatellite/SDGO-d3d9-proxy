param([string]$Config='Release')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$run=Join-Path $repo ('build\battle_ai_server_native_runs\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$mod=Join-Path $run 'sdgo\mods\battle_ai_server'
New-Item -ItemType Directory -Force -Path $mod,(Join-Path $run 'sdgo\symbols') | Out-Null
Copy-Item -LiteralPath (Join-Path $repo "build\$Config\battle_ai_server_native_test.exe") -Destination $run
Copy-Item -LiteralPath (Join-Path $repo "build\$Config\core.dll") -Destination (Join-Path $run 'sdgo')
Copy-Item -LiteralPath (Join-Path $repo "build\$Config\battle_ai_server.dll") -Destination $mod
$manifest=[IO.File]::ReadAllText((Join-Path $repo 'mods\battle_ai_server\mod.ini')).Replace('hosts=BattleServer-548A891D-0046C000','hosts=*')
[IO.File]::WriteAllText((Join-Path $mod 'mod.ini'),$manifest)
[IO.File]::WriteAllText((Join-Path $run 'sdgo\sdgo.ini'),"[core]`r`nlog_level=1`r`n")
& (Join-Path $run 'battle_ai_server_native_test.exe') 2>&1 | Tee-Object -FilePath (Join-Path $run 'result.txt')
if($LASTEXITCODE -ne 0) { throw "Server rules native fixture failed: $run" }
Write-Output "SERVER NATIVE SP CASE PASSED: $run"
