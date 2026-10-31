param([string]$Config='Release')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$run=Join-Path $repo ('build\battle_ai_native_runs\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$bin=Join-Path $repo "build\$Config"
foreach($case in 'enabled','disabled','bad-signature','missing-script','generic') {
    $dir=Join-Path $run $case
    $mod=Join-Path $dir 'sdgo\mods\battle_ai'
    New-Item -ItemType Directory -Force -Path $mod,(Join-Path $dir 'sdgo\symbols') | Out-Null
    Copy-Item -LiteralPath (Join-Path $bin 'battle_ai_native_test.exe') -Destination $dir
    Copy-Item -LiteralPath (Join-Path $bin 'core.dll') -Destination (Join-Path $dir 'sdgo')
    Copy-Item -LiteralPath (Join-Path $bin 'battle_ai.dll') -Destination $mod
    $manifest=(Get-Content -LiteralPath (Join-Path $repo 'mods\battle_ai\mod.ini') -Raw).Replace('hosts=GOnline-54BE1568-00C97000','hosts=*')
    [IO.File]::WriteAllText((Join-Path $mod 'mod.ini'),$manifest)
    if($case -ne 'missing-script') { Copy-Item -LiteralPath (Join-Path $repo 'mods\battle_ai\battle_ai.lua') -Destination $mod }
    Copy-Item -LiteralPath (Join-Path $repo 'mods\battle_ai\universal_ai.lua') -Destination $mod
    $policy="[settings]`r`nGenericMode=0`r`n"
    if($case -eq 'generic') { $policy="[settings]`r`nGenericMode=1`r`nReactionMs=0`r`nTargetHoldMs=0`r`nBurstMs=0`r`n" }
    [IO.File]::WriteAllText((Join-Path $mod 'config.ini'),$policy)
    $cfg="[core]`r`nlog_level=1`r`n[mods]`r`n"
    if($case -eq 'disabled') { $cfg+="battle_ai=0`r`n" }
    [IO.File]::WriteAllText((Join-Path $dir 'sdgo\sdgo.ini'),$cfg)
    & (Join-Path $dir 'battle_ai_native_test.exe') $case 2>&1 | Tee-Object -FilePath (Join-Path $dir 'result.txt')
    if($LASTEXITCODE -ne 0) { throw "Native AI fixture $case failed; $dir" }
}
Write-Output "ALL NATIVE ADAPTER CASES PASSED: $run"
