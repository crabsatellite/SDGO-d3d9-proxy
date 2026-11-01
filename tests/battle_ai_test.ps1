param([string]$NativeScriptDir,[string]$GameExe,[string]$ServerExe,[string]$AgentExe)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$run=Join-Path $repo ('build\battle_ai_results\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
New-Item -ItemType Directory -Force -Path $run | Out-Null
Push-Location $repo
try {
    & '.\build\Release\battle_ai_lua_test.exe' 'tests\battle_ai_policy.lua' 2>&1 | Tee-Object -FilePath (Join-Path $run 'policy.txt')
    if($LASTEXITCODE -ne 0) { throw 'Policy regression failed' }
    & '.\build\Release\battle_ai_lua_test.exe' 'tests\battle_ai_universal.lua' 2>&1 | Tee-Object -FilePath (Join-Path $run 'universal.txt')
    if($LASTEXITCODE -ne 0) { throw 'Universal controller failed' }
    foreach($test in 'battle_ai_math_test','battle_ai_capabilities_test','battle_ai_navigation_test','battle_ai_items_test','battle_ai_special_budget_test','battle_ai_loadout_test') {
        & ('.\build\Release\'+$test+'.exe') 2>&1 | Tee-Object -FilePath (Join-Path $run ($test+'.txt'))
        if($LASTEXITCODE -ne 0) { throw "Native component $test failed" }
    }
    if($NativeScriptDir) {
        $dir=(Resolve-Path -LiteralPath $NativeScriptDir).Path.Replace('\','/')
        if($dir.Contains(']]')) { throw 'Unsupported Lua long-string delimiter in input path' }
        $boot=Join-Path $run 'replay.lua'
        [IO.File]::WriteAllText($boot,"SDGO_BattleAI_ReplayDir=[[$dir]]`ndofile('tests/battle_ai_replay.lua')`n")
        & '.\build\Release\battle_ai_lua_test.exe' $boot 2>&1 | Tee-Object -FilePath (Join-Path $run 'original-replay.txt')
        if($LASTEXITCODE -ne 0) { throw 'Original bytecode replay failed' }
    }
    & '.\tests\battle_ai_native_test.ps1' 2>&1 | Tee-Object -FilePath (Join-Path $run 'native-adapter.txt')
    & '.\tests\battle_ai_server_native_test.ps1' 2>&1 | Tee-Object -FilePath (Join-Path $run 'server-native-adapter.txt')
    & '.\tests\match_ai_loadout_bridge_test.ps1' 2>&1 | Tee-Object -FilePath (Join-Path $run 'match-loadout-bridge.txt')
    if($GameExe) {
        & python 'tools\verify_battle_ai_native.py' $GameExe 2>&1 | Tee-Object -FilePath (Join-Path $run 'native-contracts.json')
        if($LASTEXITCODE -ne 0) { throw 'Native binary contract check failed' }
        & python 'tools\verify_symbols.py' $GameExe 2>&1 | Tee-Object -FilePath (Join-Path $run 'symbols.txt')
        if($LASTEXITCODE -ne 0) { throw 'Native symbol check failed' }
    }
    if($ServerExe) {
        foreach($verifier in 'verify_battle_ai_server_items.py','verify_battle_ai_server_rules.py','verify_symbols.py') {
            & python ('tools\'+$verifier) $ServerExe 2>&1 | Tee-Object -FilePath (Join-Path $run ($verifier+'.txt'))
            if($LASTEXITCODE -ne 0) { throw "Native server verification failed: $verifier" }
        }
        if($AgentExe) {
            & python 'tools\verify_battle_ai_server_rules.py' $ServerExe --agent-exe $AgentExe 2>&1 | Tee-Object -FilePath (Join-Path $run 'relay-contracts.json')
            if($LASTEXITCODE -ne 0) { throw 'Relay state forwarding contract failed' }
        }
    }
    Write-Output "AI automated checks passed: $run"
} finally { Pop-Location }
