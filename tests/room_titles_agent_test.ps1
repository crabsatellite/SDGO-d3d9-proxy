<#
.SYNOPSIS
  Actual-core integration test for room_titles_agent.dll (no AgentServer needed).

.DESCRIPTION
  Runs three isolated processes with generated symbols for the executable's
  hookable native-style Agent dispatcher and SendToServer functions:
    enabled   validates extension restoration, strict rejection and pass-through
              gates, native field preservation, TLS isolation and nesting
    disabled  proves the disabled Mod leaves the native oversized path intact
    rollback  a test Mod owns the second hook first; room_titles_agent must
              decline and the core must roll its first hook back
  Build first: cmake --build build --config Release --target
               room_titles_agent_test room_titles_agent_conflict_mod
               room_titles_agent sdgo_core
#>
param(
    [string]$Config = "Release",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    [IO.Path]::GetFullPath((Join-Path $repo "build"))
} elseif ([IO.Path]::IsPathRooted($BuildDir)) {
    [IO.Path]::GetFullPath($BuildDir)
} else {
    [IO.Path]::GetFullPath((Join-Path $repo $BuildDir))
}
$binaries = Join-Path $buildRoot $Config
$required = @(
    "room_titles_agent_test.exe",
    "room_titles_agent_conflict_mod.dll",
    "room_titles_agent.dll",
    "core.dll"
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $binaries $name))) {
        throw "Missing build\$Config\$name; build the room-title Agent integration targets first"
    }
}

$stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssfffZ")
$suffix = [Guid]::NewGuid().ToString("N").Substring(0, 8)
$work = [IO.Path]::GetFullPath(
    (Join-Path $buildRoot "room_titles_agent_test_run\$stamp-$suffix"))
if (-not $work.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use a test directory outside the repository build tree"
}

function New-TestCase(
    [string]$Name,
    [bool]$Enabled,
    [bool]$Conflict
) {
    $dir = Join-Path $work $Name
    $modDir = Join-Path $dir "sdgo\mods\room_titles_agent"
    New-Item -ItemType Directory -Force -Path $modDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $binaries "room_titles_agent_test.exe") -Destination $dir
    Copy-Item -LiteralPath (Join-Path $binaries "core.dll") -Destination (Join-Path $dir "sdgo\core.dll")
    Copy-Item -LiteralPath (Join-Path $binaries "room_titles_agent.dll") -Destination $modDir

    $manifest = Get-Content -LiteralPath (Join-Path $repo "mods\room_titles_agent\mod.ini") -Raw
    $manifest = $manifest -replace '(?m)^enabled=\d+\s*$', ('enabled=' + $(if ($Enabled) { '1' } else { '0' }))
    $manifest = $manifest -replace '(?m)^hosts=.*$', 'hosts=*'
    Set-Content -LiteralPath (Join-Path $modDir "mod.ini") -Value $manifest -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $dir "sdgo\sdgo.ini") -Encoding Ascii -Value @'
[core]
log_level=0
'@

    if ($Conflict) {
        $conflictDir = Join-Path $dir "sdgo\mods\room_titles_agent_conflict"
        New-Item -ItemType Directory -Force -Path $conflictDir | Out-Null
        Copy-Item -LiteralPath (Join-Path $binaries "room_titles_agent_conflict_mod.dll") -Destination $conflictDir
        Set-Content -LiteralPath (Join-Path $conflictDir "mod.ini") -Encoding Ascii -Value @'
[mod]
id=room_titles_agent_conflict
name=Room titles Agent hook conflict fixture
version=1
api=1
dll=room_titles_agent_conflict_mod.dll
order=0
enabled=1
hosts=*
requires=room_titles.agent_send_server
'@
    }
    return $dir
}

function Invoke-TestCase(
    [string]$Name,
    [string]$Mode,
    [bool]$Enabled,
    [bool]$Conflict
) {
    $dir = New-TestCase $Name $Enabled $Conflict
    Write-Host "== $Name"
    $output = & (Join-Path $dir "room_titles_agent_test.exe") $Mode 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }
    $logPath = Join-Path $dir "sdgo\logs\sdgo.log"
    $log = if (Test-Path -LiteralPath $logPath) {
        Get-Content -LiteralPath $logPath -Raw
    } else {
        ""
    }
    if ($exitCode -ne 0 -or
        ($output -join "`n") -notmatch "ALL ROOM TITLES AGENT CHECKS PASSED") {
        throw "case $Name failed (exit $exitCode); see $dir"
    }

    switch ($Name) {
        "enabled" {
            if (-not $log.Contains("ready: preserve validated room-title metadata") -or
                -not $log.Contains("loaded Room titles transport (Agent) 0.1.0") -or
                -not $log.Contains("4 code hooks switched on")) {
                throw "enabled case did not activate all four real room-title hooks; see $logPath"
            }
        }
        "disabled" {
            if (-not $log.Contains("room_titles_agent: disabled by configuration") -or
                -not $log.Contains("0 code hooks switched on")) {
                throw "disabled case did not leave both hook sites native; see $logPath"
            }
        }
        "rollback" {
            if (-not $log.Contains("test conflict owns room_titles.agent_send_server") -or
                -not $log.Contains("hook room_titles.agent_send_server failed: site already hooked by room_titles_agent_conflict") -or
                -not $log.Contains("room_titles_agent: declined to load") -or
                -not $log.Contains("1 code hooks switched on")) {
                throw "rollback case did not exercise the expected second-hook conflict and first-hook rollback; see $logPath"
            }
        }
    }
}

Invoke-TestCase "enabled" "enabled" $true $false
Invoke-TestCase "disabled" "disabled" $false $false
Invoke-TestCase "rollback" "rollback" $true $true
Write-Host "ALL REAL ROOM TITLES AGENT INTEGRATION CASES PASSED"
Write-Host "Receipt: $work"
