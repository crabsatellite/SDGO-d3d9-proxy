<# Copy game assets into this worktree only. No service, database, tunnel or
   launcher is started; no existing mod installation is updated. #>
param([Parameter(Mandatory=$true)][string]$ClientDir)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$source=(Resolve-Path -LiteralPath $ClientDir).Path
$dest=Join-Path $repo 'build\battle_ai_sandbox\client'
if(Test-Path -LiteralPath $dest) { throw "Sandbox already exists; refusing to overwrite $dest" }
$resolved=[IO.Path]::GetFullPath($dest)
if(-not $resolved.StartsWith($repo+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Sandbox path escaped worktree' }
$files=@('GOnline.exe','GPackData.zdx','GPackData.zpk','config.xml','ClientVer.dat','GameGuard.des',
 'dbghelp.dll','dinput8.dll','dinput8.ini','Mss32.dll','SDPlus.dll','SDPlus.ini',
 'zoa_core.dll','zoa_graphics.dll','zoa_input.dll','zoa_obs.dll','zoa_sound.dll','zoa_ui.dll',
 'intro.mpg','box.xml','sphere.xml','cylinder.xml')
New-Item -ItemType Directory -Path $dest -Force | Out-Null
foreach($file in $files) {
    $path=Join-Path $source $file
    if(Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $dest }
}
foreach($folder in 'BGM','Briefing','miles') {
    $path=Join-Path $source $folder
    if(Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $dest -Recurse }
}
# Keep display/control preferences, but never inherit saved login/autologin data.
$config=Join-Path $dest 'config.xml'
if(Test-Path -LiteralPath $config) {
    $xml=[Xml.XmlDocument]::new()
    $xml.PreserveWhitespace=$true
    $xml.XmlResolver=$null
    $xml.Load($config)
    foreach($node in $xml.SelectNodes('//*[local-name()="LoginData"]')) { $node.RemoveAll() }
    $xml.Save($config)
}
$stage=Join-Path $repo 'build\stage\Release\client'
& (Join-Path $repo 'tools\install_client.ps1') -GameDir $dest -Source $stage
[IO.File]::WriteAllText((Join-Path $dest 'AI-SANDBOX.txt'),@'
Independent AI client asset copy. No backend, database, FRP, or automatic login.
No launcher/account data or saved chat was copied. All automated checks use
private executable fixtures, not this client or a running game service.
For manual gameplay acceptance, supply a separately isolated test backend's
address with your normal game startup method. Existing service addresses are
not preconfigured here. Do not use another running task's backend for this test.
'@)
$manifest=Get-ChildItem -LiteralPath $dest -Recurse -File | ForEach-Object {
    [pscustomobject]@{path=$_.FullName.Substring($dest.Length+1);bytes=$_.Length}
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path (Split-Path $dest) 'client-inventory.json') -Encoding utf8
Write-Output "Independent client prepared (not launched): $dest"
