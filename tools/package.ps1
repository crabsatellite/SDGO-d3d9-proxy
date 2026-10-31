<#
.SYNOPSIS
  Builds the release zip from a built tree.

.DESCRIPTION
  Collects build\stage\<Config>\client and \server, the two install scripts, the docs and
  the licenses into dist\SDGO-ModFramework-<version>\ and zips it to
  dist\SDGO-ModFramework-<version>.zip. The version comes from the VERSION file.
  Build first: cmake --build build --config Release

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\package.ps1
#>
param(
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repo "VERSION") -TotalCount 1).Trim()
$stage = Join-Path $repo "build\stage\$Config"
foreach ($tree in "client", "server") {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $tree))) { throw "No $tree tree in $stage; build first" }
}

$name = "SDGO-ModFramework-$version"
$dist = [IO.Path]::GetFullPath((Join-Path $repo "dist"))
$out = [IO.Path]::GetFullPath((Join-Path $dist $name))
if ([IO.Path]::GetDirectoryName($out) -ne $dist) { throw "Package output must stay directly inside $dist" }
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Force -Path $out | Out-Null

Copy-Item -LiteralPath (Join-Path $stage "client") -Destination $out -Recurse
Copy-Item -LiteralPath (Join-Path $stage "server") -Destination $out -Recurse
Copy-Item -LiteralPath (Join-Path $repo "tools\install_client.ps1") -Destination $out
Copy-Item -LiteralPath (Join-Path $repo "tools\install_server.ps1") -Destination $out
Copy-Item -LiteralPath (Join-Path $repo "README.md") -Destination $out
Copy-Item -LiteralPath (Join-Path $repo "README.zh-CN.md") -Destination $out
Copy-Item -LiteralPath (Join-Path $repo "docs") -Destination $out -Recurse
# Owner-side preparation only. Whitelist sources, never Python caches or generated rosters.
New-Item -ItemType Directory -Force -Path (Join-Path $out "backend") | Out-Null
foreach ($file in "ai_catalog.py", "ai_catalog.rules.example.json", "README.zh-CN.md") {
    Copy-Item -LiteralPath (Join-Path $repo "backend\$file") -Destination (Join-Path $out "backend")
}
Copy-Item -LiteralPath (Join-Path $repo "THIRD_PARTY_NOTICES.md") -Destination $out
if (Test-Path -LiteralPath (Join-Path $repo "LICENSE")) { Copy-Item -LiteralPath (Join-Path $repo "LICENSE") -Destination $out }
New-Item -ItemType Directory -Force -Path (Join-Path $out "licenses") | Out-Null
Copy-Item -LiteralPath (Join-Path $repo "third_party\minhook\LICENSE.txt") -Destination (Join-Path $out "licenses\MinHook-LICENSE.txt")

$zip = Join-Path $dist "$name.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
# ZipFile writes '/' separators; Compress-Archive in Windows PowerShell 5.1 writes '\'.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($out, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
Write-Host "$zip"
Write-Host "SHA256 $hash"
