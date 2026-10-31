<#
.SYNOPSIS
Build and run the offline AI, HUD, loader and installer tests without starting a game.
.DESCRIPTION
For the Lua simulation, pass -LuaSource pointing at extracted official Lua 5.0.3
sources, or put them at <BuildDir>/deps/lua-5.0.3. -NativeOnly explicitly skips it.
All outputs stay in the selected build tree; no installation or deployment runs.
#>
param(
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
    [string]$BuildDir = 'build\ai_review',
    [string]$LuaSource,
    [switch]$NativeOnly
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }))
if (-not $LuaSource) { $LuaSource = Join-Path $buildRoot 'deps\lua-5.0.3' }
if ($NativeOnly) {
    $LuaSource = ''
    Write-Host 'Lua simulation explicitly skipped (-NativeOnly).'
} else {
    $LuaSource = [IO.Path]::GetFullPath($LuaSource)
    if (-not (Test-Path -LiteralPath (Join-Path $LuaSource 'include\lua.h'))) {
        throw 'Pass -LuaSource <official Lua 5.0.3 source directory>, or use -NativeOnly to explicitly skip the Lua simulation.'
    }
}
& cmake -S $repo -B $buildRoot -G 'Visual Studio 17 2022' -A Win32 "-DSDGO_LUA50_SOURCE=$LuaSource" -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
& cmake --build $buildRoot --config $Config --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& ctest --test-dir $buildRoot -C $Config --output-on-failure --output-log (Join-Path $buildRoot "$Config-tests.log")
if ($LASTEXITCODE -ne 0) { throw 'Offline tests failed' }
