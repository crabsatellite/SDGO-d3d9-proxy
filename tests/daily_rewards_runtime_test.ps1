param([string]$Config='Release')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$binaries=Join-Path $repo "build\$Config"
$work=Join-Path $repo ('build\daily_rewards_runtime_run\'+[guid]::NewGuid().ToString('N'))
foreach($mode in @('active','clamp','inactive','invalid','disabled','wrongkind','badbytes','badlayout')){
    $dir=Join-Path $work $mode
    $mod=Join-Path $dir 'sdgo\mods\daily_rewards'
    New-Item -ItemType Directory -Force -Path $mod | Out-Null
    Copy-Item -LiteralPath (Join-Path $binaries 'daily_rewards_runtime_test.exe') -Destination $dir
    Copy-Item -LiteralPath (Join-Path $binaries 'core.dll') -Destination (Join-Path $dir 'sdgo')
    Copy-Item -LiteralPath (Join-Path $binaries 'daily_rewards.dll') -Destination $mod
    $manifest=(Get-Content -LiteralPath (Join-Path $repo 'mods\daily_rewards\mod.ini') -Raw) -replace '(?m)^hosts=.*$','hosts=*'
    [IO.File]::WriteAllText((Join-Path $mod 'mod.ini'),$manifest)
    $global="[core]`r`nlog_level=0`r`n"
    if($mode -eq 'disabled'){$global+="[mods]`r`ndaily_rewards=0`r`n"}
    [IO.File]::WriteAllText((Join-Path $dir 'sdgo\sdgo.ini'),$global)
    $output=& (Join-Path $dir 'daily_rewards_runtime_test.exe') $mode 2>&1
    if($LASTEXITCODE -ne 0 -or ($output -join "`n") -notmatch 'ALL DAILY REWARD RUNTIME CHECKS PASSED'){throw ($output -join "`n")}
    $log=[IO.File]::ReadAllText((Join-Path $dir 'sdgo\logs\sdgo.log'))
    if($mode -in @('active','clamp','inactive')){
        if(-not $log.Contains('loaded Daily double battle rewards (Beta)')){throw "Mod was not active in $mode"}
    }elseif($log.Contains('loaded Daily double battle rewards (Beta)')){throw "Invalid or disabled Mod unexpectedly loaded in $mode"}
    Write-Host "$mode : payout, decomposition, guards, general/FP/SIMD state and error checks passed for all ten paths"
}
Write-Host "ALL DAILY REWARD RUNTIME CASES PASSED`nReceipt: $work"
