<# Verify data-only mod files are restaged by an incremental VS build. No game runs. #>
param([string]$Config = 'Release', [string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }))
$stage = Join-Path $buildRoot "stage\$Config\client\sdgo\mods\mission_ai"
$saved = @{}
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
        if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $actual).Hash) {
            throw "Incremental build did not restage $name"
        }
    }
    Write-Host 'MOD DATA RESTAGING CHECKS PASSED'
} finally {
    foreach ($name in $saved.Keys) { [IO.File]::WriteAllBytes((Join-Path $stage $name), $saved[$name]) }
}
