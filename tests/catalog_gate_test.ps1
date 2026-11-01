param([string]$Config = 'Release', [string]$BuildDir = 'build')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildRoot = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }))
$bin = Join-Path $buildRoot $Config
$work = Join-Path $buildRoot ('catalog_gate_test_run\' + [guid]::NewGuid().ToString('N'))
$cases = @(
    @{ Name='defaults'; Ini=''; On=@('part_a','part_b','base_fix','legacy') },
    @{ Name='category-off'; Ini="[categories]`ngameplay=0"; On=@('base_fix','legacy') },
    @{ Name='feature-off'; Ini="[features]`nsample-feature=0"; On=@('base_fix','legacy') },
    @{ Name='one-mod-off'; Ini="[mods]`npart_a=0"; On=@('part_b','base_fix','legacy') },
    @{ Name='cannot-force-on'; Ini="[categories]`ngameplay=1`n[features]`nsample-feature=1`n[mods]`nmanual_tool=1"; On=@('part_a','part_b','base_fix','legacy') },
    @{ Name='fixes-off-legacy-compatible'; Ini="[categories]`nfixes=0"; On=@('part_a','part_b','legacy') },
    @{ Name='legacy-feature-id'; Ini="[features]`nlegacy=0"; On=@('part_a','part_b','base_fix') }
)
foreach ($case in $cases) {
    $dir = Join-Path $work $case.Name
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'sdgo\mods') | Out-Null
    Copy-Item -LiteralPath (Join-Path $bin 'loader_test.exe') -Destination $dir
    Copy-Item -LiteralPath (Join-Path $bin 'd3d9.dll') -Destination $dir
    Copy-Item -LiteralPath (Join-Path $bin 'core.dll') -Destination (Join-Path $dir 'sdgo')
    Set-Content -LiteralPath (Join-Path $dir 'sdgo\sdgo.ini') -Encoding Ascii -Value ("[core]`nlog_level=1`n" + $case.Ini)
    foreach ($id in @('part_a','part_b','base_fix','manual_tool','legacy')) {
        $modDir = Join-Path $dir "sdgo\mods\$id"
        New-Item -ItemType Directory -Force -Path $modDir | Out-Null
        Copy-Item -LiteralPath (Join-Path $bin 'catalog_gate_probe.dll') -Destination $modDir
        $metadata = if ($id -eq 'legacy') { '' } elseif ($id -eq 'base_fix') { "category=fixes`nfeature=foundation`n" } else { "category=gameplay`nfeature=sample-feature`n" }
        $enabled = if ($id -eq 'manual_tool') { 0 } else { 1 }
        Set-Content -LiteralPath (Join-Path $modDir 'mod.ini') -Encoding Ascii -Value "[mod]`nid=$id`nname=$id`napi=1`ndll=catalog_gate_probe.dll`nhosts=*`nenabled=$enabled`n$metadata"
    }
    $output = & (Join-Path $dir 'loader_test.exe')
    if ($LASTEXITCODE -ne 0) { throw "Synthetic host failed: $output" }
    $log = Get-Content -LiteralPath (Join-Path $dir 'sdgo\logs\sdgo.log') -Raw
    foreach ($id in @('part_a','part_b','base_fix','manual_tool','legacy')) {
        $loaded = $log.Contains("$id`: CATEGORY_GATE_PROBE_LOADED")
        if ($loaded -ne ($case.On -contains $id)) { throw "$($case.Name): unexpected load state for $id" }
    }
    Write-Host "PASS $($case.Name)"
}
Write-Host 'ALL CATEGORY/FEATURE GATE CHECKS PASSED (synthetic hosts, no game)'
