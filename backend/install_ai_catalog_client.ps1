# One-time setup for the exported player overlay; normal launchers stay unchanged.
param([string]$GameDir = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$GameDir = [IO.Path]::GetFullPath($GameDir)
$exe = Join-Path $GameDir 'GOnline.exe'
if (!(Test-Path -LiteralPath $exe) -or !(Test-Path -LiteralPath (Join-Path $GameDir 'sdgo/core.dll'))) {
    throw 'Extract into the game folder after installing the matching mod framework.'
}
$raw = [IO.File]::ReadAllBytes($exe)
if ($raw.Length -lt 64 -or [BitConverter]::ToUInt16($raw, 0) -ne 0x5A4D) { throw 'Invalid game executable.' }
$pe = [BitConverter]::ToInt32($raw, 60)
if ($pe -lt 0 -or $pe + 84 -gt $raw.Length -or [BitConverter]::ToUInt32($raw, $pe) -ne 0x4550 -or
    [BitConverter]::ToUInt16($raw, $pe + 4) -ne 0x14C -or
    [BitConverter]::ToUInt32($raw, $pe + 8) -ne 0x54BE1568 -or
    [BitConverter]::ToUInt32($raw, $pe + 80) -ne 0xC97000) { throw 'Unsupported game build.' }
foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='GOnline.exe'")) {
    if (!$process.ExecutablePath) { throw 'Cannot verify a running game copy. Close it first.' }
    if ([IO.Path]::GetFullPath($process.ExecutablePath) -eq $exe) { throw 'Close this game copy before setup.' }
}
function Read-Ini([string]$Text) {
    $sections = @{}
    $section = $null
    foreach ($line in ($Text -split '\r?\n')) {
        $value = $line.Trim().TrimStart([char]0xFEFF)
        if (!$value -or $value.StartsWith(';') -or $value.StartsWith('#')) { continue }
        if ($value -match '^\[([^\]]+)\]$') {
            $section = $Matches[1]
            if ($sections.ContainsKey($section)) { throw "Duplicate section: $section" }
            $sections[$section] = @{}
        } elseif ($section -and $value -match '^([^=]+)=(.*)$') {
            $key = $Matches[1].Trim()
            if ($sections[$section].ContainsKey($key)) { throw "Duplicate key: $section/$key" }
            $sections[$section][$key] = $Matches[2].Trim()
        } else { throw 'Invalid symbol INI line.' }
    }
    return $sections
}
$fragment = Join-Path $GameDir 'sdgo/mods/ai_catalog/client-symbols.ini'
$symbol = Join-Path $GameDir 'sdgo/symbols/GOnline-54BE1568-00C97000.ini'
$donor = Read-Ini ([IO.File]::ReadAllText($fragment))
if (!(Test-Path -LiteralPath $symbol)) { throw 'Matching framework symbol file is missing.' }
$original = [IO.File]::ReadAllBytes($symbol)
$current = Read-Ini ([IO.File]::ReadAllText($symbol))
if (!$current.ContainsKey('host') -or !$donor.ContainsKey('host')) { throw 'Symbol host identity missing.' }
foreach ($key in 'timestamp', 'size_of_image') {
    if ($current.host[$key] -ne $donor.host[$key]) { throw 'Symbol host identity differs from this addon.' }
}
$append = ''
foreach ($section in ($donor.Keys | Sort-Object)) {
    if ($current.ContainsKey($section)) {
        if ($section -eq 'host') { continue }
        foreach ($key in $donor[$section].Keys) {
            if ($current[$section][$key] -ne $donor[$section][$key]) { throw "Existing symbol conflicts: $section/$key" }
        }
    } else {
        $append += "`r`n[$section]`r`n"
        foreach ($key in ($donor[$section].Keys | Sort-Object)) { $append += "$key=$($donor[$section][$key])`r`n" }
    }
}
if ($append) {
    $backup = $symbol + '.ai-catalog-before'
    if (!(Test-Path -LiteralPath $backup)) { [IO.File]::WriteAllBytes($backup, $original) }
    $temporary = $symbol + '.ai-catalog-' + [guid]::NewGuid().ToString('N')
    $replaced = $temporary + '.before'
    try {
        [IO.File]::WriteAllBytes($temporary, [byte[]]($original + [Text.Encoding]::UTF8.GetBytes($append)))
        # Both files exist on the same volume: replace atomically after full preflight.
        [IO.File]::Replace($temporary, $symbol, $replaced)
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
        if (Test-Path -LiteralPath $replaced) { Remove-Item -LiteralPath $replaced }
    }
}
Write-Host 'AI catalog client setup complete. Use your normal launcher.'
