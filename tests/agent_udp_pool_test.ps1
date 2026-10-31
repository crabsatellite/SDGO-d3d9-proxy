<#
.SYNOPSIS
  Actual-core integration test for agent_udp_pool.dll (no AgentServer needed).

.DESCRIPTION
  Each isolated process writes a host-specific symbol file for its exact x86
  FF 15 bind-IAT fixture before loading core.dll:
    valid    exhausts a three-port pool, proves no fallback, wrap/reuse,
             getsockname advertisement, caller storage, call-site and type gates
    invalid  uses FirstPort=65535, PortCount=2 and verifies that the mod declines
  Build first: cmake --build build --config Release --target
               agent_udp_pool_test agent_udp_pool sdgo_core
#>
param([string]$Config = "Release")

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$binaries = Join-Path $repo "build\$Config"
foreach ($name in "agent_udp_pool_test.exe", "agent_udp_pool.dll", "core.dll") {
    if (-not (Test-Path -LiteralPath (Join-Path $binaries $name))) {
        throw "Missing build\$Config\$name; build the agent_udp_pool integration-test targets first"
    }
}

$buildRoot = [IO.Path]::GetFullPath((Join-Path $repo "build"))
$stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssfffZ")
$suffix = [Guid]::NewGuid().ToString("N").Substring(0, 8)
$work = [IO.Path]::GetFullPath((Join-Path $buildRoot "agent_udp_pool_test_run\$stamp-$suffix"))
if (-not $work.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use a test directory outside the repository build tree"
}

function New-TestCase([string]$Name) {
    $dir = Join-Path $work $Name
    $modDir = Join-Path $dir "sdgo\mods\agent_udp_pool"
    New-Item -ItemType Directory -Force -Path $modDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $binaries "agent_udp_pool_test.exe") -Destination $dir
    $coreFile=Join-Path $binaries 'core.dll'
    Copy-Item -LiteralPath $coreFile -Destination (Join-Path $dir "sdgo\core.dll")
    Copy-Item -LiteralPath (Join-Path $binaries "agent_udp_pool.dll") -Destination $modDir

    $manifest = Get-Content -LiteralPath (Join-Path $repo "mods\agent_udp_pool\mod.ini") -Raw
    $manifest = $manifest -replace '(?m)^enabled=0\s*$', 'enabled=1'
    $manifest = $manifest -replace '(?m)^hosts=.*$', 'hosts=*'
    Set-Content -LiteralPath (Join-Path $modDir "mod.ini") -Value $manifest -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $dir "sdgo\sdgo.ini") -Encoding Ascii -Value @'
[core]
log_level=0
'@
    return $dir
}

function Invoke-TestCase([string]$Name, [string]$Mode) {
    $dir = New-TestCase $Name
    Write-Host "== $Name"
    $output = & (Join-Path $dir "agent_udp_pool_test.exe") $Mode 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }
    $logPath = Join-Path $dir "sdgo\logs\sdgo.log"
    $log = if (Test-Path -LiteralPath $logPath) { Get-Content -LiteralPath $logPath -Raw } else { "" }
    if ($exitCode -ne 0 -or ($output -join "`n") -notmatch "ALL AGENT UDP POOL CHECKS PASSED") {
        throw "case $Name failed (exit $exitCode); see $dir"
    }
    if ($Name -in @("valid", "config")) {
        if (-not $log.Contains("native per-user UDP pool") -or
            -not $log.Contains("loaded Agent native UDP port pool 0.2.0") -or
            -not $log.Contains("no usable UDP pool port")) {
            throw "valid case did not exercise activation and bounded exhaustion; see $logPath"
        }
    } elseif ($Name -eq "invalid") {
        if (-not $log.Contains("need FirstPort=1024..65535") -or
            -not $log.Contains("agent_udp_pool: declined to load")) {
            throw "invalid case did not reject the overflowing configured range; see $logPath"
        }
    } else {
        if (-not $log.Contains("need agent_relay_log_fix active") -or
            -not $log.Contains("agent_udp_pool: declined to load")) {
            throw "unsafe-format case did not reject an uncorrected native logger; see $logPath"
        }
    }
}

Invoke-TestCase "valid" "valid"
Invoke-TestCase "config" "config"
Invoke-TestCase "invalid" "invalid"
Invoke-TestCase "unsafe" "unsafe"
Write-Host "ALL REAL AGENT UDP POOL INTEGRATION CASES PASSED"
Write-Host "Receipt: $work"
