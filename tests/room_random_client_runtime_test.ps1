param([string]$BuildDir='build',[string]$Config='Release')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$build=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($BuildDir)){$BuildDir}else{Join-Path $repo $BuildDir}))
$work=Join-Path $build ('room_random_client_run\'+[guid]::NewGuid().ToString('N'))
foreach($mode in @('enabled','disabled')) {
    $root=Join-Path $work $mode
    $client=Join-Path $root 'sdgo/mods/room_random_client';$head=Join-Path $root 'sdgo/mods/head_probe'
    New-Item -ItemType Directory -Force -Path $client,$head,(Join-Path $root 'sdgo/symbols')|Out-Null
    Copy-Item -LiteralPath (Join-Path $build "$Config/room_random_client_runtime_test.exe") -Destination $root
    Copy-Item -LiteralPath (Join-Path $build "$Config/core.dll") -Destination (Join-Path $root 'sdgo')
    Copy-Item -LiteralPath (Join-Path $build "$Config/room_random_client.dll") -Destination $client
    Copy-Item -LiteralPath (Join-Path $build "$Config/room_random_head_probe.dll") -Destination $head
    $manifest=[IO.File]::ReadAllText((Join-Path $repo 'mods/room_random_client/mod.ini')) -replace '(?m)^hosts=.*$','hosts=*'
    [IO.File]::WriteAllText((Join-Path $client 'mod.ini'),$manifest)
    [IO.File]::WriteAllText((Join-Path $head 'mod.ini'),"[mod]`nname=Head owner fixture`nversion=1`napi=1`ndll=room_random_head_probe.dll`norder=1`nenabled=1`nhosts=*`nrequires=test.dispatch.entry`n")
    $global="[core]`nlog_level=0`n"
    if($mode -eq 'disabled'){$global+="[mods]`nroom_random_client=0`n"}
    [IO.File]::WriteAllText((Join-Path $root 'sdgo/sdgo.ini'),$global)
    Push-Location $root
    try {$output=& (Join-Path $root 'room_random_client_runtime_test.exe') $mode 2>&1;$code=$LASTEXITCODE}
    finally {Pop-Location}
    if($code -ne 0){throw ($output -join "`n")}
    $output|Write-Host
    $log=[IO.File]::ReadAllText((Join-Path $root 'sdgo/logs/sdgo.log'))
    if($mode -eq 'enabled' -and (-not $log.Contains('human factory type 15 model 11032: matches server') -or -not $log.Contains('2 of 2 mods active'))){throw 'Interior/entry hook coexistence or native model observation failed'}
}
Write-Host 'ALL CLIENT START-STATE RUNTIME CASES PASSED'
