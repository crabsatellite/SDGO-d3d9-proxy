param([Parameter(Mandatory=$true)][string]$GameExe)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$work=Join-Path $repo ('build\battle_ai_install_runs\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')+'\game with spaces')
$source=Join-Path $repo 'build\stage\Release\client'
New-Item -ItemType Directory -Force -Path (Join-Path $work 'sdgo\symbols'),(Join-Path $work 'sdgo\mods\net_route'),(Join-Path $work 'sdgo\mods\battle_ai') | Out-Null
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
for($i=0;$i -lt 2;$i++) { & (Join-Path $repo 'tools\install_battle_ai.ps1') -GameDir $work -Source $source }
foreach($key in $keep.Keys) {
    if([IO.File]::ReadAllText((Join-Path $work $key)) -cne $keep[$key]) { throw "Installer overwrote $key" }
}
$readback=[IO.File]::ReadAllText($symbols)
foreach($name in 'npc.think','npc.init_ai','native.extra') {
    if([regex]::Matches($readback,'(?m)^\['+[regex]::Escape($name)+'\]').Count -ne 1) { throw "Symbol merge failed $name" }
}
if([IO.File]::ReadAllBytes((Join-Path $work 'sdgo\mods\battle_ai\battle_ai.dll')).Length -eq 0) { throw 'No DLL installed' }
Write-Output "PASS additive, idempotent install preserves separate config and unrelated network settings: $work"
