<#
.SYNOPSIS
  Runtime test of the real core and findwindow_fix against a simulated SDPlus hook (no game needed).

.DESCRIPTION
  Builds two throw-away folders next to findwindow_fix_test.exe and runs it in each:
    enabled   the fix loads: input DLLs get normal lookups, the game exe and SDPlus keep SDPlus's routine
    disabled  sdgo.ini [mods] findwindow_fix=0: every caller still gets SDPlus's routine
  Both also load export_hook_test, which checks HookSystemExport and then declines, so the core
  must roll its hook back. Uses a private desktop; no input method is activated.
  Build first: cmake --build build --config Release
#>
param([string]$Config = "Release", [string]$BuildDir = "build")

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }))
$binaries = Join-Path $buildRoot $Config
if (-not (Test-Path (Join-Path $binaries "findwindow_fix_test.exe"))) { throw "Build findwindow_fix_test first" }
$work = [IO.Path]::GetFullPath((Join-Path $buildRoot "findwindow_test_run"))
if (-not $work.StartsWith($buildRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe test directory" }
if (Test-Path $work) { Remove-Item $work -Recurse -Force }

foreach ($case in "enabled", "disabled") {
    $dir = Join-Path $work $case
    $modDir = Join-Path $dir "sdgo\mods\findwindow_fix"
    $apiDir = Join-Path $dir "sdgo\mods\export_hook_test"
    New-Item -ItemType Directory -Force -Path $modDir, $apiDir | Out-Null
    Copy-Item (Join-Path $binaries "findwindow_fix_test.exe") $dir
    Copy-Item (Join-Path $binaries "core.dll") (Join-Path $dir "sdgo")
    Copy-Item (Join-Path $binaries "findwindow_fix.dll") $modDir
    Copy-Item (Join-Path $binaries "export_hook_test_mod.dll") $apiDir
    Copy-Item (Join-Path $binaries "generic_input_method.dll") $dir
    Copy-Item (Join-Path $binaries "SDPlus.dll") $dir
    $manifest = (Get-Content (Join-Path $repo "mods\findwindow_fix\mod.ini") -Raw).Replace(
        "hosts=GOnline-54BE1568-00C97000", "hosts=*")
    Set-Content -LiteralPath (Join-Path $modDir "mod.ini") -Value $manifest -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $apiDir "mod.ini") -Encoding Ascii -Value @'
[mod]
id=export_hook_test
name=HookSystemExport rollback test
api=1
dll=export_hook_test_mod.dll
order=0
hosts=*
'@
    $ini = "[core]`r`nlog_level=1`r`n[mods]`r`n"
    if ($case -eq "disabled") { $ini += "findwindow_fix=0`r`n" }
    Set-Content -LiteralPath (Join-Path $dir "sdgo\sdgo.ini") -Value $ini -Encoding Ascii

    Write-Host "== $case"
    & (Join-Path $dir "findwindow_fix_test.exe") $case
    $exitCode = $LASTEXITCODE
    $log = Get-Content (Join-Path $dir "sdgo\logs\sdgo.log") -Raw
    if ($exitCode -ne 0 -or $log.Contains("TEST FAILURE") -or
        -not $log.Contains("room command API checks passed") -or
        -not $log.Contains("export API checks passed; deliberately decline for rollback")) {
        throw "case $case failed (exit $exitCode); see $dir"
    }
    if ($case -eq "enabled" -and -not $log.Contains("loaded FindWindowW fix 1.0.0")) { throw "the fix did not load; see $dir" }
    if ($case -eq "disabled" -and -not $log.Contains("findwindow_fix: disabled by configuration")) {
        throw "the disable switch did not apply; see $dir"
    }
}
Write-Host "ALL FINDWINDOW FIX CHECKS PASSED"
