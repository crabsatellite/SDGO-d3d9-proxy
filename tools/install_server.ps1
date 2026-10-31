<#
.SYNOPSIS
  Installs, updates or removes the SDGO mod framework in battle server folders.

.DESCRIPTION
  -ServerRoot is either one battle server folder (it contains BattleServer.exe) or a folder
  whose subfolders are battle servers (BattleServer1, BattleServer2, ...). The folder names
  and how the servers are started (a server manager, a batch file, a service) do not
  matter: Windows loads winmm.dll from BattleServer.exe's own folder.

  Installs server\ (winmm.dll + sdgo\) from the release package next to this script, or
  from -Source (for example build\stage\Release\server).
  * The server build is read from BattleServer.exe and checked against the bundled symbol
    files. An unknown build still installs: the framework then loads no mods.
  * Battle servers must be stopped while installing (their winmm.dll would be locked).
    Start them again afterwards.
  * An existing winmm.dll that is not this framework's is renamed to winmm_chain.dll and
    chained in sdgo\sdgo.ini.
  * Existing sdgo\sdgo.ini and mod.ini files are kept unless -OverwriteConfig is given.
  * -Only limits the install to the named subfolders; -Uninstall removes the framework.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install_server.ps1 -ServerRoot "D:\SDGO-Server\Server"

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install_server.ps1 -ServerRoot "D:\SDGO-Server\Server" -Only BattleServer3
#>
param(
    [Parameter(Mandatory = $true)][string]$ServerRoot,
    [string[]]$Only = @(),
    [string]$Source,
    [switch]$OverwriteConfig,
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
$ProductName = "SDGO mod framework"
$Loader = "winmm.dll"
$Chain = "winmm_chain.dll"
$ChainKey = "chain_winmm"

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

function Test-OurLoader([string]$file) {
    if ((Get-Item -LiteralPath $file).VersionInfo.ProductName -eq $ProductName) { return $true }
    $text = [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($file))
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

$root = (Resolve-Path -LiteralPath $ServerRoot).Path
if (Test-Path -LiteralPath (Join-Path $root "BattleServer.exe")) {
    $targets = @(Get-Item -LiteralPath $root)
} else {
    $targets = @(Get-ChildItem -LiteralPath $root -Directory | Where-Object {
        (Test-Path -LiteralPath (Join-Path $_.FullName "BattleServer.exe")) -and ($Only.Count -eq 0 -or $Only -contains $_.Name)
    })
}
if ($targets.Count -eq 0) { throw "No battle server folder (with BattleServer.exe) at $root" }

$running = Get-CimInstance Win32_Process -Filter "Name='BattleServer.exe'" | ForEach-Object { Split-Path -Parent $_.ExecutablePath }
$busy = $targets | Where-Object { $running -contains $_.FullName }
if ($busy) { throw "Stop the battle servers in $(($busy | ForEach-Object { $_.Name }) -join ', ') first" }

if (-not $Uninstall) {
    if (-not $Source) {
        $Source = Join-Path $PSScriptRoot "server"
        if (-not (Test-Path -LiteralPath $Source)) { $Source = Join-Path $PSScriptRoot "..\build\stage\Release\server" }
    }
    $Source = (Resolve-Path -LiteralPath $Source).Path
    if (-not (Test-Path -LiteralPath (Join-Path $Source $Loader))) { throw "$Source has no $Loader; pass -Source <package>\server" }
    $userEditable = @("sdgo\sdgo.ini")
    Get-ChildItem -LiteralPath (Join-Path $Source "sdgo\mods") -Directory | ForEach-Object { $userEditable += "sdgo\mods\$($_.Name)\mod.ini" }
}

foreach ($target in $targets) {
    $dir = $target.FullName
    $name = $target.Name
    $loaderPath = Join-Path $dir $Loader
    $sdgoDir = Join-Path $dir "sdgo"
    $iniPath = Join-Path $sdgoDir "sdgo.ini"

    if ($Uninstall) {
        $chainName = ""
        if (Test-Path -LiteralPath $iniPath) { $chainName = Get-IniValue $iniPath "loader" $ChainKey }
        if ((Test-Path -LiteralPath $loaderPath) -and (Test-OurLoader $loaderPath)) { Remove-ToRecycleBin $loaderPath }
        Remove-ToRecycleBin $sdgoDir
        if ($chainName -and -not (Test-Path -LiteralPath $loaderPath) -and (Test-Path -LiteralPath (Join-Path $dir $chainName))) {
            Rename-Item -LiteralPath (Join-Path $dir $chainName) -NewName $Loader
        }
        Write-Host "$name : removed"
        continue
    }

    $hostKey = Get-HostKey (Join-Path $dir "BattleServer.exe")
    if (Test-Path -LiteralPath (Join-Path $Source "sdgo\symbols\$hostKey.ini")) {
        Write-Host "$name : server build $hostKey is supported"
    } else {
        Write-Warning "$name : server build $hostKey has no symbol file in this package; the framework will load but keep every mod disabled"
    }
    $chainName = ""
    if ((Test-Path -LiteralPath $loaderPath) -and -not (Test-OurLoader $loaderPath)) {
        if (Test-Path -LiteralPath (Join-Path $dir $Chain)) { throw "$name has both a foreign $Loader and $Chain; resolve which one to keep first" }
        Rename-Item -LiteralPath $loaderPath -NewName $Chain
        $chainName = $Chain
        Write-Host "$name : kept the existing $Loader as $Chain (chained)"
    }
    $copied = 0
    $kept = 0
    Get-ChildItem -LiteralPath $Source -Recurse -File | ForEach-Object {
        $relative = $_.FullName.Substring($Source.Length).TrimStart('\')
        $destination = Join-Path $dir $relative
        if (($userEditable -contains $relative) -and (Test-Path -LiteralPath $destination) -and -not $OverwriteConfig) {
            $kept++
            return
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
        $copied++
    }
    if ($chainName) { [void][SdgoInstall.Ini]::WritePrivateProfileString("loader", $ChainKey, $chainName, $iniPath) }
    Write-Host "$name : installed ($copied files copied, $kept settings files kept)"
}
if (-not $Uninstall) {
    Write-Host "Start the battle servers again. Features with a server part also need the client install (install_client.ps1) on the players' machines."
}
