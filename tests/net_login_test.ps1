$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$binaries=Join-Path $repo 'build\Release'
$work=Join-Path $repo ('build\net_login_test_run\'+[guid]::NewGuid().ToString('N'))
foreach($mode in @('valid','reject','invalid')){
    $dir=Join-Path $work "$mode different layout"
    $mod=Join-Path $dir 'sdgo\mods\net_route'
    New-Item -ItemType Directory -Force -Path $mod | Out-Null
    Copy-Item -LiteralPath (Join-Path $binaries 'net_login_test.exe') -Destination $dir
    $core=Join-Path $binaries 'core.dll'
    Copy-Item -LiteralPath $core -Destination (Join-Path $dir 'sdgo\core.dll')
    Copy-Item -LiteralPath (Join-Path $binaries 'net_route.dll') -Destination $mod
    $manifest=(Get-Content -LiteralPath (Join-Path $repo 'mods\net_route\mod.ini') -Raw).Replace('enabled=0','enabled=1') -replace '(?m)^hosts=.*$','hosts=*'
    [IO.File]::WriteAllText((Join-Path $mod 'mod.ini'),$manifest)
    # Legacy manifest is intentionally invalid; config.ini must be authoritative.
    $output=& (Join-Path $dir 'net_login_test.exe') $mode 'old-port' 'keep space' 'quote"and\slash' 2>&1
    if($LASTEXITCODE -ne 0 -or ($output -join "`n") -notmatch 'ALL NET LOGIN CHECKS PASSED'){throw "Exit $LASTEXITCODE : $($output -join [Environment]::NewLine)"}
    Write-Host "$mode : native login parser, config precedence, scope and route checks passed"
}
Write-Host "ALL REAL NET LOGIN CASES PASSED`nReceipt: $work"
