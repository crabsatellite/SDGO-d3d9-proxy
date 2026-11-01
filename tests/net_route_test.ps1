<#
.SYNOPSIS
  Runtime integration test of the real core.dll and net_route.dll (no game needed).

.DESCRIPTION
  Runs six isolated receipt cases:
    routes    verifies two logical TCP aliases can share one external listener,
              a third TCP endpoint reaches an independent listener, plus UDP mapping
    rollback  a test mod owns sendto first; net_route must decline and the core
              must roll back the connect hook that net_route already installed
    invalid   a zero logical port is rejected before any hooks are installed
    duplicate identical routes are rejected before any hooks are installed
    udp_alias two logical UDP endpoints sharing one reverse endpoint are rejected
    disabled  sdgo.ini [mods] disables net_route and leaves Winsock untouched
  Build first: cmake --build build --config Release --target net_route_test
               net_route_conflict_mod net_route sdgo_core
#>
param([string]$Config = "Release")

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$binaries = Join-Path $repo "build\$Config"
$required = @(
    "net_route_test.exe",
    "net_route_conflict_mod.dll",
    "net_route.dll",
    "core.dll"
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $binaries $name))) {
        throw "Missing build\$Config\$name; build the net_route integration-test targets first"
    }
}

$buildRoot = [IO.Path]::GetFullPath((Join-Path $repo "build"))
$stamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssfffZ")
$suffix = [Guid]::NewGuid().ToString("N").Substring(0, 8)
$work = [IO.Path]::GetFullPath((Join-Path $buildRoot "net_route_test_run\$stamp-$suffix"))
if (-not $work.StartsWith($buildRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to use a test directory outside the repository build tree"
}

function New-TestCase([string]$Name, [bool]$Conflict, [bool]$Disabled) {
    $dir = Join-Path $work $Name
    $netRouteDir = Join-Path $dir "sdgo\mods\net_route"
    New-Item -ItemType Directory -Force -Path $netRouteDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $binaries "net_route_test.exe") -Destination $dir
    $coreFile=Join-Path $binaries 'core.dll'
    Copy-Item -LiteralPath $coreFile -Destination (Join-Path $dir "sdgo\core.dll")
    Copy-Item -LiteralPath (Join-Path $binaries "net_route.dll") -Destination $netRouteDir

    $manifest = Get-Content -LiteralPath (Join-Path $repo "mods\net_route\mod.ini") -Raw
    $manifest = $manifest -replace '(?m)^enabled=0\s*$', 'enabled=1'
    $manifest = $manifest -replace '(?m)^hosts=.*$', 'hosts=*'
    Set-Content -LiteralPath (Join-Path $netRouteDir "mod.ini") -Value $manifest -Encoding Ascii
    $globalIni = @'
[core]
log_level=0
'@
    if ($Disabled) {
        $globalIni += "`r`n[mods]`r`nnet_route=0`r`n"
    }
    Set-Content -LiteralPath (Join-Path $dir "sdgo\sdgo.ini") -Encoding Ascii -Value $globalIni

    if ($Conflict) {
        $conflictDir = Join-Path $dir "sdgo\mods\net_route_conflict"
        New-Item -ItemType Directory -Force -Path $conflictDir | Out-Null
        Copy-Item -LiteralPath (Join-Path $binaries "net_route_conflict_mod.dll") -Destination $conflictDir
        Set-Content -LiteralPath (Join-Path $conflictDir "mod.ini") -Encoding Ascii -Value @'
[mod]
id=net_route_conflict
name=Network route conflict fixture
version=1
api=1
dll=net_route_conflict_mod.dll
order=0
enabled=1
hosts=*
'@
    }
    return $dir
}

function Invoke-TestCase([string]$Name, [string]$Mode, [bool]$Conflict, [bool]$Disabled) {
    $dir = New-TestCase $Name $Conflict $Disabled
    Write-Host "== $Name"
    $output = & (Join-Path $dir "net_route_test.exe") $Mode 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }
    $logPath = Join-Path $dir "sdgo\logs\sdgo.log"
    $log = if (Test-Path -LiteralPath $logPath) { Get-Content -LiteralPath $logPath -Raw } else { "" }
    if ($exitCode -ne 0 -or ($output -join "`n") -notmatch "ALL NET ROUTE CHECKS PASSED") {
        throw "case $Name failed (exit $exitCode); see $dir"
    }
    switch ($Name) {
        "routes" {
            if (-not $log.Contains("endpoint translation active (4 routes)") -or
                -not $log.Contains("loaded Network endpoint routes 0.2.0")) {
                throw "routes case did not activate the real net_route mod; see $logPath"
            }
        }
        "rollback" {
            if (-not $log.Contains("test conflict owns ws2_32.dll!sendto") -or
                -not $log.Contains("hook ws2_32.dll!sendto failed: site already hooked by net_route_conflict") -or
                -not $log.Contains("net_route: declined to load")) {
                throw "rollback case did not exercise the expected hook conflict; see $logPath"
            }
        }
        "invalid" {
            if (-not $log.Contains("Route1 invalid") -or -not $log.Contains("net_route: declined to load")) {
                throw "invalid case did not reject Route1 before installing hooks; see $logPath"
            }
        }
        "duplicate" {
            if (-not $log.Contains("Route2 conflicts with Route1") -or
                -not $log.Contains("net_route: declined to load")) {
                throw "duplicate case did not reject the conflicting route; see $logPath"
            }
        }
        "udp_alias" {
            if (-not $log.Contains("Route3 conflicts with Route2 (logical or reverse endpoint)") -or
                -not $log.Contains("net_route: declined to load")) {
                throw "udp_alias case did not reject the ambiguous UDP reverse endpoint; see $logPath"
            }
        }
        "disabled" {
            if (-not $log.Contains("net_route: disabled by configuration")) {
                throw "disabled case did not honor sdgo.ini [mods]; see $logPath"
            }
        }
    }
}

Invoke-TestCase "routes" "routes" $false $false
Invoke-TestCase "rollback" "rollback" $true $false
Invoke-TestCase "invalid" "invalid" $false $false
Invoke-TestCase "duplicate" "duplicate" $false $false
Invoke-TestCase "udp_alias" "udp_alias" $false $false
Invoke-TestCase "disabled" "disabled" $false $true
Write-Host "ALL REAL NET ROUTE INTEGRATION CASES PASSED"
Write-Host "Receipt: $work"
