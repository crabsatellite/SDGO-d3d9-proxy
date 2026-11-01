param([string]$Config = 'Release', [string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }))
$stage = Join-Path $buildRoot "stage\$Config"
$work = Join-Path $buildRoot ('package_catalog_test_run\' + [guid]::NewGuid().ToString('N'))
$stale = Join-Path $stage ('client\sdgo\mods\stale_fixture_' + [guid]::NewGuid().ToString('N'))
$undeclared = Join-Path $stage ('client\sdgo\mods\match_ai\fixture_' + [guid]::NewGuid().ToString('N') + '.json')
New-Item -ItemType Directory -Force -Path $work, $stale | Out-Null
Set-Content -LiteralPath (Join-Path $stale 'old.dll') -Value 'not a real DLL' -Encoding Ascii
Set-Content -LiteralPath $undeclared -Value '{"fixture":"not user data"}' -Encoding Ascii
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Read-Sha256([string]$path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($path)
    try { [BitConverter]::ToString($sha.ComputeHash($stream)) }
    finally { $stream.Dispose(); $sha.Dispose() }
}

function New-FakeExe([string]$path, [uint32]$stamp, [uint32]$size) {
    $bytes = New-Object byte[] 0x100
    $bytes[0] = 0x4D; $bytes[1] = 0x5A
    [BitConverter]::GetBytes([int]0x40).CopyTo($bytes, 0x3C)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($bytes, 0x40)
    [BitConverter]::GetBytes($stamp).CopyTo($bytes, 0x48)
    [BitConverter]::GetBytes($size).CopyTo($bytes, 0x40 + 0x18 + 0x38)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    [IO.File]::WriteAllBytes($path, $bytes)
}
function Invoke-Package([string]$edition, [string]$features, [string]$destination) {
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $repo 'tools\package.ps1'),'-Config',$Config,'-BuildDir',$buildRoot,'-OutputDir',$destination,'-Edition',$edition)
    if ($features) { $arguments += @('-Features',$features) }
    $output = & powershell @arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Package failed: $output" }
    (Get-ChildItem -LiteralPath $destination -Filter '*.zip').FullName
}
function Check-Package([string]$zip, [string[]]$expected) {
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $names = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\','/') })
        $actual = @($names | Where-Object { $_ -match '^(client|server|agent)/sdgo/mods/([^/]+)/' } | ForEach-Object { ($_ -split '/')[3] } | Sort-Object -Unique)
        if (@(Compare-Object ($actual | Sort-Object) ($expected | Sort-Object)).Count) { throw "Wrong selected mods: $actual" }
        foreach ($required in @('install_common.ps1','CONTENTS.md','client/d3d9.dll','server/winmm.dll','client/sdgo/catalog.json')) {
            if ($names -notcontains $required) { throw "Package lacks $required" }
        }
        if ($names -match 'stale_fixture_|fixture_.*\.json') { throw 'Unregistered/undeclared payload entered the archive' }
        $reader = New-Object IO.StreamReader ($archive.GetEntry('client/sdgo/catalog.json').Open())
        try { $catalog = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
        if (@(Compare-Object (@($catalog.mods | ForEach-Object { $_.id }) | Sort-Object) ($expected | Sort-Object)).Count) {
            throw 'Package catalog differs from actual payload'
        }
    } finally { $archive.Dispose() }
}
try {
    $baseMods = @('findwindow_fix','ime_fix','startup_crash_fix','agent_relay_log_fix','rank_progression_fix','new_account_profile')
    $baseZip = Invoke-Package 'Base' '' (Join-Path $work 'base')
    Check-Package $baseZip $baseMods
    $customZip = Invoke-Package 'Full' 'pvp-ai' (Join-Path $work 'custom')
    Check-Package $customZip ($baseMods + @('match_ai','pvp_ai'))
    $titleZip = Invoke-Package 'Full' 'room-titles' (Join-Path $work 'titles')
    Check-Package $titleZip ($baseMods + @('room_titles','room_titles_agent','room_titles_server'))
    $rewardZip = Invoke-Package 'Full' 'battle-rewards' (Join-Path $work 'rewards')
    Check-Package $rewardZip ($baseMods + @('battle_rewards'))
    $rewardArchive = [IO.Compression.ZipFile]::OpenRead($rewardZip)
    try {
        foreach ($required in @('SDGO Mod Manager.exe','rewards/tools/install_battle_rewards.py','rewards/owner/rewards/sql/battle_reward_wallet.sql','rewards/SHA256.json')) {
            if (-not $rewardArchive.GetEntry($required)) { throw "Selected reward package lacks $required" }
        }
        if ($rewardArchive.GetEntry('frp/install_frp.ps1')) { throw 'Reward-only package unexpectedly contains the FRP toolkit' }
    } finally { $rewardArchive.Dispose() }
    $fullZip = Invoke-Package 'Full' '' (Join-Path $work 'full')
    Check-Package $fullZip ($baseMods + @('match_ai','pvp_ai','mission_ai','coop_ai','room_random','room_titles','room_titles_agent','room_titles_server','battle_rewards','net_probe','net_route','agent_udp_pool'))
    $fullArchive = [IO.Compression.ZipFile]::OpenRead($fullZip)
    try {
        if (-not $fullArchive.GetEntry('frp/install_common.ps1') -or -not $fullArchive.GetEntry('frp/install_frp.ps1')) {
            throw 'Full package lost the complete incremental FRP toolkit'
        }
        if (-not $fullArchive.GetEntry('rewards/tools/install_battle_rewards.py') -or -not $fullArchive.GetEntry('SDGO Mod Manager.exe')) {
            throw 'Full package lost the complete reward owner toolkit'
        }
        if ($fullArchive.GetEntry('rewards/tools/reward_assistant.py') -or $fullArchive.GetEntry('rewards/tools/Start-Battle-Rewards.ps1') -or $fullArchive.GetEntry('rewards/tools/assistant_modules/rewards/ui.html')) { throw 'Retired reward UI leaked into the unified package' }
    } finally { $fullArchive.Dispose() }

    $baseFolder = Join-Path $work 'base-unpacked'; $customFolder = Join-Path $work 'custom-unpacked'
    [IO.Compression.ZipFile]::ExtractToDirectory($baseZip, $baseFolder)
    [IO.Compression.ZipFile]::ExtractToDirectory($customZip, $customFolder)
    $game = Join-Path $work 'game'
    New-FakeExe (Join-Path $game 'GOnline.exe') 0x54BE1568 0x00C97000
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $baseFolder 'install_client.ps1') -GameDir $game 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Packaged base installer failed: $output" }
    $ini = Join-Path $game 'sdgo\sdgo.ini'
    Set-Content -LiteralPath $ini -Value "[categories]`ngameplay=0`n[mods]`nime_fix=0" -Encoding Ascii
    $other = Join-Path $game 'sdgo\mods\unrelated\owner.json'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $other) | Out-Null
    Set-Content -LiteralPath $other -Value '{"keep":true}' -Encoding Ascii
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $customFolder 'install_client.ps1') -GameDir $game 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Packaged feature installer failed: $output" }
    if (-not (Test-Path -LiteralPath (Join-Path $game 'sdgo\mods\match_ai\match_ai.dll'))) { throw 'Feature client not installed' }
    if (-not ([IO.File]::ReadAllText($ini).Contains('gameplay=0')) -or -not (Test-Path -LiteralPath $other)) { throw 'Update lost user switches or unrelated mods' }
    $server = Join-Path $work 'server'
    New-FakeExe (Join-Path $server 'BattleServer.exe') 0x548A891D 0x0046C000
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $customFolder 'install_server.ps1') -ServerRoot $server 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Packaged server installer failed: $output" }
    if (-not (Test-Path -LiteralPath (Join-Path $server 'sdgo\mods\pvp_ai\pvp_ai.dll'))) { throw 'Feature server half not installed' }
    $titlesFolder = Join-Path $work 'titles-unpacked'
    [IO.Compression.ZipFile]::ExtractToDirectory($titleZip, $titlesFolder)
    $oldAi = Read-Sha256 (Join-Path $game 'sdgo\mods\match_ai\match_ai.dll')
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $titlesFolder 'install_client.ps1') -GameDir $game 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Packaged room-title client install failed: $output" }
    $titleIni = Join-Path $game 'sdgo\mods\room_titles\mod.ini'
    if (-not (Test-Path -LiteralPath $titleIni) -or [IO.File]::ReadAllText($titleIni) -notmatch '(?m)^enabled=0\s*$') { throw 'Room-title client must be installed disabled' }
    if ($oldAi -ne (Read-Sha256 (Join-Path $game 'sdgo\mods\match_ai\match_ai.dll')) -or
        -not ([IO.File]::ReadAllText($ini).Contains('gameplay=0')) -or -not (Test-Path -LiteralPath $other)) { throw 'Room-title install changed existing feature or user state' }
    [IO.File]::WriteAllText($titleIni, ([IO.File]::ReadAllText($titleIni) -replace '(?m)^enabled=0\s*$', 'enabled=1'))
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $titlesFolder 'install_client.ps1') -GameDir $game 2>&1
    if ($LASTEXITCODE -ne 0 -or [IO.File]::ReadAllText($titleIni) -notmatch '(?m)^enabled=1\s*$') { throw 'Room-title update lost the owner enable switch' }
    $agent = Join-Path $work 'agent'
    New-FakeExe (Join-Path $agent 'AgentServer.exe') 0x548A86BC 0x000D7000
    foreach ($item in @(@{Root=$server;Role='Battle';Id='room_titles_server';Other='room_titles_agent'}, @{Root=$agent;Role='Agent';Id='room_titles_agent';Other='room_titles_server'})) {
        $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $titlesFolder 'install_server.ps1') -ServerRoot $item.Root -Role $item.Role 2>&1
        if ($LASTEXITCODE -ne 0) { throw "Packaged room-title $($item.Role) install failed: $output" }
        if (-not (Test-Path -LiteralPath (Join-Path $item.Root "sdgo\mods\$($item.Id)\$($item.Id).dll"))) { throw 'Room-title server payload missing for selected host role' }
        # The framework's common server tree intentionally carries both hosts'
        # Mods. Runtime host manifests decide which DLL can load, as before.
        $otherManifest = Join-Path $item.Root "sdgo\mods\$($item.Other)\mod.ini"
        $wrongHost = if ($item.Role -eq 'Battle') { 'BattleServer-' } else { 'AgentServer-' }
        if ((Test-Path -LiteralPath $otherManifest) -and
            [IO.File]::ReadAllText($otherManifest) -match ('(?m)^hosts=' + $wrongHost)) { throw 'Room-title manifest allows the wrong server host' }
    }
    $world = Join-Path $work 'world'
    New-FakeExe (Join-Path $world 'WorldServer.exe') 0x548A86E3 0x0010C000
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $baseFolder 'install_server.ps1') -ServerRoot $world -Role World 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Packaged World installer failed: $output" }
    foreach ($file in @('winmm.dll','sdgo\core.dll','sdgo\mods\new_account_profile\new_account_profile.dll','sdgo\mods\new_account_profile\config.example.ini','sdgo\symbols\WorldServer-548A86E3-0010C000.ini')) {
        if (-not (Test-Path -LiteralPath (Join-Path $world $file))) { throw "World install lost $file" }
    }
    $rankFiles = @(Get-ChildItem -LiteralPath (Join-Path $world 'sdgo\mods\rank_progression_fix') -File | ForEach-Object { $_.Name } | Sort-Object)
    if (($rankFiles -join ',') -ne 'manager.json,mod.ini,rank_progression_fix.dll') { throw 'Rank repair is missing its unified Manager registration' }
    Write-Host 'BASE/FULL/FEATURE PACKAGES, CONTENTS AND PACKAGED INSTALLERS PASSED'
} finally {
    # Only remove the exact files this fixture introduced in the staged build.
    Remove-Item -LiteralPath $undeclared -Force
    Remove-Item -LiteralPath (Join-Path $stale 'old.dll') -Force
    Remove-Item -LiteralPath $stale -Force
}
