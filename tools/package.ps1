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
    [string]$Config = "Release",
    [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) "build"),
    [string]$OutputName
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $repo "VERSION") -TotalCount 1).Trim()
$stage = Join-Path ([IO.Path]::GetFullPath($BuildDir)) "stage\$Config"
foreach ($tree in "client", "server") {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $tree))) { throw "No $tree tree in $stage; build first" }
}

$name = "SDGO-ModFramework-$version"
if ($OutputName) { $name=$OutputName }
if ($name -notmatch '^[A-Za-z0-9._-]+$') { throw 'Invalid package output name' }
$dist = Join-Path $repo "dist"
$out = Join-Path $dist $name
if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($out)) -ne [IO.Path]::GetFullPath($dist)) { throw 'Package output leaves dist' }
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory -Force -Path $out | Out-Null

Copy-Item -LiteralPath (Join-Path $stage "client") -Destination $out -Recurse
Copy-Item -LiteralPath (Join-Path $stage "server") -Destination $out -Recurse
Copy-Item -LiteralPath (Join-Path $repo "tools\install_client.ps1") -Destination $out
Copy-Item -LiteralPath (Join-Path $repo "tools\install_server.ps1") -Destination $out
New-Item -ItemType Directory -Force -Path (Join-Path $out "tools") | Out-Null
foreach ($helper in "mod_assistant.py", "operator_snapshot.py", "Install-Operator.ps1", "Start-Assistant.ps1", "requirements-operator-assistant.txt") {
    Copy-Item -LiteralPath (Join-Path $repo "tools\$helper") -Destination (Join-Path $out "tools")
}
$moduleSource=Join-Path $repo "tools\assistant_modules"
foreach ($file in Get-ChildItem -LiteralPath $moduleSource -Recurse -File) {
    if ($file.Extension -notin '.py','.html') { continue }
    $relative=$file.FullName.Substring($moduleSource.Length).TrimStart('\')
    $target=Join-Path (Join-Path $out 'tools\assistant_modules') $relative
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
    [IO.File]::Copy($file.FullName,$target,$true)
}
New-Item -ItemType Directory -Force -Path (Join-Path $out "assistant_module\operator_catalog") | Out-Null
foreach ($data in "mod.ini", "symbols.fragment.ini", "module.json", "server") {
    Copy-Item -LiteralPath (Join-Path $repo "mods\operator_catalog\$data") -Destination (Join-Path $out "assistant_module\operator_catalog") -Recurse
}
Copy-Item -LiteralPath (Join-Path $repo "README.md") -Destination $out
Copy-Item -LiteralPath (Join-Path $repo "README.zh-CN.md") -Destination $out
Copy-Item -LiteralPath (Join-Path $repo "docs") -Destination $out -Recurse
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
