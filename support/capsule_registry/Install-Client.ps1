<#
.SYNOPSIS
  Installs a manifest-verified SDGO client mod package without Python.

.DESCRIPTION
  The package controls only files listed in package-manifest.json. Existing
  sdgo.ini, unrelated mods and unrelated symbol sections are retained.

  Expected package layout:

    Install-Client.ps1
    package-manifest.json
    symbol-fragment.ini
    client\
      d3d9.dll                         (optional on framework updates)
      sdgo\core.dll                    (optional on mod-only updates)
      sdgo\sdgo.ini                    (used only when the target has none)
      sdgo\mods\<id>\...

  package-manifest.json uses SHA-256 values:

    {
      "schema_version": 1,
      "package_id": "capsule-catalog",
      "symbol_fragment_sha256": "<64 hex>",
      "files": {
        "d3d9.dll": "<64 hex>",
        "sdgo/core.dll": "<64 hex>",
        "sdgo/mods/capsule_catalog/mod.ini": "<64 hex>"
      }
    }

  -CheckOnly performs every source, host, process, symbol and chaining check but
  does not create a backup or write a target file.
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$SourceClient,
    [string]$SymbolFragment,
    [string]$PackageManifest,
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$ScriptPath = $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($ScriptPath)) {
    throw 'The installer must be run from a saved Install-Client.ps1 file'
}
$ScriptDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($ScriptPath))
if ([string]::IsNullOrWhiteSpace($SourceClient)) {
    $SourceClient = Join-Path $ScriptDirectory 'client'
}
if ([string]::IsNullOrWhiteSpace($SymbolFragment)) {
    $SymbolFragment = Join-Path $ScriptDirectory 'symbol-fragment.ini'
}
if ([string]::IsNullOrWhiteSpace($PackageManifest)) {
    $PackageManifest = Join-Path $ScriptDirectory 'package-manifest.json'
}
$ExpectedTimestamp = [uint32]0x54BE1568
$ExpectedImageSize = [uint32]0x00C97000
$HostKey = 'GOnline-54BE1568-00C97000'
$FrameworkProduct = 'SDGO mod framework'
$LoaderRelative = 'd3d9.dll'
$CoreRelative = 'sdgo\core.dll'
$GlobalIniRelative = 'sdgo\sdgo.ini'
$SymbolRelative = "sdgo\symbols\$HostKey.ini"

function Select-GameDirectory {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Select the SDGO client folder that contains GOnline.exe'
    $dialog.ShowNewFolderButton = $false
    try {
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
            throw 'No client directory was selected'
        }
        return $dialog.SelectedPath
    } finally {
        $dialog.Dispose()
    }
}

function Get-AbsoluteExistingPath([string]$Path, [string]$Label, [bool]$Leaf) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Label is required"
    }
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    if (@($resolved).Count -ne 1) {
        throw "$Label must resolve to exactly one path"
    }
    $item = Get-Item -LiteralPath $resolved.Path -Force
    if ($Leaf -and $item.PSIsContainer) {
        throw "$Label must be a file: $($item.FullName)"
    }
    if (-not $Leaf -and -not $item.PSIsContainer) {
        throw "$Label must be a directory: $($item.FullName)"
    }
    return $item.FullName
}

function Test-PathInside([string]$Root, [string]$Candidate, [switch]$AllowRoot) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $candidateFull = [IO.Path]::GetFullPath($Candidate)
    if ($AllowRoot -and $candidateFull.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    return $candidateFull.StartsWith(
        $rootFull + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    )
}

function Assert-NoReparseAncestors([string]$Path, [string]$Label) {
    $current = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrWhiteSpace($current)) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "$Label uses a linked path: $current"
            }
        }
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent -or $parent.FullName.Equals($current, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $current = $parent.FullName
    }
}

function Assert-NoReparsePath([string]$Root, [string]$Candidate, [string]$Label) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $candidateFull = [IO.Path]::GetFullPath($Candidate)
    if (-not (Test-PathInside $rootFull $candidateFull -AllowRoot)) {
        throw "$Label escapes its root: $candidateFull"
    }
    $current = $rootFull
    if (Test-Path -LiteralPath $current) {
        $rootItem = Get-Item -LiteralPath $current -Force
        if ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "$Label uses a linked root: $current"
        }
    }
    if ($candidateFull.Length -eq $rootFull.Length) {
        return
    }
    $suffix = $candidateFull.Substring($rootFull.Length).TrimStart('\', '/')
    foreach ($part in $suffix.Split(@('\', '/'), [StringSplitOptions]::RemoveEmptyEntries)) {
        $current = Join-Path $current $part
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "$Label contains a linked path: $current"
            }
        }
    }
}

function Convert-SafeRelativePath([string]$Relative) {
    if ([string]::IsNullOrWhiteSpace($Relative)) {
        throw 'Package manifest contains an empty file path'
    }
    $normalized = $Relative.Replace('/', '\')
    if (
        [IO.Path]::IsPathRooted($normalized) -or
        $normalized.StartsWith('\') -or
        $normalized.Contains(':')
    ) {
        throw "Package file path must be relative: $Relative"
    }
    $parts = @($normalized.Split('\'))
    if (
        $parts.Count -eq 0 -or
        @($parts | Where-Object { $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' }).Count -gt 0
    ) {
        throw "Unsafe package file path: $Relative"
    }
    $joined = [string]::Join('\', $parts)
    if (
        $joined.StartsWith('sdgo\backups\', [StringComparison]::OrdinalIgnoreCase) -or
        $joined.StartsWith('sdgo\install-receipts\', [StringComparison]::OrdinalIgnoreCase)
    ) {
        throw "Package cannot write installer state: $Relative"
    }
    $isModFile = (
        $parts.Count -ge 4 -and
        $parts[0].Equals('sdgo', [StringComparison]::OrdinalIgnoreCase) -and
        $parts[1].Equals('mods', [StringComparison]::OrdinalIgnoreCase)
    )
    $isSupportedFrameworkFile = @(
        'd3d9.dll',
        'sdgo\core.dll',
        'sdgo\sdgo.ini',
        "sdgo\symbols\$HostKey.ini"
    ) | Where-Object { $_.Equals($joined, [StringComparison]::OrdinalIgnoreCase) }
    if (-not $isModFile -and -not $isSupportedFrameworkFile) {
        throw "Package file is outside the supported client install scope: $Relative"
    }
    return $joined
}

function Get-Sha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Read-PeHost([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        if ($stream.Length -lt 256) {
            throw "Invalid PE target: $Path"
        }
        $reader = New-Object IO.BinaryReader($stream)
        $stream.Position = 0x3c
        $nt = $reader.ReadInt32()
        if ($nt -lt 0x40 -or $nt + 0x58 -gt $stream.Length) {
            throw "Invalid PE target: $Path"
        }
        $stream.Position = $nt
        if ($reader.ReadUInt32() -ne 0x4550) {
            throw "Invalid PE signature: $Path"
        }
        $stream.Position = $nt + 8
        $timestamp = $reader.ReadUInt32()
        $stream.Position = $nt + 0x18 + 0x38
        $imageSize = $reader.ReadUInt32()
        return [pscustomobject]@{
            Timestamp = $timestamp
            ImageSize = $imageSize
        }
    } finally {
        $stream.Dispose()
    }
}

function Assert-TargetClosed([string]$Executable) {
    $expected = [IO.Path]::GetFullPath($Executable)
    $unknownPath = $false
    try {
        $processes = @(Get-CimInstance Win32_Process -Filter "Name='GOnline.exe'")
        foreach ($process in $processes) {
            if ([string]::IsNullOrWhiteSpace($process.ExecutablePath)) {
                $unknownPath = $true
                continue
            }
            $actual = [IO.Path]::GetFullPath($process.ExecutablePath)
            if ($actual.Equals($expected, [StringComparison]::OrdinalIgnoreCase)) {
                throw "This GOnline.exe is running; close it first. No process was stopped: $expected"
            }
        }
    } catch {
        if ($_.Exception.Message -like 'This GOnline.exe is running*') {
            throw
        }
        $unknownPath = $true
    }
    if ($unknownPath) {
        try {
            $probe = [IO.File]::Open(
                $expected,
                [IO.FileMode]::Open,
                [IO.FileAccess]::Write,
                [IO.FileShare]7
            )
            $probe.Dispose()
        } catch {
            throw "Cannot establish that this target is closed; no process was stopped: $expected"
        }
    }
}

function Test-FrameworkBinary([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    return (Get-Item -LiteralPath $Path -Force).VersionInfo.ProductName -eq $FrameworkProduct
}

function Get-TextEncoding([byte[]]$Bytes) {
    if ($Bytes.Length -ge 3 -and $Bytes[0] -eq 0xef -and $Bytes[1] -eq 0xbb -and $Bytes[2] -eq 0xbf) {
        return [pscustomobject]@{
            Encoding = New-Object Text.UTF8Encoding($true)
            Offset = 3
        }
    }
    if ($Bytes.Length -ge 2 -and $Bytes[0] -eq 0xff -and $Bytes[1] -eq 0xfe) {
        return [pscustomobject]@{
            Encoding = New-Object Text.UnicodeEncoding($false, $true)
            Offset = 2
        }
    }
    if ($Bytes.Length -ge 2 -and $Bytes[0] -eq 0xfe -and $Bytes[1] -eq 0xff) {
        return [pscustomobject]@{
            Encoding = New-Object Text.UnicodeEncoding($true, $true)
            Offset = 2
        }
    }
    return [pscustomobject]@{
        Encoding = New-Object Text.UTF8Encoding($false)
        Offset = 0
    }
}

function Read-TextDocument([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $format = Get-TextEncoding $bytes
    return [pscustomobject]@{
        Text = $format.Encoding.GetString($bytes, $format.Offset, $bytes.Length - $format.Offset)
        Encoding = $format.Encoding
    }
}

function Parse-Ini([string]$Text, [string]$Label) {
    $sections = [ordered]@{}
    $current = $null
    $lineNumber = 0
    foreach ($line in [regex]::Split($Text, "\r\n|\n|\r")) {
        $lineNumber++
        $trimmed = $line.Trim()
        if ($trimmed -eq '' -or $trimmed.StartsWith(';') -or $trimmed.StartsWith('#')) {
            continue
        }
        if ($trimmed -match '^\[([^\]]+)\]$') {
            $name = $Matches[1].Trim()
            $existingName = $sections.Keys | Where-Object { $_.Equals($name, [StringComparison]::OrdinalIgnoreCase) }
            if ($existingName) {
                throw "$Label contains duplicate section [$name]"
            }
            $sections[$name] = [ordered]@{}
            $current = $name
            continue
        }
        $at = $trimmed.IndexOf('=')
        if ($null -eq $current -or $at -le 0) {
            throw "$Label contains an invalid INI line $lineNumber"
        }
        $key = $trimmed.Substring(0, $at).Trim()
        $value = $trimmed.Substring($at + 1).Trim()
        $existingKey = $sections[$current].Keys | Where-Object { $_.Equals($key, [StringComparison]::OrdinalIgnoreCase) }
        if ($existingKey) {
            throw "$Label contains duplicate key [$current] $key"
        }
        $sections[$current][$key] = $value
    }
    return ,$sections
}

function Find-IniSection($Sections, [string]$Name) {
    foreach ($key in $Sections.Keys) {
        if ($key.Equals($Name, [StringComparison]::OrdinalIgnoreCase)) {
            return $key
        }
    }
    return $null
}

function Find-IniValue($Section, [string]$Key) {
    foreach ($name in $Section.Keys) {
        if ($name.Equals($Key, [StringComparison]::OrdinalIgnoreCase)) {
            return $Section[$name]
        }
    }
    return $null
}

function Convert-HexUInt32([string]$Value, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "$Label is missing"
    }
    $text = $Value.Trim()
    if ($text.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) {
        $text = $text.Substring(2)
    }
    if ($text -notmatch '^[0-9a-fA-F]{1,8}$') {
        throw "$Label is not a 32-bit hexadecimal value"
    }
    return [Convert]::ToUInt32($text, 16)
}

function Render-IniSections($Sections, [string]$NewLine) {
    $builder = New-Object Text.StringBuilder
    foreach ($section in $Sections.Keys) {
        [void]$builder.Append('[').Append($section).Append(']').Append($NewLine)
        foreach ($key in $Sections[$section].Keys) {
            [void]$builder.Append($key).Append('=').Append($Sections[$section][$key]).Append($NewLine)
        }
        [void]$builder.Append($NewLine)
    }
    return $builder.ToString()
}

function Merge-SymbolText([string]$ExistingText, $FragmentSections) {
    $newline = if ($ExistingText.Contains("`r`n")) { "`r`n" } else { "`n" }
    if ([string]::IsNullOrEmpty($ExistingText)) {
        return Render-IniSections $FragmentSections $newline
    }
    $existingSections = Parse-Ini $ExistingText 'Existing symbol file'
    $existingHostName = Find-IniSection $existingSections 'host'
    if ($null -eq $existingHostName) {
        throw 'Existing symbol file has no [host] section'
    }
    foreach ($pair in @(
        @('timestamp', $ExpectedTimestamp),
        @('size_of_image', $ExpectedImageSize)
    )) {
        $actual = Convert-HexUInt32 (Find-IniValue $existingSections[$existingHostName] $pair[0]) "Existing [host] $($pair[0])"
        if ($actual -ne $pair[1]) {
            throw 'Existing symbol file belongs to a different host'
        }
    }

    $replace = @{}
    $appendExact = @{}
    foreach ($section in $FragmentSections.Keys) {
        if ($section.Equals('host', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $lower = $section.ToLowerInvariant()
        if ($lower -eq 'grid.data' -or $lower -eq 'grid.find') {
            $existingName = Find-IniSection $existingSections $section
            if ($null -ne $existingName) {
                if ($existingSections[$existingName].Count -ne $FragmentSections[$section].Count) {
                    throw "Shared symbol conflict: [$section]"
                }
                foreach ($key in $FragmentSections[$section].Keys) {
                    $actual = Find-IniValue $existingSections[$existingName] $key
                    if ($null -eq $actual -or $actual -cne $FragmentSections[$section][$key]) {
                        throw "Shared symbol conflict: [$section] $key"
                    }
                }
            } else {
                $appendExact[$lower] = $section
            }
        } else {
            $replace[$lower] = $section
        }
    }

    $lines = [regex]::Split($ExistingText, "\r\n|\n|\r")
    $output = New-Object Collections.Generic.List[string]
    $seen = @{}
    $skip = $false
    foreach ($line in $lines) {
        $trimmed = $line.Trim()
        if ($trimmed -match '^\[([^\]]+)\]$') {
            $sectionName = $Matches[1].Trim()
            $key = $sectionName.ToLowerInvariant()
            $skip = $replace.ContainsKey($key)
            if ($skip -and -not $seen.ContainsKey($key)) {
                $sourceName = $replace[$key]
                $output.Add("[$sourceName]")
                foreach ($entry in $FragmentSections[$sourceName].Keys) {
                    $output.Add("$entry=$($FragmentSections[$sourceName][$entry])")
                }
                $seen[$key] = $true
            }
        }
        if (-not $skip) {
            $output.Add($line)
        }
    }
    foreach ($key in $replace.Keys) {
        if (-not $seen.ContainsKey($key)) {
            if ($output.Count -gt 0 -and $output[$output.Count - 1] -ne '') {
                $output.Add('')
            }
            $sourceName = $replace[$key]
            $output.Add("[$sourceName]")
            foreach ($entry in $FragmentSections[$sourceName].Keys) {
                $output.Add("$entry=$($FragmentSections[$sourceName][$entry])")
            }
        }
    }
    foreach ($key in $appendExact.Keys) {
        if ($output.Count -gt 0 -and $output[$output.Count - 1] -ne '') {
            $output.Add('')
        }
        $sourceName = $appendExact[$key]
        $output.Add("[$sourceName]")
        foreach ($entry in $FragmentSections[$sourceName].Keys) {
            $output.Add("$entry=$($FragmentSections[$sourceName][$entry])")
        }
    }
    return [string]::Join($newline, $output)
}

function Get-IniSetting([string]$Text, [string]$SectionName, [string]$KeyName) {
    if ([string]::IsNullOrEmpty($Text)) {
        return $null
    }
    $sections = Parse-Ini $Text 'INI file'
    $section = Find-IniSection $sections $SectionName
    if ($null -eq $section) {
        return $null
    }
    return Find-IniValue $sections[$section] $KeyName
}

function Set-IniSettingText(
    [string]$Text,
    [string]$SectionName,
    [string]$KeyName,
    [string]$Value
) {
    $newline = if ($Text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $lines = New-Object Collections.Generic.List[string]
    foreach ($line in [regex]::Split($Text, "\r\n|\n|\r")) {
        $lines.Add($line)
    }
    $sectionStart = -1
    $sectionEnd = $lines.Count
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -match '^\[([^\]]+)\]$') {
            $name = $Matches[1].Trim()
            if ($sectionStart -ge 0) {
                $sectionEnd = $i
                break
            }
            if ($name.Equals($SectionName, [StringComparison]::OrdinalIgnoreCase)) {
                $sectionStart = $i
            }
        }
    }
    if ($sectionStart -lt 0) {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -ne '') {
            $lines.Add('')
        }
        $lines.Add("[$SectionName]")
        $lines.Add("$KeyName=$Value")
        return [string]::Join($newline, $lines)
    }
    for ($i = $sectionStart + 1; $i -lt $sectionEnd; $i++) {
        $trimmed = $lines[$i].Trim()
        $at = $trimmed.IndexOf('=')
        if ($at -gt 0) {
            $name = $trimmed.Substring(0, $at).Trim()
            if ($name.Equals($KeyName, [StringComparison]::OrdinalIgnoreCase)) {
                $lines[$i] = "$KeyName=$Value"
                return [string]::Join($newline, $lines)
            }
        }
    }
    $lines.Insert($sectionEnd, "$KeyName=$Value")
    return [string]::Join($newline, $lines)
}

function Write-AtomicBytes([string]$Destination, [byte[]]$Bytes) {
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $temporary = Join-Path $parent ('.' + [IO.Path]::GetFileName($Destination) + '.' + [guid]::NewGuid().ToString('N') + '.pending')
    $replaced = Join-Path $parent ('.' + [IO.Path]::GetFileName($Destination) + '.' + [guid]::NewGuid().ToString('N') + '.replaced')
    try {
        [IO.File]::WriteAllBytes($temporary, $Bytes)
        if (Test-Path -LiteralPath $Destination) {
            [IO.File]::Replace($temporary, $Destination, $replaced, $true)
        } else {
            Move-Item -LiteralPath $temporary -Destination $Destination
        }
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
        if (Test-Path -LiteralPath $replaced) {
            Remove-Item -LiteralPath $replaced -Force
        }
    }
}

function Write-AtomicText([string]$Destination, [string]$Text, [Text.Encoding]$Encoding) {
    $preamble = $Encoding.GetPreamble()
    $body = $Encoding.GetBytes($Text)
    if ($preamble.Length -eq 0) {
        Write-AtomicBytes $Destination $body
        return
    }
    $bytes = New-Object byte[] ($preamble.Length + $body.Length)
    [Array]::Copy($preamble, 0, $bytes, 0, $preamble.Length)
    [Array]::Copy($body, 0, $bytes, $preamble.Length, $body.Length)
    Write-AtomicBytes $Destination $bytes
}

function Copy-AtomicFile([string]$Source, [string]$Destination) {
    Write-AtomicBytes $Destination ([IO.File]::ReadAllBytes($Source))
}

function Add-JournalEntry(
    [Collections.Generic.List[object]]$Journal,
    [string]$GameRoot,
    [string]$BackupRoot,
    [string]$Destination
) {
    foreach ($entry in $Journal) {
        if ($entry.Destination.Equals($Destination, [StringComparison]::OrdinalIgnoreCase)) {
            return
        }
    }
    if (-not (Test-PathInside $GameRoot $Destination)) {
        throw "Backup destination escapes the game directory: $Destination"
    }
    $relative = $Destination.Substring($GameRoot.TrimEnd('\').Length).TrimStart('\')
    $existed = Test-Path -LiteralPath $Destination -PathType Leaf
    $backup = Join-Path $BackupRoot ('before\' + $relative)
    if ($existed) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $backup) | Out-Null
        Copy-Item -LiteralPath $Destination -Destination $backup -Force
    }
    $Journal.Add([pscustomobject]@{
        Destination = $Destination
        Relative = $relative
        Existed = $existed
        Backup = $backup
    })
}

function Restore-Journal(
    [Collections.Generic.List[object]]$Journal,
    [string]$BackupRoot
) {
    for ($i = $Journal.Count - 1; $i -ge 0; $i--) {
        $entry = $Journal[$i]
        if ($entry.Existed) {
            if (
                (Test-Path -LiteralPath $entry.Destination -PathType Leaf) -and
                (Get-Sha256 $entry.Backup) -eq (Get-Sha256 $entry.Destination)
            ) {
                continue
            }
            Copy-AtomicFile $entry.Backup $entry.Destination
        } elseif (Test-Path -LiteralPath $entry.Destination -PathType Leaf) {
            $failedPath = Join-Path $BackupRoot ('failed-new\' + $entry.Relative)
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $failedPath) | Out-Null
            Move-Item -LiteralPath $entry.Destination -Destination $failedPath -Force
        }
    }
}

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    $GameDir = Select-GameDirectory
}

Assert-NoReparseAncestors $GameDir 'GameDir'
Assert-NoReparseAncestors $SourceClient 'SourceClient'
Assert-NoReparseAncestors $PackageManifest 'PackageManifest'
Assert-NoReparseAncestors $SymbolFragment 'SymbolFragment'
$game = Get-AbsoluteExistingPath $GameDir 'GameDir' $false
$source = Get-AbsoluteExistingPath $SourceClient 'SourceClient' $false
$manifestPath = Get-AbsoluteExistingPath $PackageManifest 'PackageManifest' $true
$fragmentPath = Get-AbsoluteExistingPath $SymbolFragment 'SymbolFragment' $true
Assert-NoReparsePath $game $game 'GameDir'
Assert-NoReparsePath $source $source 'SourceClient'
Assert-NoReparsePath (Split-Path -Parent $manifestPath) $manifestPath 'PackageManifest'
Assert-NoReparsePath (Split-Path -Parent $fragmentPath) $fragmentPath 'SymbolFragment'

$exe = Join-Path $game 'GOnline.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "GameDir does not contain GOnline.exe: $game"
}
Assert-NoReparsePath $game $exe 'GOnline.exe'
$targetHost = Read-PeHost $exe
if ($targetHost.Timestamp -ne $ExpectedTimestamp -or $targetHost.ImageSize -ne $ExpectedImageSize) {
    throw ('Unsupported GOnline.exe host: timestamp={0:X8}, size={1:X8}; expected {2:X8}/{3:X8}' -f
        $targetHost.Timestamp, $targetHost.ImageSize, $ExpectedTimestamp, $ExpectedImageSize)
}
Assert-TargetClosed $exe

$manifest = ConvertFrom-Json ([IO.File]::ReadAllText($manifestPath))
if ($manifest.schema_version -ne 1) {
    throw 'Unsupported package manifest schema_version'
}
if ($null -eq $manifest.files) {
    throw 'Package manifest has no files object'
}
$fragmentHash = [string]$manifest.symbol_fragment_sha256
if ($fragmentHash -notmatch '^[0-9a-fA-F]{64}$') {
    throw 'Package manifest has no valid symbol_fragment_sha256'
}
if ((Get-Sha256 $fragmentPath) -ine $fragmentHash) {
    throw 'Symbol fragment checksum mismatch; no files changed'
}

$files = New-Object Collections.Generic.List[object]
$seenPaths = @{}
foreach ($property in $manifest.files.PSObject.Properties) {
    $relative = Convert-SafeRelativePath ([string]$property.Name)
    $key = $relative.ToLowerInvariant()
    if ($seenPaths.ContainsKey($key)) {
        throw "Package manifest contains a duplicate path: $relative"
    }
    $seenPaths[$key] = $true
    $expectedHash = [string]$property.Value
    if ($expectedHash -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Invalid SHA-256 in package manifest: $relative"
    }
    $sourcePath = [IO.Path]::GetFullPath((Join-Path $source $relative))
    if (-not (Test-PathInside $source $sourcePath)) {
        throw "Package file escapes SourceClient: $relative"
    }
    Assert-NoReparsePath $source $sourcePath "Package file $relative"
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw "Package file is missing: $relative"
    }
    if ((Get-Sha256 $sourcePath) -ine $expectedHash) {
        throw "Package checksum mismatch: $relative; no files changed"
    }
    $destination = [IO.Path]::GetFullPath((Join-Path $game $relative))
    if (-not (Test-PathInside $game $destination)) {
        throw "Package destination escapes GameDir: $relative"
    }
    Assert-NoReparsePath $game $destination "Package destination $relative"
    if (Test-Path -LiteralPath $destination -PathType Container) {
        throw "Package destination is an existing directory: $relative"
    }
    $files.Add([pscustomobject]@{
        Relative = $relative
        Source = $sourcePath
        Destination = $destination
        Hash = $expectedHash.ToLowerInvariant()
    })
}
if ($files.Count -eq 0) {
    throw 'Package manifest contains no installable files'
}

$fragmentDocument = Read-TextDocument $fragmentPath
$fragmentSections = Parse-Ini $fragmentDocument.Text 'Symbol fragment'
$fragmentHostName = Find-IniSection $fragmentSections 'host'
if ($null -eq $fragmentHostName) {
    throw 'Symbol fragment has no [host] section'
}
foreach ($pair in @(
    @('timestamp', $ExpectedTimestamp),
    @('size_of_image', $ExpectedImageSize)
)) {
    $value = Convert-HexUInt32 (Find-IniValue $fragmentSections[$fragmentHostName] $pair[0]) "Fragment [host] $($pair[0])"
    if ($value -ne $pair[1]) {
        throw 'Symbol fragment does not match the supported GOnline.exe host'
    }
}
foreach ($section in $fragmentSections.Keys) {
    if (
        -not $section.Equals('host', [StringComparison]::OrdinalIgnoreCase) -and
        -not $section.StartsWith('capsule.', [StringComparison]::OrdinalIgnoreCase) -and
        -not $section.Equals('grid.data', [StringComparison]::OrdinalIgnoreCase) -and
        -not $section.Equals('grid.find', [StringComparison]::OrdinalIgnoreCase)
    ) {
        throw "Symbol fragment may own only capsule.* and exact grid dependencies: [$section]"
    }
}
if ($fragmentSections.Count -lt 2) {
    throw 'Symbol fragment has no capsule.* section'
}

$packageProfileEntry = $files | Where-Object {
    $_.Relative.Equals($SymbolRelative, [StringComparison]::OrdinalIgnoreCase)
}
if (-not $packageProfileEntry) {
    throw "Package client is missing the complete $SymbolRelative profile"
}
$packageProfileDocument = Read-TextDocument $packageProfileEntry.Source
$packageProfileSections = Parse-Ini $packageProfileDocument.Text 'Package symbol profile'
$packageHostName = Find-IniSection $packageProfileSections 'host'
if ($null -eq $packageHostName) {
    throw 'Package symbol profile has no [host] section'
}
foreach ($pair in @(
    @('timestamp', $ExpectedTimestamp),
    @('size_of_image', $ExpectedImageSize)
)) {
    $value = Convert-HexUInt32 (Find-IniValue $packageProfileSections[$packageHostName] $pair[0]) "Package profile [host] $($pair[0])"
    if ($value -ne $pair[1]) {
        throw 'Package symbol profile does not match the supported GOnline.exe host'
    }
}
$mergeSections = [ordered]@{}
$mergeSections['host'] = $packageProfileSections[$packageHostName]
foreach ($sharedName in @('grid.data', 'grid.find')) {
    $profileName = Find-IniSection $packageProfileSections $sharedName
    if ($null -eq $profileName) {
        throw "Package symbol profile is missing [$sharedName]"
    }
    $fragmentName = Find-IniSection $fragmentSections $sharedName
    if ($null -ne $fragmentName) {
        if ($fragmentSections[$fragmentName].Count -ne $packageProfileSections[$profileName].Count) {
            throw "Symbol fragment conflicts with package profile [$sharedName]"
        }
        foreach ($key in $packageProfileSections[$profileName].Keys) {
            if ((Find-IniValue $fragmentSections[$fragmentName] $key) -cne $packageProfileSections[$profileName][$key]) {
                throw "Symbol fragment conflicts with package profile [$sharedName] $key"
            }
        }
    }
    $mergeSections[$sharedName] = $packageProfileSections[$profileName]
}
foreach ($section in $fragmentSections.Keys) {
    if ($section.StartsWith('capsule.', [StringComparison]::OrdinalIgnoreCase)) {
        $mergeSections[$section] = $fragmentSections[$section]
    }
}
if (@($mergeSections.Keys | Where-Object { $_.StartsWith('capsule.', [StringComparison]::OrdinalIgnoreCase) }).Count -eq 0) {
    throw 'Symbol fragment has no capsule.* section'
}

$symbolPath = Join-Path $game $SymbolRelative
Assert-NoReparsePath $game $symbolPath 'Target symbol file'
if (Test-Path -LiteralPath $symbolPath -PathType Container) {
    throw 'Target symbol path is an existing directory'
}
$existingSymbol = $null
$symbolEncoding = $packageProfileDocument.Encoding
$symbolBaseText = $packageProfileDocument.Text
if (Test-Path -LiteralPath $symbolPath -PathType Leaf) {
    $existingSymbol = Read-TextDocument $symbolPath
    $symbolEncoding = $existingSymbol.Encoding
    $symbolBaseText = $existingSymbol.Text
}
$mergedSymbols = Merge-SymbolText $symbolBaseText $mergeSections

$sourceLoaderEntry = $files | Where-Object { $_.Relative.Equals($LoaderRelative, [StringComparison]::OrdinalIgnoreCase) }
$sourceCoreEntry = $files | Where-Object { $_.Relative.Equals($CoreRelative, [StringComparison]::OrdinalIgnoreCase) }
$targetLoader = Join-Path $game $LoaderRelative
$targetCore = Join-Path $game $CoreRelative
$targetIni = Join-Path $game $GlobalIniRelative
$chainPath = Join-Path $game 'd3d9_chain.dll'
foreach ($specialPath in @(
    @($targetLoader, 'Target loader'),
    @($targetCore, 'Target framework core'),
    @($targetIni, 'Target framework configuration'),
    @($chainPath, 'Target chained loader')
)) {
    Assert-NoReparsePath $game $specialPath[0] $specialPath[1]
    if (Test-Path -LiteralPath $specialPath[0] -PathType Container) {
        throw "$($specialPath[1]) is an existing directory"
    }
}
$chainRequired = $false
$chainText = $null
$chainEncoding = New-Object Text.UTF8Encoding($false)

if ($sourceCoreEntry -and -not (Test-FrameworkBinary $sourceCoreEntry.Source)) {
    throw "Bundled sdgo\core.dll is not '$FrameworkProduct'"
}
if ($sourceLoaderEntry) {
    if (-not (Test-FrameworkBinary $sourceLoaderEntry.Source)) {
        throw "Bundled d3d9.dll is not '$FrameworkProduct'"
    }
    if (-not $sourceCoreEntry -and -not (Test-FrameworkBinary $targetCore)) {
        throw 'A package that introduces the framework loader must also provide sdgo\core.dll'
    }
    if (Test-Path -LiteralPath $targetLoader -PathType Leaf) {
        if (-not (Test-FrameworkBinary $targetLoader)) {
            $chainRequired = $true
            if (Test-Path -LiteralPath $targetIni -PathType Leaf) {
                $iniDocument = Read-TextDocument $targetIni
                $chainText = $iniDocument.Text
                $chainEncoding = $iniDocument.Encoding
                $configuredChain = Get-IniSetting $chainText 'loader' 'chain_d3d9'
                if (
                    -not [string]::IsNullOrWhiteSpace($configuredChain) -and
                    -not $configuredChain.Equals('d3d9_chain.dll', [StringComparison]::OrdinalIgnoreCase)
                ) {
                    throw "Existing loader chain uses '$configuredChain'; refusing to guess a deeper chain"
                }
            } else {
                $sourceIniEntry = $files | Where-Object {
                    $_.Relative.Equals($GlobalIniRelative, [StringComparison]::OrdinalIgnoreCase)
                }
                if ($sourceIniEntry) {
                    $iniDocument = Read-TextDocument $sourceIniEntry.Source
                    $chainText = $iniDocument.Text
                    $chainEncoding = $iniDocument.Encoding
                } else {
                    $chainText = ''
                }
            }
            if (Test-Path -LiteralPath $chainPath -PathType Leaf) {
                if ((Get-Sha256 $chainPath) -ne (Get-Sha256 $targetLoader)) {
                    throw 'A different d3d9_chain.dll already exists; no files changed'
                }
            }
            $chainText = Set-IniSettingText $chainText 'loader' 'chain_d3d9' 'd3d9_chain.dll'
        }
    }
} else {
    if (
        -not (Test-FrameworkBinary $targetLoader) -or
        -not (Test-FrameworkBinary $targetCore)
    ) {
        throw 'This mod-only package requires an existing SDGO mod framework'
    }
}

foreach ($entry in $files | Where-Object { $_.Relative.EndsWith('\mod.ini', [StringComparison]::OrdinalIgnoreCase) }) {
    $modText = (Read-TextDocument $entry.Source).Text
    $modSections = Parse-Ini $modText "Package $($entry.Relative)"
    $modName = Find-IniSection $modSections 'mod'
    if ($null -eq $modName) {
        throw "Package mod.ini has no [mod] section: $($entry.Relative)"
    }
}

$planned = New-Object Collections.Generic.List[object]
foreach ($entry in $files) {
    if ($entry.Relative.Equals($SymbolRelative, [StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    if ($entry.Relative.Equals($GlobalIniRelative, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $entry.Destination -PathType Leaf)) {
        continue
    }
    $planned.Add($entry)
}

$installId = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$packageId = [string]$manifest.package_id
if (-not [string]::IsNullOrWhiteSpace($packageId)) {
    $safePackageId = [regex]::Replace($packageId, '[^A-Za-z0-9._-]', '-').Trim('-', '.')
    if ($safePackageId) {
        $installId = $safePackageId + '-' + $installId
    }
}
$backupRoot = Join-Path $game "sdgo\backups\$installId"
Assert-NoReparsePath $game $backupRoot 'Backup path'

if ($CheckOnly) {
    [pscustomobject]@{
        ok = $true
        check_only = $true
        host = $HostKey
        package_id = $packageId
        files = $planned.Count
        symbol_sections = @($mergeSections.Keys | Where-Object { $_ -ne 'host' }).Count
        preserves_existing_sdgo_ini = (Test-Path -LiteralPath $targetIni -PathType Leaf)
        third_party_loader_chain = $chainRequired
        source_client = $source
        game_dir = $game
    } | ConvertTo-Json -Compress
    return
}

$journal = New-Object Collections.Generic.List[object]
$installedFiles = New-Object Collections.Generic.List[string]
try {
    New-Item -ItemType Directory -Force -Path (Join-Path $backupRoot 'before') | Out-Null

    foreach ($entry in $planned) {
        Add-JournalEntry $journal $game $backupRoot $entry.Destination
    }
    Add-JournalEntry $journal $game $backupRoot $symbolPath
    if ($chainRequired) {
        Add-JournalEntry $journal $game $backupRoot $chainPath
        Add-JournalEntry $journal $game $backupRoot $targetIni
    }

    $ordinary = @($planned | Where-Object {
        -not $_.Relative.Equals($LoaderRelative, [StringComparison]::OrdinalIgnoreCase) -and
        -not $_.Relative.EndsWith('\mod.ini', [StringComparison]::OrdinalIgnoreCase)
    } | Sort-Object Relative)
    $modInis = @($planned | Where-Object {
        $_.Relative.EndsWith('\mod.ini', [StringComparison]::OrdinalIgnoreCase)
    } | Sort-Object Relative)
    $rootLoaders = @($planned | Where-Object {
        $_.Relative.Equals($LoaderRelative, [StringComparison]::OrdinalIgnoreCase)
    })

    foreach ($entry in $ordinary) {
        Copy-AtomicFile $entry.Source $entry.Destination
        if ((Get-Sha256 $entry.Destination) -ne $entry.Hash) {
            throw "Installed file verification failed: $($entry.Relative)"
        }
        $installedFiles.Add($entry.Relative)
    }

    Write-AtomicText $symbolPath $mergedSymbols $symbolEncoding
    $readbackSymbols = (Read-TextDocument $symbolPath).Text
    $readbackSections = Parse-Ini $readbackSymbols 'Installed symbol file'
    foreach ($section in $mergeSections.Keys) {
        if ($section.Equals('host', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $actualName = Find-IniSection $readbackSections $section
        if ($null -eq $actualName) {
            throw "Installed symbol section is missing: [$section]"
        }
        foreach ($key in $mergeSections[$section].Keys) {
            if ((Find-IniValue $readbackSections[$actualName] $key) -cne $mergeSections[$section][$key]) {
                throw "Installed symbol value mismatch: [$section] $key"
            }
        }
    }
    $installedFiles.Add($SymbolRelative)

    if ($chainRequired) {
        if (-not (Test-Path -LiteralPath $chainPath -PathType Leaf)) {
            Copy-AtomicFile $targetLoader $chainPath
        }
        if ((Get-Sha256 $chainPath) -ne (Get-Sha256 $targetLoader)) {
            throw 'Third-party d3d9 chain verification failed'
        }
        Write-AtomicText $targetIni $chainText $chainEncoding
        if ((Get-IniSetting (Read-TextDocument $targetIni).Text 'loader' 'chain_d3d9') -ne 'd3d9_chain.dll') {
            throw 'sdgo.ini loader chain verification failed'
        }
        $installedFiles.Add('d3d9_chain.dll')
        $installedFiles.Add($GlobalIniRelative)
    }

    foreach ($entry in $modInis) {
        Copy-AtomicFile $entry.Source $entry.Destination
        if ((Get-Sha256 $entry.Destination) -ne $entry.Hash) {
            throw "Installed mod manifest verification failed: $($entry.Relative)"
        }
        $installedMod = Read-TextDocument $entry.Destination
        $enabledMod = Set-IniSettingText $installedMod.Text 'mod' 'enabled' '1'
        Write-AtomicText $entry.Destination $enabledMod $installedMod.Encoding
        $enabledSections = Parse-Ini (Read-TextDocument $entry.Destination).Text "Installed $($entry.Relative)"
        $enabledName = Find-IniSection $enabledSections 'mod'
        if ($null -eq $enabledName -or (Find-IniValue $enabledSections[$enabledName] 'enabled') -ne '1') {
            throw "Installed mod did not enable: $($entry.Relative)"
        }
        $installedFiles.Add($entry.Relative)
    }

    foreach ($entry in $rootLoaders) {
        Copy-AtomicFile $entry.Source $entry.Destination
        if ((Get-Sha256 $entry.Destination) -ne $entry.Hash -or -not (Test-FrameworkBinary $entry.Destination)) {
            throw 'Installed root loader verification failed'
        }
        $installedFiles.Add($entry.Relative)
    }

    $installedState = [ordered]@{}
    foreach ($relative in @($installedFiles | Select-Object -Unique)) {
        $installedPath = Join-Path $game $relative
        if (-not (Test-Path -LiteralPath $installedPath -PathType Leaf)) {
            throw "Installed receipt file is missing before readback: $relative"
        }
        $installedState[$relative] = Get-Sha256 $installedPath
    }

    $receipt = [ordered]@{
        schema_version = 2
        install_id = $installId
        package_id = $packageId
        installed_at = [DateTime]::UtcNow.ToString('o')
        host = $HostKey
        game_dir = $game
        source_client = $source
        manifest_sha256 = Get-Sha256 $manifestPath
        symbol_fragment_sha256 = Get-Sha256 $fragmentPath
        installed_files = @($installedFiles | Select-Object -Unique)
        installed_sha256 = $installedState
        backup_entries = @($journal | ForEach-Object {
            [ordered]@{
                path = $_.Relative
                existed = $_.Existed
            }
        })
    }
    $receiptPath = Join-Path $backupRoot 'install-receipt.json'
    [IO.File]::WriteAllText(
        $receiptPath,
        (($receipt | ConvertTo-Json -Depth 6) + [Environment]::NewLine),
        (New-Object Text.UTF8Encoding($false))
    )
    [pscustomobject]@{
        ok = $true
        check_only = $false
        install_id = $installId
        package_id = $packageId
        host = $HostKey
        installed_files = @($installedFiles)
        backup = $backupRoot
        receipt = $receiptPath
        third_party_loader_chain = $chainRequired
    } | ConvertTo-Json -Depth 5 -Compress
} catch {
    $failure = $_
    try {
        Restore-Journal $journal $backupRoot
        $rollbackPath = Join-Path $backupRoot 'rollback-error.txt'
        [IO.File]::WriteAllText(
            $rollbackPath,
            $failure.Exception.Message + [Environment]::NewLine,
            (New-Object Text.UTF8Encoding($false))
        )
    } catch {
        throw "Installation failed: $($failure.Exception.Message). Rollback also failed: $($_.Exception.Message). Backup: $backupRoot"
    }
    throw "Installation failed and was rolled back: $($failure.Exception.Message). Backup: $backupRoot"
}
