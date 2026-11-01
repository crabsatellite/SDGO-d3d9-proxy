<#
.SYNOPSIS
  Installs or removes the standalone activity-capsule currency display fix.

.DESCRIPTION
  This installer is deliberately scoped to capsule_currency_fix:
  - verifies the package hashes and the exact supported GOnline.exe build;
  - installs the required SDGO loader/core only when needed or updates this framework;
  - preserves sdgo.ini, every other mod directory and every unrelated symbol section;
  - merges only [capsule.activity.load_items];
  - uninstall removes only this mod and its symbol section.
#>
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
$PackageRoot = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$Payload = Join-Path $PackageRoot "payload\client"
$Proposal = Join-Path $PackageRoot "symbols\GOnline-54BE1568-00C97000.capsule_currency_fix.symbol-proposal.ini"
$ManifestPath = Join-Path $PackageRoot "manifest.json"
$HashListPath = Join-Path $PackageRoot "SHA256SUMS.txt"
$SymbolName = "capsule.activity.load_items"
$ModId = "capsule_currency_fix"
$ProductName = "SDGO mod framework"

Add-Type -Namespace CapsuleCurrencyInstall -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
'@
Add-Type -AssemblyName Microsoft.VisualBasic

function Assert-PathInside([string]$Root, [string]$Path) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $pathFull = [IO.Path]::GetFullPath($Path)
    if (-not $pathFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "path escapes package root: $Path"
    }
    $pathFull
}

function Assert-PackageIntegrity {
    if (-not (Test-Path -LiteralPath $HashListPath)) { throw "missing SHA256SUMS.txt" }
    foreach ($line in [IO.File]::ReadAllLines($HashListPath)) {
        if (-not $line.Trim()) { continue }
        $match = [regex]::Match($line, '^([0-9A-Fa-f]{64})\s+\*(.+)$')
        if (-not $match.Success) { throw "bad SHA256SUMS line: $line" }
        $relative = $match.Groups[2].Value.Replace('/', '\')
        $file = Assert-PathInside $PackageRoot (Join-Path $PackageRoot $relative)
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "package file is missing: $relative"
        }
        $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
        if ($actual -ine $match.Groups[1].Value) {
            throw "package hash mismatch: $relative"
        }
    }
}

function Get-HostKey([string]$Exe) {
    $stream = [IO.File]::OpenRead($Exe)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        $stream.Position = 0x3C
        $nt = $reader.ReadInt32()
        $stream.Position = $nt
        if ($reader.ReadUInt32() -ne 0x4550) { throw "$Exe is not a Windows executable" }
        $stream.Position = $nt + 8
        $timestamp = $reader.ReadUInt32()
        $stream.Position = $nt + 0x18 + 0x38
        $size = $reader.ReadUInt32()
    } finally {
        $stream.Close()
    }
    "{0}-{1:X8}-{2:X8}" -f [IO.Path]::GetFileNameWithoutExtension($Exe), $timestamp, $size
}

function Get-IniValue([string]$File, [string]$Section, [string]$Key) {
    if (-not (Test-Path -LiteralPath $File)) { return "" }
    $current = ""
    foreach ($line in [IO.File]::ReadAllLines($File)) {
        $text = $line.Trim()
        if (-not $text -or $text.StartsWith(";") -or $text.StartsWith("#")) { continue }
        if ($text.StartsWith("[") -and $text.EndsWith("]")) {
            $current = $text.Substring(1, $text.Length - 2).Trim()
            continue
        }
        if ($current -ine $Section) { continue }
        $at = $text.IndexOf("=")
        if ($at -gt 0 -and $text.Substring(0, $at).Trim() -ieq $Key) {
            return $text.Substring($at + 1).Trim()
        }
    }
    ""
}

function Get-SectionRange([string[]]$Lines, [string]$Section) {
    $start = -1
    $end = $Lines.Count
    for ($index = 0; $index -lt $Lines.Count; ++$index) {
        $text = $Lines[$index].Trim()
        if (-not ($text.StartsWith("[") -and $text.EndsWith("]"))) { continue }
        $name = $text.Substring(1, $text.Length - 2).Trim()
        if ($start -lt 0) {
            if ($name -ieq $Section) { $start = $index }
        } else {
            $end = $index
            break
        }
    }
    @{ Start = $start; End = $end }
}

function Write-LinesAtomically([string]$Path, [System.Collections.Generic.List[string]]$Lines) {
    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $temporary = Join-Path $directory (".capsule_currency_fix.{0}.{1}.tmp" -f $PID, [guid]::NewGuid().ToString("N"))
    try {
        [IO.File]::WriteAllLines($temporary, $Lines, [Text.Encoding]::ASCII)
        if (Test-Path -LiteralPath $Path) {
            # PowerShell 5.1 coerces a null string to an empty filename here.
            $previous = $temporary + '.previous'
            [IO.File]::Replace($temporary, $Path, $previous, $true)
            Remove-ToRecycleBin $previous
        } else {
            [IO.File]::Move($temporary, $Path)
        }
    } finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

function Merge-SymbolSection([string]$Destination, [string]$Source, [string]$Section) {
    $sourceLines = [IO.File]::ReadAllLines($Source)
    $sourceRange = Get-SectionRange $sourceLines $Section
    if ($sourceRange.Start -lt 0) { throw "proposal has no [$Section] section" }

    if (-not (Test-Path -LiteralPath $Destination)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
        Copy-Item -LiteralPath $Source -Destination $Destination
        return "created"
    }

    $destinationLines = [IO.File]::ReadAllLines($Destination)
    $destinationRange = Get-SectionRange $destinationLines $Section
    $output = New-Object 'System.Collections.Generic.List[string]'

    if ($destinationRange.Start -lt 0) {
        foreach ($line in $destinationLines) { $output.Add($line) }
        if ($output.Count -gt 0 -and $output[$output.Count - 1].Trim()) { $output.Add("") }
        for ($index = $sourceRange.Start; $index -lt $sourceRange.End; ++$index) {
            $output.Add($sourceLines[$index])
        }
        Write-LinesAtomically $Destination $output
        return "added"
    }

    for ($index = 0; $index -lt $destinationRange.Start; ++$index) {
        $output.Add($destinationLines[$index])
    }
    for ($index = $sourceRange.Start; $index -lt $sourceRange.End; ++$index) {
        $output.Add($sourceLines[$index])
    }
    for ($index = $destinationRange.End; $index -lt $destinationLines.Count; ++$index) {
        $output.Add($destinationLines[$index])
    }
    Write-LinesAtomically $Destination $output
    "updated"
}

function Remove-SymbolSection([string]$Destination, [string]$Section) {
    if (-not (Test-Path -LiteralPath $Destination)) { return $false }
    $lines = [IO.File]::ReadAllLines($Destination)
    $range = Get-SectionRange $lines $Section
    if ($range.Start -lt 0) { return $false }
    $output = New-Object 'System.Collections.Generic.List[string]'
    for ($index = 0; $index -lt $range.Start; ++$index) { $output.Add($lines[$index]) }
    for ($index = $range.End; $index -lt $lines.Count; ++$index) { $output.Add($lines[$index]) }
    Write-LinesAtomically $Destination $output
    $true
}

function Test-OurLoader([string]$File) {
    if ((Get-Item -LiteralPath $File).VersionInfo.ProductName -eq $ProductName) { return $true }
    $bytes = [IO.File]::ReadAllBytes($File)
    [Text.Encoding]::Unicode.GetString($bytes).Contains("sdgo\core.dll")
}

function Remove-ToRecycleBin([string]$Path) {
    if (Test-Path -LiteralPath $Path -PathType Container) {
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory(
            $Path, 'OnlyErrorDialogs', 'SendToRecycleBin')
    } elseif (Test-Path -LiteralPath $Path) {
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile(
            $Path, 'OnlyErrorDialogs', 'SendToRecycleBin')
    }
}

Assert-PackageIntegrity
$manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
$game = (Resolve-Path -LiteralPath $GameDir).Path
$exe = Join-Path $game "GOnline.exe"
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "$game does not contain GOnline.exe"
}

$running = Get-CimInstance Win32_Process -Filter "Name='GOnline.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -eq $exe }
if ($running) { throw "GOnline.exe in $game is running; close it before changing files" }

$hostKey = Get-HostKey $exe
$exeHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
if ($hostKey -ine $manifest.host.key -or $exeHash -ine $manifest.host.sha256) {
    throw "unsupported client: host=$hostKey sha256=$exeHash"
}

$sdgo = Join-Path $game "sdgo"
$symbolFile = Join-Path $sdgo "symbols\$hostKey.ini"
$modDirectory = Join-Path $sdgo "mods\$ModId"

if ($Uninstall) {
    $removedSymbol = Remove-SymbolSection $symbolFile $SymbolName
    if (Test-Path -LiteralPath $modDirectory) {
        Remove-ToRecycleBin $modDirectory
        Write-Host "recycled sdgo\mods\$ModId"
    } else {
        Write-Host "mod directory was already absent"
    }
    if ($removedSymbol) {
        Write-Host "removed  [$SymbolName] and preserved all other symbol sections"
    } else {
        Write-Host "symbol section was already absent"
    }
    Write-Host "framework files and every other mod were left in place"
    return
}

if (Test-Path -LiteralPath $symbolFile) {
    $timestamp = Get-IniValue $symbolFile "host" "timestamp"
    $imageSize = Get-IniValue $symbolFile "host" "size_of_image"
    if ([Convert]::ToUInt32(($timestamp -replace '^0x', ''), 16) -ne 0x54BE1568 -or
        [Convert]::ToUInt32(($imageSize -replace '^0x', ''), 16) -ne 0x00C97000) {
        throw "existing symbol file [host] does not describe $hostKey"
    }
}

$existingCore = Join-Path $sdgo "core.dll"
if ((Test-Path -LiteralPath $existingCore) -and
    (Get-Item -LiteralPath $existingCore).VersionInfo.ProductName -ne $ProductName) {
    throw 'Existing sdgo/core.dll is not the supported framework; refusing replacement'
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$backup = Join-Path $sdgo "backups\$ModId\$stamp"
function Backup-Existing([string]$Path, [string]$Relative) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $destination = Join-Path $backup $Relative
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $Path -Destination $destination -Force
}

$loaderSource = Join-Path $Payload "d3d9.dll"
$loaderDestination = Join-Path $game "d3d9.dll"
$chainDestination = Join-Path $game "d3d9_chain.dll"
$chainExisting = $false
if (Test-Path -LiteralPath $loaderDestination) {
    if (Test-OurLoader $loaderDestination) {
        if ((Get-FileHash $loaderDestination -Algorithm SHA256).Hash -ine
            (Get-FileHash $loaderSource -Algorithm SHA256).Hash) {
            Backup-Existing $loaderDestination "d3d9.dll"
        }
    } else {
        if (Test-Path -LiteralPath $chainDestination) {
            throw "both a foreign d3d9.dll and d3d9_chain.dll exist; resolve them manually first"
        }
        $previousChain = Get-IniValue (Join-Path $sdgo 'sdgo.ini') 'loader' 'chain_d3d9'
        if ($previousChain -and $previousChain -ine 'd3d9_chain.dll') {
            throw 'An existing D3D9 chain setting conflicts; refusing to replace it'
        }
        Move-Item -LiteralPath $loaderDestination -Destination $chainDestination
        $chainExisting = $true
        Write-Host "kept foreign d3d9.dll as d3d9_chain.dll"
    }
}
Copy-Item -LiteralPath $loaderSource -Destination $loaderDestination -Force

$coreSource = Join-Path $Payload "sdgo\core.dll"
$coreDestination = Join-Path $sdgo "core.dll"
if (Test-Path -LiteralPath $coreDestination) {
    if ((Get-FileHash $coreDestination -Algorithm SHA256).Hash -ine
        (Get-FileHash $coreSource -Algorithm SHA256).Hash) {
        Backup-Existing $coreDestination "sdgo\core.dll"
    }
}
New-Item -ItemType Directory -Force -Path $sdgo | Out-Null
Copy-Item -LiteralPath $coreSource -Destination $coreDestination -Force

$iniSource = Join-Path $Payload "sdgo\sdgo.ini"
$iniDestination = Join-Path $sdgo "sdgo.ini"
if (-not (Test-Path -LiteralPath $iniDestination)) {
    Copy-Item -LiteralPath $iniSource -Destination $iniDestination
} else {
    Write-Host "kept     sdgo\sdgo.ini"
}
if ($chainExisting) {
    [void][CapsuleCurrencyInstall.Ini]::WritePrivateProfileString(
        "loader", "chain_d3d9", "d3d9_chain.dll", $iniDestination)
}

$oldEnabled = Get-IniValue (Join-Path $modDirectory "mod.ini") "mod" "enabled"
if (Test-Path -LiteralPath $modDirectory) {
    Backup-Existing $modDirectory "sdgo\mods\$ModId"
}
New-Item -ItemType Directory -Force -Path $modDirectory | Out-Null
Copy-Item -LiteralPath (Join-Path $Payload "sdgo\mods\$ModId\capsule_currency_fix.dll") `
    -Destination (Join-Path $modDirectory "capsule_currency_fix.dll") -Force
Copy-Item -LiteralPath (Join-Path $Payload "sdgo\mods\$ModId\mod.ini") `
    -Destination (Join-Path $modDirectory "mod.ini") -Force
if ($oldEnabled) {
    [void][CapsuleCurrencyInstall.Ini]::WritePrivateProfileString(
        "mod", "enabled", $oldEnabled, (Join-Path $modDirectory "mod.ini"))
}

if (Test-Path -LiteralPath $symbolFile) {
    Backup-Existing $symbolFile "sdgo\symbols\$hostKey.ini"
}
$symbolAction = Merge-SymbolSection $symbolFile $Proposal $SymbolName

Write-Host "installed capsule_currency_fix for $hostKey"
Write-Host "$symbolAction [$SymbolName]; unrelated symbols and mods were preserved"
if (Test-Path -LiteralPath $backup) {
    Write-Host "changed-file backup: $backup"
}
Write-Host "start the client manually and inspect sdgo\logs\sdgo.log"
