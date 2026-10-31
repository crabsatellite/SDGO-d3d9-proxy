param([string]$NativeScriptDir)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$run=Join-Path $repo ('build\battle_ai_results\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
New-Item -ItemType Directory -Force -Path $run | Out-Null
Push-Location $repo
try {
    & '.\build\Release\battle_ai_lua_test.exe' 'tests\battle_ai_policy.lua' 2>&1 | Tee-Object -FilePath (Join-Path $run 'policy.txt')
    if($LASTEXITCODE -ne 0) { throw 'Policy regression failed' }
    if($NativeScriptDir) {
        $dir=(Resolve-Path -LiteralPath $NativeScriptDir).Path.Replace('\','/')
        if($dir.Contains(']]')) { throw 'Unsupported Lua long-string delimiter in input path' }
        $boot=Join-Path $run 'replay.lua'
        [IO.File]::WriteAllText($boot,"SDGO_BattleAI_ReplayDir=[[$dir]]`ndofile('tests/battle_ai_replay.lua')`n")
        & '.\build\Release\battle_ai_lua_test.exe' $boot 2>&1 | Tee-Object -FilePath (Join-Path $run 'original-replay.txt')
        if($LASTEXITCODE -ne 0) { throw 'Original bytecode replay failed' }
    }
    & '.\tests\battle_ai_native_test.ps1' 2>&1 | Tee-Object -FilePath (Join-Path $run 'native-adapter.txt')
    Write-Output "AI automated checks passed: $run"
} finally { Pop-Location }
