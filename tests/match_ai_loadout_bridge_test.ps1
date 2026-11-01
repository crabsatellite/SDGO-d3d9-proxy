param([string]$Config = 'Release')

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo "build\$Config"
$run = Join-Path $repo (
    'build\match_ai_loadout_bridge_runs\' +
    [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))

$sourceExe = Join-Path $bin 'match_ai_loadout_bridge_test.exe'
$sourceFixture = Join-Path $bin 'battle_ai_bridge_fixture.dll'
if (-not (Test-Path -LiteralPath $sourceExe)) {
    throw "Missing test executable: $sourceExe"
}
if (-not (Test-Path -LiteralPath $sourceFixture)) {
    throw "Missing bridge fixture: $sourceFixture"
}

New-Item -ItemType Directory -Force -Path $run | Out-Null
$testExe = Join-Path $run 'match_ai_loadout_bridge_test.exe'
$fixture = Join-Path $run 'battle_ai_bridge_fixture.dll'
Copy-Item -LiteralPath $sourceExe -Destination $testExe
Copy-Item -LiteralPath $sourceFixture -Destination $fixture

& $testExe $fixture 2>&1 |
    Tee-Object -FilePath (Join-Path $run 'result.txt')
if ($LASTEXITCODE -ne 0) {
    throw "Match AI loadout bridge fixture failed: $run"
}
if (Test-Path -LiteralPath (Join-Path $run 'battle_ai.dll')) {
    throw "Fixture left a staged battle_ai.dll behind: $run"
}

Write-Output "MATCH AI LOADOUT BRIDGE FIXTURE PASSED: $run"
