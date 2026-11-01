<#
.SYNOPSIS
  Test of install_client.ps1 and install_server.ps1 updating an existing install (no game,
  no build needed).

.DESCRIPTION
  Builds throw-away packages and game/server folders with a stand-in exe and one mod, demo:
    1. installs package 1, then changes the mod's settings the way a player would;
    2. updates with package 2: its mod.ini (a new version and requires) is installed, and the
       player's [mod] enabled and [settings] values come back, a new setting gets its default,
       sdgo.ini is kept;
    3. -OverwriteConfig installs mod.ini and sdgo.ini as they are in package 2.
  The battle server installer gets the same update check.
#>
param([string]$BuildDir = "build")

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }))
$work = [IO.Path]::GetFullPath((Join-Path $buildRoot "install_test_run"))
if (-not $work.StartsWith($buildRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe test directory" }
if (Test-Path $work) { Remove-Item $work -Recurse -Force }

Add-Type -Namespace SdgoInstallTest -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern uint GetPrivateProfileString(string section, string key, string def, System.Text.StringBuilder value, uint size, string file);
'@

# What the core reads (GetPrivateProfileString), "<none>" for a missing key.
function Read-Ini([string]$file, [string]$section, [string]$key) {
    $value = New-Object Text.StringBuilder 1024
    [void][SdgoInstallTest.Ini]::GetPrivateProfileString($section, $key, "<none>", $value, 1024, $file)
    $value.ToString()
}

# Just enough of a PE header for the installers' host key: e_lfanew, "PE", TimeDateStamp,
# SizeOfImage. Host key: <name>-12345678-00001000.
function New-FakeExe([string]$path) {
    $bytes = New-Object byte[] 0x100
    $bytes[0] = 0x4D
    $bytes[1] = 0x5A
    [BitConverter]::GetBytes([int]0x40).CopyTo($bytes, 0x3C)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($bytes, 0x40)
    [BitConverter]::GetBytes([uint32]0x12345678).CopyTo($bytes, 0x48)
    [BitConverter]::GetBytes([uint32]0x1000).CopyTo($bytes, 0x40 + 0x18 + 0x38)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    [IO.File]::WriteAllBytes($path, $bytes)
}

function New-Package([string]$dir, [string]$loader, [string]$hostName, [string]$modIni) {
    New-Item -ItemType Directory -Force -Path (Join-Path $dir "sdgo\mods\demo"), (Join-Path $dir "sdgo\symbols") | Out-Null
    # The installers recognise this framework's loader by the core path in it.
    [IO.File]::WriteAllBytes((Join-Path $dir $loader), [Text.Encoding]::Unicode.GetBytes("sdgo\core.dll"))
    Set-Content -LiteralPath (Join-Path $dir "sdgo\core.dll") -Value "core" -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $dir "sdgo\sdgo.ini") -Value "[core]`r`nlog_level=1" -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $dir "sdgo\symbols\$hostName-12345678-00001000.ini") -Value "" -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $dir "sdgo\mods\demo\demo.dll") -Value "demo" -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $dir "sdgo\mods\demo\mod.ini") -Value $modIni -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $dir "sdgo\mods\demo\config.ini") -Value '[owner] mode=package' -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $dir "sdgo\mods\demo\roster.json") -Value '{"pool":"package"}' -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $dir "sdgo\mods\demo\config.example.ini") -Value $modIni.Split("`n")[0] -Encoding Ascii
}

$modIni1 = @'
; demo 1.0.0
[mod]
id=demo
category=fixes
feature=demo
config_files=config.ini,roster.json
version=1.0.0
enabled=0
requires=old.symbol

[settings]
; a setting the player changes
Alpha=1
Beta=2
Units=1,2,3
'@
$modIni2 = @'
; demo 2.0.0
[mod]
id=demo
category=gameplay
feature=demo
config_files=config.ini,roster.json
version=2.0.0
enabled=0
requires=old.symbol,new.symbol

[settings]
; a setting the player changes
Alpha=1
Beta=3
Units=1,2,3
Delta=4
'@

$failures = 0
function Check([string]$what, [string]$actual, [string]$expected) {
    $ok = $actual -ceq $expected
    Write-Host ("{0} {1}: {2}" -f ($(if ($ok) { "ok  " } else { "FAIL" })), $what, $actual)
    if (-not $ok) { $script:failures++ }
}

function Install([string]$script, [string[]]$arguments) {
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo "tools\$script") @arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "$script failed: $output" }
    $output | Where-Object { "$_" -match "demo\\mod.ini|installed \(" } | ForEach-Object { Write-Host "     $_" }
}

# A player's (or server owner's) changes: switch the mod on, change Alpha, add a key of their own.
function Edit-Settings([string]$modIniPath, [string]$sdgoIniPath) {
    $text = (Get-Content -LiteralPath $modIniPath -Raw).Replace("enabled=0", "enabled=1").Replace("Alpha=1", "Alpha=5")
    Set-Content -LiteralPath $modIniPath -Value ($text.TrimEnd() + "`r`nGamma=7") -Encoding Ascii
    Set-Content -LiteralPath $sdgoIniPath -Value "[core]`r`nlog_level=0" -Encoding Ascii
    $folder = Split-Path -Parent $modIniPath
    Set-Content -LiteralPath (Join-Path $folder 'config.ini') -Value '[owner] mode=custom' -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $folder 'roster.json') -Value '{"pool":"custom"}' -Encoding Ascii
}

function Check-Update([string]$side, [string]$modIniPath, [string]$sdgoIniPath) {
    Check "$side version from package 2" (Read-Ini $modIniPath "mod" "version") "2.0.0"
    Check "$side requires from package 2" (Read-Ini $modIniPath "mod" "requires") "old.symbol,new.symbol"
    Check "$side enabled kept" (Read-Ini $modIniPath "mod" "enabled") "1"
    Check "$side changed setting kept" (Read-Ini $modIniPath "settings" "Alpha") "5"
    Check "$side setting value in the player's file kept" (Read-Ini $modIniPath "settings" "Beta") "2"
    Check "$side own setting kept" (Read-Ini $modIniPath "settings" "Gamma") "7"
    Check "$side new setting has its default" (Read-Ini $modIniPath "settings" "Delta") "4"
    Check "$side list setting unchanged" (Read-Ini $modIniPath "settings" "Units") "1,2,3"
    Check "$side header comment from package 2" (Get-Content -LiteralPath $modIniPath -TotalCount 1) "; demo 2.0.0"
    Check "$side sdgo.ini kept" (Read-Ini $sdgoIniPath "core" "log_level") "0"
    Check "$side category updates from manifest" (Read-Ini $modIniPath 'mod' 'category') 'gameplay'
    $folder = Split-Path -Parent $modIniPath
    Check "$side user config kept" (Get-Content -LiteralPath (Join-Path $folder 'config.ini') -TotalCount 1) '[owner] mode=custom'
    Check "$side user roster kept" (Get-Content -LiteralPath (Join-Path $folder 'roster.json') -TotalCount 1) '{"pool":"custom"}'
    Check "$side example updates" (Get-Content -LiteralPath (Join-Path $folder 'config.example.ini') -TotalCount 1) '; demo 2.0.0'
}

# ---- client
$client1 = Join-Path $work "package1\client"
$client2 = Join-Path $work "package2\client"
New-Package $client1 "d3d9.dll" "GOnline" $modIni1
New-Package $client2 "d3d9.dll" "GOnline" $modIni2
$game = Join-Path $work "game"
New-FakeExe (Join-Path $game "GOnline.exe")
$gameModIni = Join-Path $game "sdgo\mods\demo\mod.ini"
$gameSdgoIni = Join-Path $game "sdgo\sdgo.ini"

Write-Host "== client: install package 1, change settings, update with package 2"
Install "install_client.ps1" @("-GameDir", $game, "-Source", $client1)
Check "client fresh install version" (Read-Ini $gameModIni "mod" "version") "1.0.0"
Edit-Settings $gameModIni $gameSdgoIni
Install "install_client.ps1" @("-GameDir", $game, "-Source", $client2)
Check-Update "client" $gameModIni $gameSdgoIni

Write-Host "== client: -OverwriteConfig"
Install "install_client.ps1" @("-GameDir", $game, "-Source", $client2, "-OverwriteConfig")
Check "client mod.ini as in package 2" ((Get-Content -LiteralPath $gameModIni) -join "|") (($modIni2 -split "`r?`n") -join "|")
Check "client sdgo.ini as in package 2" (Read-Ini $gameSdgoIni "core" "log_level") "1"
Check 'client explicit config overwrite' (Get-Content -LiteralPath (Join-Path $game 'sdgo\mods\demo\config.ini') -TotalCount 1) '[owner] mode=package'

# Unsafe metadata is rejected before any target payload is updated.
$sourceIni = Join-Path $client2 'sdgo\mods\demo\mod.ini'
$originalManifest = [IO.File]::ReadAllText($sourceIni)
foreach ($bad in @('../outside.ini','C:\outside.ini','demo.dll','mod.ini')) {
    [IO.File]::WriteAllText($sourceIni, $originalManifest.Replace('config_files=config.ini,roster.json', "config_files=$bad"))
    $ErrorActionPreference = 'Continue'
    $badOutput = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\install_client.ps1') -GameDir $game -Source $client2 2>&1
    $badExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    Check "unsafe config rejected: $bad" ([string]($badExit -ne 0)) 'True'
}
[IO.File]::WriteAllText($sourceIni, $originalManifest)

# ---- battle server
$server1 = Join-Path $work "package1\server"
$server2 = Join-Path $work "package2\server"
New-Package $server1 "winmm.dll" "BattleServer" $modIni1
New-Package $server2 "winmm.dll" "BattleServer" $modIni2
$serverRoot = Join-Path $work "Server"
New-FakeExe (Join-Path $serverRoot "BattleServer1\BattleServer.exe")
$serverModIni = Join-Path $serverRoot "BattleServer1\sdgo\mods\demo\mod.ini"
$serverSdgoIni = Join-Path $serverRoot "BattleServer1\sdgo\sdgo.ini"

Write-Host "== server: install package 1, change settings, update with package 2"
Install "install_server.ps1" @("-ServerRoot", $serverRoot, "-Source", $server1)
Edit-Settings $serverModIni $serverSdgoIni
Install "install_server.ps1" @("-ServerRoot", $serverRoot, "-Source", $server2)
Check-Update "server" $serverModIni $serverSdgoIni

# World uses the same updater, including owner-config preservation.
foreach ($version in 1,2) {
    $packageRoot = Join-Path $work "package$version\world"
    New-Package $packageRoot "winmm.dll" "WorldServer" $(if ($version -eq 1) { $modIni1 } else { $modIni2 })
}
$world = Join-Path $work "WorldServer"
New-FakeExe (Join-Path $world "WorldServer.exe")
$worldModIni = Join-Path $world "sdgo\mods\demo\mod.ini"
$worldSdgoIni = Join-Path $world "sdgo\sdgo.ini"
Install "install_server.ps1" @("-ServerRoot", $world, "-Role", "World", "-Source", (Join-Path $work "package1\world"))
Edit-Settings $worldModIni $worldSdgoIni
Install "install_server.ps1" @("-ServerRoot", $world, "-Role", "World", "-Source", (Join-Path $work "package2\world"))
Check-Update "world" $worldModIni $worldSdgoIni

Remove-Item $work -Recurse -Force
if ($failures) { throw "$failures install check(s) failed" }
Write-Host "ALL INSTALL CHECKS PASSED"
