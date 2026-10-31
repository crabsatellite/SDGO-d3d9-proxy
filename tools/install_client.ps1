<#
.SYNOPSIS
  Installs, updates or removes the SDGO mod framework in a game folder.

.DESCRIPTION
  -GameDir is the folder that contains GOnline.exe. How the game is started (any launcher,
  a shortcut, GOnline.exe itself) does not matter: Windows loads d3d9.dll from GOnline.exe's
  own folder, and the framework finds everything relative to GOnline.exe.

  Installs client\ (d3d9.dll + sdgo\) from the release package next to this script, or from
  -Source (for example build\stage\Release\client).
  * The game build is read from GOnline.exe and checked against the bundled symbol files.
    An unknown build still installs: the framework then loads no mods and the game runs
    unchanged (see README, "Other game builds").
  * An existing d3d9.dll that is not this framework's (a graphics wrapper, for example) is
    renamed to d3d9_chain.dll and chained in sdgo\sdgo.ini, so it keeps working.
  * Existing sdgo\sdgo.ini and mod.ini files are kept unless -OverwriteConfig is given.
  * -Uninstall removes d3d9.dll and sdgo\ (to the Recycle Bin) and puts a chained d3d9.dll back.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install_client.ps1 -GameDir "D:\Games\SDGO"

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install_client.ps1 -GameDir "D:\Games\SDGO" -Uninstall
#>
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [string]$Source,
    [switch]$OverwriteConfig,
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
$ProductName = "SDGO mod framework"
$Loader = "d3d9.dll"
$Chain = "d3d9_chain.dll"
$ChainKey = "chain_d3d9"

Add-Type -Namespace SdgoInstall -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern uint GetPrivateProfileString(string section, string key, string def, System.Text.StringBuilder value, uint size, string file);
'@
Add-Type -AssemblyName Microsoft.VisualBasic

function Get-HostKey([string]$exe) {
    $stream = [IO.File]::OpenRead($exe)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        $stream.Position = 0x3C
        $nt = $reader.ReadInt32()
        $stream.Position = $nt
        if ($reader.ReadUInt32() -ne 0x4550) { throw "$exe is not a Windows executable" }
        $stream.Position = $nt + 8
        $timestamp = $reader.ReadUInt32()
        $stream.Position = $nt + 0x18 + 0x38
        $size = $reader.ReadUInt32()
    } finally {
        $stream.Close()
    }
    "{0}-{1:X8}-{2:X8}" -f [IO.Path]::GetFileNameWithoutExtension($exe), $timestamp, $size
}

# This framework's loader DLLs carry its product name; older builds without version info
# are recognised by the core path they load.
function Test-OurLoader([string]$file) {
    if ((Get-Item -LiteralPath $file).VersionInfo.ProductName -eq $ProductName) { return $true }
    $bytes = [IO.File]::ReadAllBytes($file)
    $text = [Text.Encoding]::Unicode.GetString($bytes)
    return $text.Contains("sdgo\core.dll")
}

function Remove-ToRecycleBin([string]$path) {
    if (Test-Path -LiteralPath $path -PathType Container) {
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory($path, 'OnlyErrorDialogs', 'SendToRecycleBin')
    } elseif (Test-Path -LiteralPath $path) {
        [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile($path, 'OnlyErrorDialogs', 'SendToRecycleBin')
    }
}

function Get-IniValue([string]$file, [string]$section, [string]$key) {
    $value = New-Object Text.StringBuilder 1024
    [void][SdgoInstall.Ini]::GetPrivateProfileString($section, $key, "", $value, 1024, $file)
    $value.ToString()
}

$game = (Resolve-Path -LiteralPath $GameDir).Path
$exe = Join-Path $game "GOnline.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "$game does not contain GOnline.exe" }
$running = Get-CimInstance Win32_Process -Filter "Name='GOnline.exe'" | Where-Object { $_.ExecutablePath -eq $exe }
if ($running) { throw "GOnline.exe in $game is running; close the game first" }

$loaderPath = Join-Path $game $Loader
$sdgoDir = Join-Path $game "sdgo"
$iniPath = Join-Path $sdgoDir "sdgo.ini"

if ($Uninstall) {
    $chainName = ""
    if (Test-Path -LiteralPath $iniPath) { $chainName = Get-IniValue $iniPath "loader" $ChainKey }
    if ((Test-Path -LiteralPath $loaderPath) -and (Test-OurLoader $loaderPath)) {
        Remove-ToRecycleBin $loaderPath
        Write-Host "removed  $Loader"
    }
    Remove-ToRecycleBin $sdgoDir
    Write-Host "removed  sdgo\"
    if ($chainName -and -not (Test-Path -LiteralPath $loaderPath)) {
        $chainPath = Join-Path $game $chainName
        if (Test-Path -LiteralPath $chainPath) {
            Rename-Item -LiteralPath $chainPath -NewName $Loader
            Write-Host "restored $Loader (was $chainName)"
        }
    }
    Write-Host "SDGO mod framework removed from $game (files are in the Recycle Bin)"
    return
}

if (-not $Source) {
    $Source = Join-Path $PSScriptRoot "client"
    if (-not (Test-Path -LiteralPath $Source)) { $Source = Join-Path $PSScriptRoot "..\build\stage\Release\client" }
}
$Source = (Resolve-Path -LiteralPath $Source).Path
if (-not (Test-Path -LiteralPath (Join-Path $Source $Loader))) { throw "$Source has no $Loader; pass -Source <package>\client" }

$hostKey = Get-HostKey $exe
if (Test-Path -LiteralPath (Join-Path $Source "sdgo\symbols\$hostKey.ini")) {
    Write-Host "game build $hostKey is supported"
} else {
    Write-Warning "game build $hostKey has no symbol file in this package; the framework will load but keep every mod disabled (README: Other game builds)"
}

$chainName = ""
if (Test-Path -LiteralPath $loaderPath) {
    if (Test-OurLoader $loaderPath) {
        Write-Host "updating the installed framework"
    } else {
        $chainPath = Join-Path $game $Chain
        if (Test-Path -LiteralPath $chainPath) {
            throw "$game has both a foreign $Loader and $Chain; resolve which one to keep first"
        }
        Rename-Item -LiteralPath $loaderPath -NewName $Chain
        $chainName = $Chain
        Write-Host "kept the existing $Loader as $Chain (chained)"
    }
}

$userEditable = @("sdgo\sdgo.ini")
Get-ChildItem -LiteralPath (Join-Path $Source "sdgo\mods") -Directory | ForEach-Object { $userEditable += "sdgo\mods\$($_.Name)\mod.ini" }
Get-ChildItem -LiteralPath $Source -Recurse -File | ForEach-Object {
    $relative = $_.FullName.Substring($Source.Length).TrimStart('\')
    $destination = Join-Path $game $relative
    if (($userEditable -contains $relative) -and (Test-Path -LiteralPath $destination) -and -not $OverwriteConfig) {
        Write-Host "kept     $relative"
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
    Write-Host "copied   $relative"
}
if ($chainName) {
    [void][SdgoInstall.Ini]::WritePrivateProfileString("loader", $ChainKey, $chainName, $iniPath)
    Write-Host "set      sdgo\sdgo.ini [loader] $ChainKey=$chainName"
}
Write-Host "SDGO mod framework installed in $game. Start the game as usual; the log is sdgo\logs\sdgo.log."
