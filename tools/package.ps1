<# Build a full, base-fixes or selected-feature package from a staged build. #>
param(
    [string]$Config = 'Release',
    [string]$BuildDir = 'build',
    [ValidateSet('Full','Base')][string]$Edition = 'Full',
    [string[]]$Features = @(),
    [string]$OutputDir
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repo 'VERSION') -TotalCount 1).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+([+-][a-zA-Z0-9.-]+)?$') { throw 'Invalid package version' }
$buildRoot = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }))
$stage = Join-Path $buildRoot "stage\$Config"
$requested = @(($Features -join ',') -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ } | Sort-Object -Unique)
$catalogArgs = @((Join-Path $PSScriptRoot 'mod_catalog.py'), '--root', $repo, '--stage', $stage, '--profile', $Edition.ToLowerInvariant())
if ($requested.Count) { $catalogArgs += @('--features', ($requested -join ',')) }
$catalogText = & python @catalogArgs
if ($LASTEXITCODE -ne 0) { throw 'Mod catalog/payload validation failed; build the selected mods first' }
$catalog = ($catalogText -join "`n") | ConvertFrom-Json
$suffix = if ($requested.Count) { '-Features-' + ($requested -join '-') } elseif ($Edition -eq 'Base') { '-Base' } else { '' }
$name = "SDGO-ModFramework-$version$suffix"
if (-not $OutputDir) { $OutputDir = Join-Path $repo 'dist' }
$dist = [IO.Path]::GetFullPath($OutputDir)
$out = [IO.Path]::GetFullPath((Join-Path $dist $name))
if (-not $out.StartsWith($dist.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe package output path' }
$zip = Join-Path $dist "$name.zip"
if ((Test-Path -LiteralPath $out) -or (Test-Path -LiteralPath $zip)) {
    throw 'Package output already exists; choose another -OutputDir or increment VERSION.'
}
New-Item -ItemType Directory -Force -Path $out | Out-Null
$manager = Join-Path $buildRoot 'manager\SDGO Mod Manager.exe'
if (-not (Test-Path -LiteralPath $manager)) { throw 'Build the unified Mod Manager before packaging' }
Copy-Item -LiteralPath $manager -Destination $out

# Core files are always present. Only catalog-owned, complete mod payloads enter
# the archive, so stale build folders cannot silently add features or user files.
foreach ($tree in @('client','server')) {
    $dest = Join-Path $out $tree
    New-Item -ItemType Directory -Force -Path (Join-Path $dest 'sdgo\mods'), (Join-Path $dest 'sdgo\symbols') | Out-Null
    $loader = if ($tree -eq 'client') { 'd3d9.dll' } else { 'winmm.dll' }
    foreach ($file in @($loader,'sdgo\core.dll','sdgo\sdgo.ini')) {
        Copy-Item -LiteralPath (Join-Path $stage "$tree\$file") -Destination (Join-Path $dest $file)
    }
    Get-ChildItem -LiteralPath (Join-Path $stage "$tree\sdgo\symbols") -File -Filter '*.ini' | Copy-Item -Destination (Join-Path $dest 'sdgo\symbols')
    [IO.File]::WriteAllText((Join-Path $dest 'sdgo\catalog.json'), ($catalogText -join "`n"), (New-Object Text.UTF8Encoding $false))
}
foreach ($mod in $catalog.mods) {
    $relative = "$($mod.target)\sdgo\mods\$($mod.id)"
    $destination = Join-Path $out $relative
    foreach ($file in @("$($mod.id).dll",'mod.ini') + @($mod.files)) {
        $target = Join-Path $destination $file
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        Copy-Item -LiteralPath (Join-Path $stage "$relative\$file") -Destination $target
    }
}
foreach ($file in @('install_client.ps1','install_server.ps1','install_common.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $out
}
foreach ($file in @('README.md','README.zh-CN.md','THIRD_PARTY_NOTICES.md','LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $repo $file) -Destination $out
}
Copy-Item -LiteralPath (Join-Path $repo 'docs') -Destination $out -Recurse
New-Item -ItemType Directory -Force -Path (Join-Path $out 'licenses') | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'third_party\minhook\LICENSE.txt') -Destination (Join-Path $out 'licenses\MinHook-LICENSE.txt')
$contents = @('# Included features', '', '| Category | Feature | Channel | Mods |', '|---|---|---|---|')
foreach ($feature in $catalog.features.PSObject.Properties) {
    $item = $feature.Value
    $contents += "| $($item.category) | $($feature.Name) | $($item.channel) | $($item.mods -join ', ') |"
}
[IO.File]::WriteAllText((Join-Path $out 'CONTENTS.md'), ($contents -join "`n"), (New-Object Text.UTF8Encoding $false))
if (@($catalog.features.PSObject.Properties | Where-Object { $_.Value.toolkit -eq 'frp' }).Count) {
    & python (Join-Path $PSScriptRoot 'package_frp.py') --out (Join-Path $out 'frp') --binaries (Join-Path $buildRoot $Config) --directory-only
    if ($LASTEXITCODE -ne 0) { throw 'FRP toolkit packaging failed' }
}
if ($catalog.features.PSObject.Properties.Name -contains 'battle-rewards') {
    & python (Join-Path $PSScriptRoot 'package_rewards.py') --out (Join-Path $out 'rewards') --binaries (Join-Path $buildRoot $Config) --directory-only
    if ($LASTEXITCODE -ne 0) { throw 'Battle reward owner toolkit packaging failed' }
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $out -File -Recurse) {
        $relative = $file.FullName.Substring($out.Length + 1).Replace('\','/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $relative, [IO.Compression.CompressionLevel]::Optimal)
    }
    foreach ($dir in Get-ChildItem -LiteralPath $out -Directory -Recurse) {
        if (@(Get-ChildItem -LiteralPath $dir.FullName -Force).Count -eq 0) {
            [void]$archive.CreateEntry($dir.FullName.Substring($out.Length + 1).Replace('\','/') + '/')
        }
    }
} finally { $archive.Dispose() }
Write-Host $zip
$hashAlgorithm = [Security.Cryptography.SHA256]::Create()
$zipStream = [IO.File]::OpenRead($zip)
try { $hash = [BitConverter]::ToString($hashAlgorithm.ComputeHash($zipStream)).Replace('-','') }
finally { $zipStream.Dispose(); $hashAlgorithm.Dispose() }
Write-Host "SHA256 $hash"
