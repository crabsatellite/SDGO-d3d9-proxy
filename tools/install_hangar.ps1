<# Installs only hangar_manager and its symbol additions. Existing core and other mods are preserved. #>
param(
    [Parameter(Mandatory=$true)][string]$GameDir,
    [string]$Source,
    [switch]$RefreshRules
)
$ErrorActionPreference='Stop'
if (-not $Source) { $Source=Join-Path $PSScriptRoot 'client' }
$gameRoot=(Resolve-Path -LiteralPath $GameDir).Path
$sourceRoot=(Resolve-Path -LiteralPath $Source).Path
$exe=Join-Path $gameRoot 'GOnline.exe'
$stream=[IO.File]::OpenRead($exe)
try {
    $reader=New-Object IO.BinaryReader($stream)
    $stream.Position=0x3C; $nt=$reader.ReadInt32(); $stream.Position=$nt
    if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x14C) { throw 'Not a Win32 SDGO client' }
    $stream.Position=$nt+8; $stamp=$reader.ReadUInt32()
    $stream.Position=$nt+0x18+0x38; $size=$reader.ReadUInt32()
} finally { $stream.Close() }
if ($stamp -ne 0x54BE1568 -or $size -ne 0x00C97000) { throw 'This package supports GOnline-54BE1568-00C97000 only' }
$running=Get-CimInstance Win32_Process -Filter "Name='GOnline.exe'" | Where-Object { $_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq $exe }
if ($running) { throw 'Close this specific client before installing; no process was stopped' }
$sourceMod=Join-Path $sourceRoot 'sdgo\mods\hangar_manager'
foreach ($name in @('hangar_manager.dll','mod.ini','units.tsv')) {
    if (-not (Test-Path -LiteralPath (Join-Path $sourceMod $name))) { throw "Missing package file: $name" }
}
$symbolName='GOnline-54BE1568-00C97000.ini'
$sourceSymbols=Join-Path $sourceRoot "sdgo\symbols\$symbolName"
if (-not (Test-Path -LiteralPath $sourceSymbols)) { throw 'Missing verified symbols' }
$core=Join-Path $gameRoot 'sdgo\core.dll'
if (-not (Test-Path -LiteralPath $core)) {
    $baseInstaller=Join-Path $PSScriptRoot 'install_client.ps1'
    if (-not (Test-Path -LiteralPath $baseInstaller)) { throw 'Install the mod framework first, or use the complete hangar package' }
    & $baseInstaller -GameDir $gameRoot -Source $sourceRoot
    if (-not (Test-Path -LiteralPath $core)) { throw 'Base framework installation failed' }
}
function IniSections([string]$text) {
    $map=@{}; $current=''
    foreach ($line in ($text -split '\r?\n')) {
        if ($line -match '^\s*\[([^]]+)\]\s*$') {
            $current=$Matches[1]
            if ($map.ContainsKey($current)) { throw "Duplicate symbol section: $current" }
            $map[$current]=New-Object 'System.Collections.Generic.List[string]'
        } elseif ($current) { $map[$current].Add($line) }
    }
    return $map
}
function IniFields($lines) {
    $fields=@{}
    foreach ($line in $lines) { if ($line -match '^\s*(kind|rva|expect)\s*=(.*)$') { $fields[$Matches[1]]=$Matches[2].Trim() } }
    return $fields
}
$incoming=IniSections ([IO.File]::ReadAllText($sourceSymbols))
$managed=@($incoming.Keys | Where-Object { $_ -like 'hangar.*' })
$baseNames=@('game.app','net.send_packet')
$destinationSymbols=Join-Path $gameRoot "sdgo\symbols\$symbolName"
New-Item -ItemType Directory -Path (Split-Path -Parent $destinationSymbols) -Force | Out-Null
$existingText=if (Test-Path -LiteralPath $destinationSymbols) { [IO.File]::ReadAllText($destinationSymbols) } else { '' }
$existing=IniSections $existingText
foreach ($name in $baseNames) {
    if ($existing.ContainsKey($name)) {
        $old=IniFields $existing[$name]; $new=IniFields $incoming[$name]
        if ($old['kind'] -ne $new['kind'] -or [Convert]::ToUInt32($old['rva'].Replace('0x',''),16) -ne [Convert]::ToUInt32($new['rva'].Replace('0x',''),16)) {
            throw "Existing prerequisite symbol differs: $name. Update the base framework; it was preserved."
        }
    }
}
$mergedSymbols=$existingText
$lineEnding=if ($existingText.Contains("`r`n")) { "`r`n" } else { "`n" }
foreach ($name in @($managed+$baseNames | Sort-Object)) {
    if (-not $existing.ContainsKey($name)) {
        $mergedSymbols+=$lineEnding+'['+$name+']'+$lineEnding+($incoming[$name] -join $lineEnding)+$lineEnding
    } elseif ($managed -contains $name) {
        $new=IniFields $incoming[$name];$old=IniFields $existing[$name]
        foreach ($field in $new.Keys) { if (-not $old.ContainsKey($field)) { throw "Incomplete existing managed symbol: $name/$field" } }
        $sectionPattern='(?ms)^\['+[regex]::Escape($name)+'\][^\r\n]*\r?\n.*?(?=^\[|\z)'
        $section=[regex]::Match($mergedSymbols,$sectionPattern)
        if (-not $section.Success) { throw "Cannot locate existing managed symbol: $name" }
        $updated=[regex]::Replace($section.Value,'(?m)^(\s*(?<key>kind|rva|expect)\s*=)[^\r\n]*',[System.Text.RegularExpressions.MatchEvaluator]{param($m) $m.Groups[1].Value+$new[$m.Groups['key'].Value]})
        $mergedSymbols=$mergedSymbols.Substring(0,$section.Index)+$updated+$mergedSymbols.Substring($section.Index+$section.Length)
    }
}
$targetMod=Join-Path $gameRoot 'sdgo\mods\hangar_manager'
if ([IO.Path]::GetFullPath($targetMod) -eq [IO.Path]::GetFullPath($sourceMod)) { throw 'Source and destination are the same' }
New-Item -ItemType Directory -Path $targetMod -Force | Out-Null
$backupTag=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmss')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8)
$manifestPath=Join-Path $targetMod 'mod.ini'
$manifest=[IO.File]::ReadAllText((Join-Path $sourceMod 'mod.ini'))
if (Test-Path -LiteralPath $manifestPath) {
    $previous=[IO.File]::ReadAllText($manifestPath)
    Copy-Item -LiteralPath $manifestPath -Destination ($manifestPath+'.'+$backupTag+'.bak')
    $settings=[regex]::Match($previous,'(?ms)^\[settings\][^\r\n]*\r?\n.*?(?=^\[|\z)').Value
    if ($settings) { $manifest=[regex]::Replace($manifest,'(?ms)^\[settings\][^\r\n]*\r?\n.*?(?=^\[|\z)',[System.Text.RegularExpressions.MatchEvaluator]{param($m) $settings}) }
    $enabled=[regex]::Match($previous,'(?m)^enabled\s*=\s*([01])\s*$')
    if ($enabled.Success) { $manifest=$manifest.Replace('[mod]','[mod]'+[Environment]::NewLine+'enabled='+$enabled.Groups[1].Value) }
}
Copy-Item -LiteralPath (Join-Path $sourceMod 'hangar_manager.dll') -Destination (Join-Path $targetMod 'hangar_manager.dll') -Force
Copy-Item -LiteralPath (Join-Path $sourceMod 'manager.json') -Destination (Join-Path $targetMod 'manager.json') -Force
$rules=Join-Path $targetMod 'units.tsv'
if ($RefreshRules -or -not (Test-Path -LiteralPath $rules)) {
    if (Test-Path -LiteralPath $rules) { Copy-Item -LiteralPath $rules -Destination ($rules+'.'+$backupTag+'.bak') }
    Copy-Item -LiteralPath (Join-Path $sourceMod 'units.tsv') -Destination $rules -Force
}
[IO.File]::WriteAllText($manifestPath,$manifest,(New-Object Text.UTF8Encoding $false))
if (Test-Path -LiteralPath $destinationSymbols) { Copy-Item -LiteralPath $destinationSymbols -Destination ($destinationSymbols+'.hangar-'+$backupTag+'.bak') }
[IO.File]::WriteAllText($destinationSymbols,$mergedSymbols,(New-Object Text.UTF8Encoding $false))
[IO.File]::WriteAllText((Join-Path $targetMod 'install-receipt.json'),(@{schema_version=1;host='GOnline-54BE1568-00C97000';symbol_sections=$managed;updated_at=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 4),(New-Object Text.UTF8Encoding $false))
Write-Host 'Installed hangar_manager. Existing core, global settings, and unrelated mods were preserved.'
