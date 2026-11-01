param([Parameter(Mandatory=$true)][string]$GameExe,[string]$ServerExe)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$work=Join-Path $repo ('build\battle_ai_install_runs\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')+'\game with spaces')
$source=Join-Path $repo 'build\stage\Release\client'
New-Item -ItemType Directory -Force -Path (Join-Path $work 'sdgo\symbols'),(Join-Path $work 'sdgo\mods\net_route'),(Join-Path $work 'sdgo\mods\battle_ai'),(Join-Path $work 'sdgo\mods\match_ai') | Out-Null
Copy-Item -LiteralPath $GameExe -Destination (Join-Path $work 'GOnline.exe')
Copy-Item -LiteralPath (Join-Path $source 'sdgo\core.dll') -Destination (Join-Path $work 'sdgo')
$symbols=Join-Path $work 'sdgo\symbols\GOnline-54BE1568-00C97000.ini'
[IO.File]::WriteAllText($symbols,"[host]`r`ntimestamp=0x54BE1568`r`nsize_of_image=0x00C97000`r`n[native.extra]`r`nkind=data`r`nrva=0x123`r`n")
$keep=@{
 'sdgo\mods\net_route\config.ini'="[settings]`nPrivateEndpoint=preserve-this`n";
 'sdgo\mods\battle_ai\config.ini'="[settings]`nStuckMs=2200`n";
 'sdgo\sdgo.ini'="[mods]`nnet_route=1`nmission_ai=1`n";
}
foreach($key in $keep.Keys) { [IO.File]::WriteAllText((Join-Path $work $key),$keep[$key]) }
[IO.File]::WriteAllText((Join-Path $work 'sdgo\mods\match_ai\mod.ini'),"[mod]`nversion=old`n[SETTINGS]`nLogDiagnostics=1`n[private.extra]`nValue=preserved`n")
for($i=0;$i -lt 2;$i++) { & (Join-Path $repo 'tools\install_battle_ai.ps1') -GameDir $work -Source $source }
foreach($key in $keep.Keys) {
    if([IO.File]::ReadAllText((Join-Path $work $key)) -cne $keep[$key]) { throw "Installer overwrote $key" }
}
$readback=[IO.File]::ReadAllText($symbols)
foreach($name in 'npc.think','npc.init_ai','native.extra') {
    if([regex]::Matches($readback,'(?m)^\['+[regex]::Escape($name)+'\]').Count -ne 1) { throw "Symbol merge failed $name" }
}
if([IO.File]::ReadAllBytes((Join-Path $work 'sdgo\mods\battle_ai\battle_ai.dll')).Length -eq 0) { throw 'No DLL installed' }
$match=[IO.File]::ReadAllText((Join-Path $work 'sdgo\mods\match_ai\mod.ini'))
if(-not $match.Contains('LogDiagnostics=1') -or -not $match.Contains('[private.extra]') -or $match.Contains('version=old')) { throw 'Match AI manifest/user-section preservation failed' }
if(-not (Test-Path -LiteralPath (Join-Path $work 'sdgo\mods\match_ai\match_ai.dll'))) { throw 'Matching loadout bridge missing' }
Write-Output "PASS additive, idempotent install preserves separate config and unrelated network settings: $work"
if($ServerExe) {
    $hostWork=Join-Path (Split-Path -Parent $work) 'server with spaces'
    foreach($dir in 'sdgo\symbols','sdgo\mods\udp_pool','sdgo\mods\pvp_ai') { New-Item -ItemType Directory -Force -Path (Join-Path $hostWork $dir) | Out-Null }
    Copy-Item -LiteralPath $ServerExe -Destination (Join-Path $hostWork 'BattleServer.exe')
    Copy-Item -LiteralPath (Join-Path $source 'sdgo\core.dll') -Destination (Join-Path $hostWork 'sdgo')
    $hostSymbols=Join-Path $hostWork 'sdgo\symbols\BattleServer-548A891D-0046C000.ini'
    [IO.File]::WriteAllText($hostSymbols,"[host]`ntimestamp=0x548A891D`nsize_of_image=0x0046C000`n[private.server.symbol]`nkind=data`nrva=0x123`n")
    $hostKeep=@{'BattleServer.ini'="[private.sql]`nDoNotModify=1`n";'sdgo\sdgo.ini'="[mods]`nudp_pool=1`n";'sdgo\mods\udp_pool\config.ini'="[settings]`nPorts=preserve`n"}
    foreach($key in $hostKeep.Keys) { [IO.File]::WriteAllText((Join-Path $hostWork $key),$hostKeep[$key]) }
    [IO.File]::WriteAllText((Join-Path $hostWork 'sdgo\mods\pvp_ai\mod.ini'),"[mod]`nversion=old`n[settings]`nDefaultLevel=4`nLevel4=3604001`n")
    $hostSource=Join-Path $repo 'build\stage\Release\server'
    for($i=0;$i -lt 2;$i++) { & (Join-Path $repo 'tools\install_battle_ai.ps1') -GameDir $hostWork -Source $hostSource -Side Server }
    foreach($key in $hostKeep.Keys) { if([IO.File]::ReadAllText((Join-Path $hostWork $key)) -cne $hostKeep[$key]) { throw "Host installer overwrote $key" } }
    $hostManifest=[IO.File]::ReadAllText((Join-Path $hostWork 'sdgo\mods\pvp_ai\mod.ini'))
    if(-not $hostManifest.Contains('DefaultLevel=4') -or $hostManifest.Contains('version=old')) { throw 'Host manifest/settings preservation failed' }
    if(-not (Test-Path -LiteralPath (Join-Path $hostWork 'sdgo\mods\battle_ai_server\battle_ai_server.dll'))) { throw 'Host fairness component missing' }
    if(-not ([IO.File]::ReadAllText($hostSymbols)).Contains('[private.server.symbol]')) { throw 'Unrelated host symbol missing' }
    Write-Output "PASS host install preserves SQL configuration, UDP pool and NPC pool settings: $hostWork"
}
