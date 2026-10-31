<#
.SYNOPSIS
  Runtime test of the d3d9.dll loader shim's forwarding (no game needed).

.DESCRIPTION
  Builds three throw-away folders next to loader_test.exe and runs it in each:
    1. no chain           -> forwards into the system d3d9.dll
    2. chain_d3d9 set     -> forwards into the chained DLL (a copy of the system d3d9.dll)
    3. chain to the shim  -> refuses the self-chain and falls back to the system d3d9.dll
  Build first: cmake --build build --config Release (target loader_test).
#>
param([string]$Config = "Release")

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $repo "build\$Config\loader_test.exe"
$stage = Join-Path $repo "build\stage\$Config\client"
$systemD3d9 = Join-Path $env:WINDIR "SysWOW64\d3d9.dll"
if (-not (Test-Path $exe)) { throw "Build loader_test first" }
$work = Join-Path $repo "build\loader_test_run"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }

function New-Case([string]$name, [string]$chain, [string]$chainFile) {
    $dir = Join-Path $work $name
    New-Item -ItemType Directory -Force -Path (Join-Path $dir "sdgo") | Out-Null
    Copy-Item $exe $dir
    Copy-Item (Join-Path $stage "d3d9.dll") $dir
    Copy-Item (Join-Path $stage "sdgo\core.dll") (Join-Path $dir "sdgo")
    $ini = "[core]`r`nlog_level=1`r`n[loader]`r`n"
    if ($chain) { $ini += "chain_d3d9=$chain`r`n" }
    Set-Content -Path (Join-Path $dir "sdgo\sdgo.ini") -Value $ini -Encoding Ascii
    if ($chainFile) { Copy-Item $systemD3d9 (Join-Path $dir $chainFile) }
    $dir
}

$failures = 0
# Expect the shim ([...\<case>\d3d9.dll]) plus exactly the forward target named here.
function Check([string]$name, [string]$dir, [string]$expected, [string]$absent) {
    $output = & (Join-Path $dir "loader_test.exe")
    $ok = ($LASTEXITCODE -eq 0) -and ($output -like "*$expected]*")
    if ($absent) { $ok = $ok -and -not ($output -like "*$absent]*") }
    Write-Host ("{0} {1}: {2}" -f ($(if ($ok) { "ok  " } else { "FAIL" })), $name, $output)
    if (-not $ok) { $script:failures++ }
}

# A 32-bit process sees the system copy as ...\system32\d3d9.dll (WOW64 file redirection).
Check "no chain" (New-Case "plain" "" "") "\system32\d3d9.dll" "d3d9_chain.dll"
Check "chain_d3d9=d3d9_chain.dll" (New-Case "chained" "d3d9_chain.dll" "d3d9_chain.dll") "\chained\d3d9_chain.dll" "\system32\d3d9.dll"
Check "chain_d3d9=d3d9.dll (itself)" (New-Case "selfchain" "d3d9.dll" "") "\system32\d3d9.dll" ""

$log = Join-Path $work "plain\sdgo\logs\sdgo.log"
if ((Test-Path $log) -and ((Get-Content $log -Raw) -match "core started by d3d9 loader")) {
    Write-Host "ok   the shim started core.dll (sdgo\logs\sdgo.log)"
} else {
    Write-Host "FAIL the shim did not start core.dll"
    $failures++
}
Remove-Item $work -Recurse -Force
if ($failures) { throw "$failures loader check(s) failed" }
Write-Host "ALL LOADER CHECKS PASSED"
