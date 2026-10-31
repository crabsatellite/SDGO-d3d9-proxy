param([string]$Config='Release')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$tag=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$name='SDGO-BattleAI-0.1.0-preview-'+$tag
$out=Join-Path $repo ('dist\'+$name)
$stage=Join-Path $repo "build\stage\$Config\client"
if(Test-Path -LiteralPath $out) { throw 'Package destination already exists' }
$mod=Join-Path $out 'client\sdgo\mods\battle_ai'
$sym=Join-Path $out 'client\sdgo\symbols'
New-Item -ItemType Directory -Force -Path $mod,$sym,(Join-Path $out 'tools'),(Join-Path $out 'docs') | Out-Null
foreach($file in 'battle_ai.dll','battle_ai.lua','mod.ini','config.example.ini') {
    Copy-Item -LiteralPath (Join-Path $stage "sdgo\mods\battle_ai\$file") -Destination $mod
}
$text=[IO.File]::ReadAllText((Join-Path $stage 'sdgo\symbols\GOnline-54BE1568-00C97000.ini'))
$fragment="; Additive NPC symbol fragment. Use tools/install_battle_ai.ps1.`r`n"
foreach($section in 'npc.think','npc.init_ai') {
    $part=[regex]::Match($text,'(?ms)^\['+[regex]::Escape($section)+'\]\r?\n.*?(?=^\[|\z)')
    if(-not $part.Success) { throw "Missing symbol $section" }
    $fragment+=$part.Value.TrimEnd()+"`r`n`r`n"
}
[IO.File]::WriteAllText((Join-Path $sym 'GOnline-54BE1568-00C97000.ini'),$fragment)
Copy-Item -LiteralPath (Join-Path $repo 'tools\install_battle_ai.ps1') -Destination (Join-Path $out 'tools')
Copy-Item -LiteralPath (Join-Path $repo 'docs\battle-ai.md') -Destination (Join-Path $out 'docs')
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $out
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=$out+'.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($out,$zip)
Write-Output $zip
Write-Output ('SHA256 '+(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)
