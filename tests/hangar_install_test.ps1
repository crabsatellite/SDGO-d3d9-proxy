param([string]$BuildDir='build\hangar',[string]$PackageRoot)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if (-not $PackageRoot) { throw 'Pass an extracted dedicated hangar package' }
$package=(Resolve-Path -LiteralPath $PackageRoot).Path
$build=[IO.Path]::GetFullPath((Join-Path $repo $BuildDir))
$root=Join-Path $build ('hangar-install-fixture-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
function FakeExe([string]$dir,[uint32]$stamp=0x54BE1568) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $data=New-Object byte[] 512
    [BitConverter]::GetBytes([int]0x80).CopyTo($data,0x3C)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($data,0x80)
    [BitConverter]::GetBytes([uint16]0x14C).CopyTo($data,0x84)
    [BitConverter]::GetBytes($stamp).CopyTo($data,0x88)
    [BitConverter]::GetBytes([uint32]0x00C97000).CopyTo($data,0xD0)
    [IO.File]::WriteAllBytes((Join-Path $dir 'GOnline.exe'),$data)
}
function Install([string]$dir,[switch]$Refresh) {
    $installerArgs=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $package 'install_hangar.ps1'),'-GameDir',$dir)
    if ($Refresh) { $installerArgs+='-RefreshRules' }
    $savedPreference=$ErrorActionPreference;$ErrorActionPreference='Continue'
    $output=& powershell @installerArgs 2>&1
    $installerExit=$LASTEXITCODE;$ErrorActionPreference=$savedPreference
    if ($installerExit -ne 0) { throw "Hangar installer failed: $($output | Out-String)" }
}
function Hash([string]$path) {
    $sha=[Security.Cryptography.SHA256]::Create();$stream=[IO.File]::OpenRead($path)
    try { [BitConverter]::ToString($sha.ComputeHash($stream)) } finally { $stream.Close();$sha.Dispose() }
}
$existing=Join-Path $root 'existing client with spaces';FakeExe $existing
New-Item -ItemType Directory -Path (Join-Path $existing 'sdgo\symbols') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $existing 'sdgo\mods\other_agent') -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $existing 'sdgo\core.dll'),'existing-core-sentinel')
[IO.File]::WriteAllText((Join-Path $existing 'd3d9.dll'),'existing-loader-sentinel')
[IO.File]::WriteAllText((Join-Path $existing 'sdgo\sdgo.ini'),"[mods]`nother_agent=1`n")
[IO.File]::WriteAllText((Join-Path $existing 'sdgo\mods\other_agent\mod.ini'),'unrelated-mod-sentinel')
$symbols=Join-Path $existing 'sdgo\symbols\GOnline-54BE1568-00C97000.ini'
$unrelated="[other_agent.future]`nkind=data`nrva=0x42`n; keep this exact block`n"
[IO.File]::WriteAllText($symbols,$unrelated)
$protected=@('sdgo\core.dll','d3d9.dll','sdgo\sdgo.ini','sdgo\mods\other_agent\mod.ini')
$before=@{};foreach ($name in $protected) { $before[$name]=Hash (Join-Path $existing $name) }
Install $existing
foreach ($name in $protected) { if ((Hash (Join-Path $existing $name)) -ne $before[$name]) { throw "Changed unrelated file: $name" } }
if (-not [IO.File]::ReadAllText($symbols).Contains($unrelated)) { throw 'Lost unrelated symbol section' }
$mod=Join-Path $existing 'sdgo\mods\hangar_manager'
$ini=Join-Path $mod 'mod.ini';$rules=Join-Path $mod 'units.tsv'
[IO.File]::WriteAllText($ini,([IO.File]::ReadAllText($ini).Replace('KeepCopies=2','KeepCopies=1')+"`nOwnerSetting=kept`n"))
[IO.File]::AppendAllText($rules,"`n# owner rule sentinel`n")
$oldRules=Hash $rules
Install $existing
if (-not [IO.File]::ReadAllText($ini).Contains('KeepCopies=1') -or -not [IO.File]::ReadAllText($ini).Contains('OwnerSetting=kept')) { throw 'Lost owner settings' }
if ((Hash $rules) -ne $oldRules) { throw 'Overwrote custom unit rules' }
$body=[IO.File]::ReadAllText($symbols)
if ([regex]::Matches($body,'(?m)^\[hangar\.brief\]').Count -ne 1) { throw 'Duplicated symbol section' }
Install $existing -Refresh
if ((Hash $rules) -ne (Hash (Join-Path $package 'client\sdgo\mods\hangar_manager\units.tsv'))) { throw 'Explicit rule refresh failed' }
$fresh=Join-Path $root 'fresh client';FakeExe $fresh
[IO.File]::WriteAllText((Join-Path $fresh 'd3d9.dll'),'foreign-graphics-proxy')
Install $fresh
if ([IO.File]::ReadAllText((Join-Path $fresh 'd3d9_chain.dll')) -ne 'foreign-graphics-proxy') { throw 'Lost existing graphics proxy' }
if (-not (Test-Path -LiteralPath (Join-Path $fresh 'sdgo\core.dll'))) { throw 'Fresh framework missing' }
$unknown=Join-Path $root 'unsupported';FakeExe $unknown 0
$oldPreference=$ErrorActionPreference;$ErrorActionPreference='Continue'
$output=& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $package 'install_hangar.ps1') -GameDir $unknown 2>&1
$ErrorActionPreference=$oldPreference
if ($LASTEXITCODE -eq 0 -or (Test-Path -LiteralPath (Join-Path $unknown 'sdgo'))) { throw 'Unsupported game was modified' }
Write-Host 'HANGAR COMPONENT INSTALLER CHECKS PASSED: existing mods/core/symbols/settings, idempotence, rule refresh, fresh chain, unsupported rejection'
