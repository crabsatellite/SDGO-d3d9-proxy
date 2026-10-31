<# Verify data-only mod files are restaged by an incremental VS build. No game runs. #>
param([string]$Config = 'Release', [string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }))
$stage = Join-Path $buildRoot "stage\$Config\client\sdgo\mods\mission_ai"
$saved = @{}
function Hash([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([IO.File]::ReadAllBytes($Path))) }
    finally { $sha.Dispose() }
}
try {
    foreach ($name in @('mod.ini', 'mission_ai.lua')) {
        $path = Join-Path $stage $name
        $saved[$name] = [IO.File]::ReadAllBytes($path)
        [IO.File]::WriteAllText($path, 'stale staging fixture')
    }
    $output = & cmake --build $buildRoot --config $Config --target mission_ai 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Incremental build failed: $output" }
    foreach ($name in @('mod.ini', 'mission_ai.lua')) {
        $source = Join-Path $repo "mods\mission_ai\$name"
        $actual = Join-Path $stage $name
        if ((Hash $source) -ne (Hash $actual)) {
            throw "Incremental build did not restage $name"
        }
    }
    Write-Host 'MOD DATA RESTAGING CHECKS PASSED'
    foreach ($role in 'client', 'server') {
        $actual = Join-Path $buildRoot "stage\$Config\$role\sdgo\mods\ai_catalog\mod.ini"
        $saved[$actual] = [IO.File]::ReadAllBytes($actual)
        [IO.File]::WriteAllText($actual, 'stale catalog staging fixture')
    }
    $output = & cmake --build $buildRoot --config $Config --target ai_catalog_data 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Catalog data staging failed: $output" }
    foreach ($role in 'client', 'server') {
        if ((Hash (Join-Path $repo "mods\ai_catalog\$role.ini")) -ne
            (Hash (Join-Path $buildRoot "stage\$Config\$role\sdgo\mods\ai_catalog\mod.ini"))) {
            throw "Catalog $role manifest was not restaged"
        }
    }
    Write-Host 'CATALOG DATA RESTAGING CHECKS PASSED'
} finally {
    foreach ($name in $saved.Keys) {
        $path = if ([IO.Path]::IsPathRooted($name)) { $name } else { Join-Path $stage $name }
        [IO.File]::WriteAllBytes($path, $saved[$name])
    }
}
