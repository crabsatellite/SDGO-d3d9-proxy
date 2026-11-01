<#
.SYNOPSIS
  Removes the collection capsule client registry using one verified install receipt.

.DESCRIPTION
  Only files below sdgo\mods\capsule_catalog that were written by the selected
  install are restored or removed. The framework loader, core, global settings,
  symbols and unrelated mods stay at their current versions.

  The uninstall refuses to write if any owned file changed after installation.
  When -Receipt is omitted, the newest collection-capsule-client receipt below
  the selected game directory is used.
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$Receipt,
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$ExpectedTimestamp = [uint32]0x54BE1568
$ExpectedImageSize = [uint32]0x00C97000
$PackageId = 'collection-capsule-client'
$OwnedPrefix = 'sdgo\mods\capsule_catalog\'

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
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 256) {
        throw 'GOnline.exe is truncated'
    }
    $pe = [BitConverter]::ToUInt32($bytes, 0x3C)
    if ($pe -lt 0x40 -or $pe + 0x18 + 0x3C -gt $bytes.Length) {
        throw 'GOnline.exe has an invalid PE header'
    }
    if ([Text.Encoding]::ASCII.GetString($bytes, $pe, 4) -ne "PE`0`0") {
        throw 'GOnline.exe has an invalid PE signature'
    }
    return [pscustomobject]@{
        Timestamp = [BitConverter]::ToUInt32($bytes, $pe + 8)
        ImageSize = [BitConverter]::ToUInt32($bytes, $pe + 0x18 + 0x38)
    }
}

function Assert-TargetClosed([string]$Executable) {
    $expected = [IO.Path]::GetFullPath($Executable)
    try {
        foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='GOnline.exe'")) {
            if (
                $process.ExecutablePath -and
                [IO.Path]::GetFullPath($process.ExecutablePath).Equals(
                    $expected, [StringComparison]::OrdinalIgnoreCase
                )
            ) {
                throw "This GOnline.exe is running; close it first. No process was stopped: $expected"
            }
        }
    } catch {
        if ($_.Exception.Message -like 'This GOnline.exe is running*') {
            throw
        }
        throw 'Unable to verify whether this GOnline.exe is running; no files changed'
    }
}

function Convert-SafeRelativePath([string]$Relative) {
    if ([string]::IsNullOrWhiteSpace($Relative)) {
        throw 'Receipt contains an empty path'
    }
    $normalized = $Relative.Replace('/', '\')
    if ([IO.Path]::IsPathRooted($normalized) -or $normalized.Contains(':')) {
        throw "Receipt path must be relative: $Relative"
    }
    $parts = @($normalized.Split('\'))
    if (@($parts | Where-Object { $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' }).Count) {
        throw "Unsafe receipt path: $Relative"
    }
    return [string]::Join('\', $parts)
}

function Copy-AtomicFile([string]$Source, [string]$Destination) {
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    $temporary = Join-Path $parent ('.' + [IO.Path]::GetFileName($Destination) + '.' + [guid]::NewGuid().ToString('N') + '.pending')
    $replaced = Join-Path $parent ('.' + [IO.Path]::GetFileName($Destination) + '.' + [guid]::NewGuid().ToString('N') + '.replaced')
    try {
        [IO.File]::WriteAllBytes($temporary, [IO.File]::ReadAllBytes($Source))
        if (Test-Path -LiteralPath $Destination -PathType Leaf) {
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

if ([string]::IsNullOrWhiteSpace($GameDir)) {
    $GameDir = Select-GameDirectory
}
Assert-NoReparseAncestors $GameDir 'GameDir'
$game = Get-AbsoluteExistingPath $GameDir 'GameDir' $false
$exe = Join-Path $game 'GOnline.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
    throw "GameDir does not contain GOnline.exe: $game"
}
Assert-NoReparsePath $game $exe 'GOnline.exe'
$targetHost = Read-PeHost $exe
if ($targetHost.Timestamp -ne $ExpectedTimestamp -or $targetHost.ImageSize -ne $ExpectedImageSize) {
    throw ('Unsupported GOnline.exe host: timestamp={0:X8}, size={1:X8}' -f
        $targetHost.Timestamp, $targetHost.ImageSize)
}
Assert-TargetClosed $exe

$backups = Join-Path $game 'sdgo\backups'
if ([string]::IsNullOrWhiteSpace($Receipt)) {
    if (-not (Test-Path -LiteralPath $backups -PathType Container)) {
        throw 'No client install receipts exist for this game directory'
    }
    $candidates = @(
        Get-ChildItem -LiteralPath $backups -Directory -Force |
        Where-Object { $_.Name.StartsWith($PackageId + '-', [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object {
            $candidate = Join-Path $_.FullName 'install-receipt.json'
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                Get-Item -LiteralPath $candidate -Force
            }
        } |
        Sort-Object LastWriteTimeUtc -Descending
    )
    if ($candidates.Count -eq 0) {
        throw 'No collection capsule client receipt exists for this game directory'
    }
    $Receipt = $candidates[0].FullName
}

Assert-NoReparseAncestors $Receipt 'Receipt'
$receiptPath = Get-AbsoluteExistingPath $Receipt 'Receipt' $true
if (-not (Test-PathInside $backups $receiptPath)) {
    throw 'Receipt is outside this game directory backup tree'
}
if (-not [IO.Path]::GetFileName($receiptPath).Equals('install-receipt.json', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Receipt filename must be install-receipt.json'
}
$backupRoot = Split-Path -Parent $receiptPath
Assert-NoReparsePath $game $receiptPath 'Receipt'
$uninstallReceipt = Join-Path $backupRoot 'uninstall-receipt.json'
if (Test-Path -LiteralPath $uninstallReceipt) {
    throw 'This install receipt has already been uninstalled'
}

$record = ConvertFrom-Json ([IO.File]::ReadAllText($receiptPath))
if ($record.schema_version -ne 2 -or [string]$record.package_id -cne $PackageId) {
    throw 'Receipt is not a supported collection capsule client install'
}
if (
    -not [IO.Path]::GetFullPath([string]$record.game_dir).Equals(
        $game, [StringComparison]::OrdinalIgnoreCase
    )
) {
    throw 'Receipt game directory differs from the selected client'
}
if ($null -eq $record.installed_sha256 -or $null -eq $record.backup_entries) {
    throw 'Receipt has no verified installed-state map'
}

$backupByPath = @{}
foreach ($entry in @($record.backup_entries)) {
    $relative = Convert-SafeRelativePath ([string]$entry.path)
    $key = $relative.ToLowerInvariant()
    if ($backupByPath.ContainsKey($key)) {
        throw "Receipt contains duplicate backup entries: $relative"
    }
    $backupByPath[$key] = $entry
}

$owned = New-Object Collections.Generic.List[object]
foreach ($property in $record.installed_sha256.PSObject.Properties) {
    $relative = Convert-SafeRelativePath ([string]$property.Name)
    if (-not $relative.StartsWith($OwnedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        continue
    }
    $expected = [string]$property.Value
    if ($expected -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Receipt has an invalid installed hash: $relative"
    }
    $key = $relative.ToLowerInvariant()
    if (-not $backupByPath.ContainsKey($key)) {
        throw "Receipt has no backup entry for owned file: $relative"
    }
    $destination = [IO.Path]::GetFullPath((Join-Path $game $relative))
    Assert-NoReparsePath $game $destination "Installed file $relative"
    if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
        throw "Installed collection file is missing; no files changed: $relative"
    }
    if ((Get-Sha256 $destination) -ine $expected) {
        throw "Installed collection file changed after installation; no files changed: $relative"
    }
    $entry = $backupByPath[$key]
    $backup = Join-Path $backupRoot ('before\' + $relative)
    if ([bool]$entry.existed) {
        Assert-NoReparsePath $backupRoot $backup "Backup file $relative"
        if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
            throw "Receipt backup file is missing; no files changed: $relative"
        }
    }
    $owned.Add([pscustomobject]@{
        Relative = $relative
        Destination = $destination
        Existed = [bool]$entry.existed
        Backup = $backup
    })
}
if ($owned.Count -eq 0) {
    throw 'Receipt contains no collection capsule client files'
}

if ($CheckOnly) {
    [pscustomobject]@{
        ok = $true
        check_only = $true
        package_id = $PackageId
        receipt = $receiptPath
        owned_files = $owned.Count
        preserves_framework = $true
        preserves_symbols = $true
    } | ConvertTo-Json -Compress
    return
}

$undoRoot = Join-Path $backupRoot 'before-uninstall'
if (Test-Path -LiteralPath $undoRoot) {
    throw 'Uninstall recovery directory already exists; no files changed'
}
New-Item -ItemType Directory -Path $undoRoot | Out-Null
try {
    foreach ($entry in $owned) {
        $undo = Join-Path $undoRoot $entry.Relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $undo) | Out-Null
        Copy-Item -LiteralPath $entry.Destination -Destination $undo
    }
    foreach ($entry in @($owned | Sort-Object Relative -Descending)) {
        if ($entry.Existed) {
            Copy-AtomicFile $entry.Backup $entry.Destination
        } else {
            Remove-Item -LiteralPath $entry.Destination -Force
        }
    }
    foreach ($entry in $owned) {
        if ($entry.Existed) {
            if (
                -not (Test-Path -LiteralPath $entry.Destination -PathType Leaf) -or
                (Get-Sha256 $entry.Destination) -ne (Get-Sha256 $entry.Backup)
            ) {
                throw "Uninstall restore verification failed: $($entry.Relative)"
            }
        } elseif (Test-Path -LiteralPath $entry.Destination) {
            throw "Uninstall removal verification failed: $($entry.Relative)"
        }
    }
    $modRoot = Join-Path $game $OwnedPrefix.TrimEnd('\')
    if (Test-Path -LiteralPath $modRoot -PathType Container) {
        Get-ChildItem -LiteralPath $modRoot -Directory -Recurse -Force |
            Sort-Object FullName -Descending |
            ForEach-Object {
                if (@(Get-ChildItem -LiteralPath $_.FullName -Force).Count -eq 0) {
                    Remove-Item -LiteralPath $_.FullName
                }
            }
        if (@(Get-ChildItem -LiteralPath $modRoot -Force).Count -eq 0) {
            Remove-Item -LiteralPath $modRoot
        }
    }
    $result = [ordered]@{
        schema_version = 1
        package_id = $PackageId
        install_receipt = $receiptPath
        uninstalled_at = [DateTime]::UtcNow.ToString('o')
        restored_or_removed = @($owned | ForEach-Object { $_.Relative })
        framework_preserved = $true
        symbols_preserved = $true
    }
    [IO.File]::WriteAllText(
        $uninstallReceipt,
        (($result | ConvertTo-Json -Depth 5) + [Environment]::NewLine),
        (New-Object Text.UTF8Encoding($false))
    )
    $result | ConvertTo-Json -Depth 5 -Compress
} catch {
    $failure = $_
    foreach ($entry in $owned) {
        $undo = Join-Path $undoRoot $entry.Relative
        if (Test-Path -LiteralPath $undo -PathType Leaf) {
            Copy-AtomicFile $undo $entry.Destination
        }
    }
    throw "Uninstall failed and the installed collection files were restored: $($failure.Exception.Message)"
}
