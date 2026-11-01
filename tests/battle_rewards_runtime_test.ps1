<#
.SYNOPSIS
  Loads the real core.dll and battle_rewards.dll in an isolated x86 host.

.DESCRIPTION
  The basic case exercises all ten native settlement sites, the verified
  effective-damage hook, native G replacement, the real Python WalletWorker,
  fresh 0x064B balance notification, and per-match policy freezing.

  The daily case additionally loads the real daily_rewards.dll and verifies
  that its configured 2x window composes once with the new reward policy while
  pilot/unit EXP keep the daily mod's native behavior.
#>
param(
    [string]$Config = "Release",
    [string]$BuildDir = "",
    [string]$DailyRewardsDll = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) {
    $BuildDir = Join-Path $repo "build"
}
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$binaries = Join-Path $BuildDir $Config
$required = @(
    "battle_rewards_runtime_test.exe",
    "battle_rewards.dll",
    "core.dll"
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $binaries $name))) {
        throw "Missing $binaries\$name; build the runtime fixture targets first"
    }
}
if (-not $DailyRewardsDll) {
    $DailyRewardsDll = Join-Path $binaries "daily_rewards.dll"
}
$DailyRewardsDll = [IO.Path]::GetFullPath($DailyRewardsDll)
if (-not (Test-Path -LiteralPath $DailyRewardsDll)) {
    throw "Missing real daily_rewards.dll: $DailyRewardsDll"
}

$buildRoot = [IO.Path]::GetFullPath($BuildDir)
$stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssfffZ")
$suffix = [Guid]::NewGuid().ToString("N").Substring(0, 8)
$work = [IO.Path]::GetFullPath(
    (Join-Path $buildRoot "battle_rewards_runtime_test_run\$stamp-$suffix")
)
$runRoot = [IO.Path]::GetFullPath(
    (Join-Path $buildRoot "battle_rewards_runtime_test_run")
)
if (-not $work.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use a test directory outside the selected build tree"
}

$battleRequires = @(
    "battle.rewards.augment.personal",
    "battle.rewards.augment.team",
    "battle.rewards.augment.leader",
    "battle.rewards.augment.ai",
    "battle.rewards.augment.ai_ship",
    "battle.rewards.augment.mode13",
    "battle.rewards.augment.mode14",
    "battle.rewards.augment.tag",
    "battle.rewards.augment.death_grid",
    "battle.rewards.augment.mission",
    "battle.rewards.record_damage",
    "battle.rewards.layout.personal",
    "battle.rewards.layout.team",
    "battle.rewards.layout.leader",
    "battle.rewards.layout.ai",
    "battle.rewards.layout.ai_ship",
    "battle.rewards.layout.mode13",
    "battle.rewards.layout.mode14",
    "battle.rewards.layout.tag",
    "battle.rewards.layout.death_grid",
    "battle.rewards.layout.mission",
    "battle.rewards.net_send",
    "battle.rewards.net_frame_proc"
) -join ","

$dailyNames = @(
    "personal", "team", "leader", "ai", "ai_ship", "mode13", "mode14",
    "tag", "death_grid", "mission"
)
$dailyRequires = @(
    $dailyNames | ForEach-Object { "battle.reward.layout.$_" }
    $dailyNames | ForEach-Object { "battle.reward.final.$_" }
) -join ","

function Get-DailyWindow {
    $now = Get-Date
    $minute = $now.Hour * 60 + $now.Minute
    $start = ($minute + 1439) % 1440
    $end = ($minute + 2) % 1440
    return @(
        ("{0:D2}:{1:D2}" -f [int][Math]::Floor($start / 60), [int]($start % 60)),
        ("{0:D2}:{1:D2}" -f [int][Math]::Floor($end / 60), [int]($end % 60))
    )
}

function New-Case([string]$Name, [bool]$WithDaily) {
    $case = Join-Path $work $Name
    $sdgo = Join-Path $case "sdgo"
    $battle = Join-Path $sdgo "mods\battle_rewards"
    New-Item -ItemType Directory -Force -Path $battle | Out-Null
    Copy-Item -LiteralPath (Join-Path $binaries "battle_rewards_runtime_test.exe") -Destination $case
    Copy-Item -LiteralPath (Join-Path $binaries "core.dll") -Destination $sdgo
    Copy-Item -LiteralPath (Join-Path $binaries "battle_rewards.dll") -Destination $battle
    $runtime = [IO.Path]::GetFullPath((Join-Path $case "runtime"))
    $battleManifest = @"
[mod]
id=battle_rewards
category=gameplay
feature=battle-rewards
channel=beta
activation=manual
name=Battle contribution rewards (Beta)
version=runtime-fixture
api=1
dll=battle_rewards.dll
order=110
enabled=1
hosts=*
requires=$battleRequires

[settings]
RuntimeDirectory=$runtime
InstanceId=battle-fixture
Trace=1
"@
    Set-Content -LiteralPath (Join-Path $battle "mod.ini") -Value $battleManifest -Encoding Ascii
    if ($WithDaily) {
        $daily = Join-Path $sdgo "mods\daily_rewards"
        New-Item -ItemType Directory -Force -Path $daily | Out-Null
        Copy-Item -LiteralPath $DailyRewardsDll -Destination $daily
        $window = Get-DailyWindow
        $dailyManifest = @"
[mod]
id=daily_rewards
category=gameplay
feature=daily-rewards
channel=beta
activation=automatic
name=Daily double battle rewards (Beta)
version=runtime-fixture
api=1
dll=daily_rewards.dll
order=100
enabled=1
hosts=*
requires=$dailyRequires

[settings]
StartTime=$($window[0])
EndTime=$($window[1])
Trace=1
"@
        Set-Content -LiteralPath (Join-Path $daily "mod.ini") -Value $dailyManifest -Encoding Ascii
    }
    Set-Content -LiteralPath (Join-Path $sdgo "sdgo.ini") -Encoding Ascii -Value @"
[core]
log_level=0
"@
    return @{
        Case = $case
        Runtime = $runtime
        Exe = Join-Path $case "battle_rewards_runtime_test.exe"
    }
}

$allPassed = $false
try {
    foreach ($entry in @(
        @{ Name = "basic"; Daily = $false },
        @{ Name = "daily"; Daily = $true }
    )) {
        $fixture = New-Case $entry.Name $entry.Daily
        Write-Host "== real core battle rewards $($entry.Name)"
        $output = & python -B (Join-Path $PSScriptRoot "battle_rewards_runtime_worker.py") `
            --runtime $fixture.Runtime --exe $fixture.Exe --mode $entry.Name 2>&1
        $exitCode = $LASTEXITCODE
        $output | ForEach-Object { Write-Host $_ }
        $logPath = Join-Path $fixture.Case "sdgo\logs\sdgo.log"
        $log = if (Test-Path -LiteralPath $logPath) {
            Get-Content -LiteralPath $logPath -Raw
        } else {
            ""
        }
        if ($exitCode -ne 0) {
            throw "runtime case $($entry.Name) failed with exit $exitCode"
        }
        if (-not $log.Contains("loaded Battle contribution rewards (Beta) runtime-fixture") -or
            -not $log.Contains("11 code hooks switched on") -and -not $entry.Daily) {
            throw "battle_rewards did not load through the real core in $($entry.Name)"
        }
        if ($entry.Daily -and
            (-not $log.Contains("loaded Daily double battle rewards (Beta) runtime-fixture") -or
             -not $log.Contains("21 code hooks switched on"))) {
            throw "daily_rewards coexistence did not load through the real core"
        }
    }
    Write-Host "ALL ISOLATED BATTLE REWARD RUNTIME CASES PASSED"
    $allPassed = $true
}
finally {
    $resolved = [IO.Path]::GetFullPath($work)
    if ($allPassed -and
        $resolved.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolved)) {
        for ($attempt = 0; $attempt -lt 5; $attempt++) {
            try {
                Remove-Item -LiteralPath $resolved -Recurse -Force
                break
            }
            catch {
                if ($attempt -eq 4) { throw }
                Start-Sleep -Milliseconds 200
            }
        }
        if (Test-Path -LiteralPath $runRoot) {
            foreach ($old in Get-ChildItem -LiteralPath $runRoot -Directory) {
                $oldPath = [IO.Path]::GetFullPath($old.FullName)
                if (-not $oldPath.StartsWith(
                        $runRoot + [IO.Path]::DirectorySeparatorChar,
                        [StringComparison]::OrdinalIgnoreCase)) {
                    throw "Refusing to clean an unexpected runtime fixture path"
                }
                Remove-Item -LiteralPath $oldPath -Recurse -Force
            }
        }
    }
    elseif (Test-Path -LiteralPath $resolved) {
        Write-Host "Failed fixture retained for diagnosis: $resolved"
    }
}
