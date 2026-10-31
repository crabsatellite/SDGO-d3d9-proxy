<# Install only the battle_ai add-on into an existing framework installation.
   Preserves all unrelated mods, network settings and the user's config.ini. #>
param([Parameter(Mandatory=$true)][string]$GameDir,[string]$Source)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if(-not $Source) {
    $Source=Join-Path $repo 'client'
    if(-not (Test-Path -LiteralPath $Source)) { $Source=Join-Path $repo 'build\stage\Release\client' }
}
$game=(Resolve-Path -LiteralPath $GameDir).Path
$exe=Join-Path $game 'GOnline.exe'
if(-not (Test-Path -LiteralPath $exe)) { throw 'GameDir must contain GOnline.exe' }
$stream=[IO.File]::OpenRead($exe)
try {
    $reader=[IO.BinaryReader]::new($stream)
    $stream.Position=0x3c; $nt=$reader.ReadInt32()
    $stream.Position=$nt; if($reader.ReadUInt32() -ne 0x4550) { throw 'Invalid PE image' }
    $stream.Position=$nt+8; $stamp=$reader.ReadUInt32()
    $stream.Position=$nt+0x18+0x38; $size=$reader.ReadUInt32()
} finally { $stream.Dispose() }
if($stamp -ne 0x54BE1568 -or $size -ne 0x00C97000) { throw 'Unsupported GOnline build' }
if(-not (Test-Path -LiteralPath (Join-Path $game 'sdgo\core.dll'))) { throw 'Install the SDGO mod framework first' }
$running=Get-CimInstance Win32_Process -Filter "Name='GOnline.exe'" | Where-Object {
    $_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq $exe
}
if($running) { throw 'Close this client before installing. Other client copies are not stopped.' }
$sourceMod=Join-Path $Source 'sdgo\mods\battle_ai'
$symbolName='GOnline-54BE1568-00C97000.ini'
$sourceSymbols=Join-Path $Source "sdgo\symbols\$symbolName"
$targetSymbols=Join-Path $game "sdgo\symbols\$symbolName"
$files=@('battle_ai.dll','battle_ai.lua','mod.ini','config.example.ini')
foreach($file in $files) {
    if(-not (Test-Path -LiteralPath (Join-Path $sourceMod $file))) { throw "Missing add-on file $file" }
}
$incoming=[IO.File]::ReadAllText($sourceSymbols)
$symbols=[IO.File]::ReadAllText($targetSymbols)
foreach($name in 'npc.think','npc.init_ai') {
    $pattern='(?ms)^\['+[regex]::Escape($name)+'\]\r?\n.*?(?=^\[|\z)'
    $section=[regex]::Match($incoming,$pattern)
    if(-not $section.Success) { throw "Missing incoming symbol $name" }
    $text=$section.Value.TrimEnd()+"`r`n`r`n"
    $replaceText=$text
    if([regex]::IsMatch($symbols,$pattern)) {
        $symbols=[regex]::Replace($symbols,$pattern,[Text.RegularExpressions.MatchEvaluator]{param($m) $replaceText})
    } else { $symbols=$symbols.TrimEnd()+"`r`n`r`n"+$text }
}
$backup=Join-Path $game ('sdgo\backups\battle_ai\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ'))
$targetMod=Join-Path $game 'sdgo\mods\battle_ai'
New-Item -ItemType Directory -Path $backup -Force | Out-Null
Copy-Item -LiteralPath $targetSymbols -Destination (Join-Path $backup $symbolName)
foreach($file in $files) {
    $prior=Join-Path $targetMod $file
    if(Test-Path -LiteralPath $prior) { Copy-Item -LiteralPath $prior -Destination (Join-Path $backup $file) }
}
New-Item -ItemType Directory -Path $targetMod -Force | Out-Null
foreach($file in $files) { Copy-Item -LiteralPath (Join-Path $sourceMod $file) -Destination (Join-Path $targetMod $file) -Force }
[IO.File]::WriteAllText($targetSymbols,$symbols,[Text.UTF8Encoding]::new($false))
Write-Output "Battle AI installed: $targetMod"
Write-Output "User config.ini and unrelated mods preserved. Backup: $backup"
