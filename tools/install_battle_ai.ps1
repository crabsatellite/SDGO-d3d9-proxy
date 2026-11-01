<# Install paired AI components into an existing client or battle-server framework. #>
param([Parameter(Mandatory=$true)][string]$GameDir,[string]$Source,
      [ValidateSet('Client','Server')][string]$Side='Client')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$sideName=$Side.ToLowerInvariant()
$loader=if($Side -eq 'Client') {'d3d9.dll'} else {'winmm.dll'}
if(-not $Source) {
    $Source=Join-Path $repo $sideName
    if(-not (Test-Path -LiteralPath $Source)) { $Source=Join-Path $repo "build\stage\Release\$sideName" }
}
$game=(Resolve-Path -LiteralPath $GameDir).Path
if($Side -eq 'Client') {
    $exeName='GOnline.exe';$symbolName='GOnline-54BE1568-00C97000.ini'
    $expectedHash='e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92'
    $products=[ordered]@{battle_ai=@('battle_ai.dll','battle_ai.lua','universal_ai.lua','mod.ini','manager.json','config.example.ini');match_ai=@('match_ai.dll','mod.ini','manager.json');mission_ai=@('mission_ai.dll','mission_ai.lua','mod.ini','manager.json');ai_catalog=@('ai_catalog.dll','mod.ini','manager.json')}
} else {
    $exeName='BattleServer.exe';$symbolName='BattleServer-548A891D-0046C000.ini'
    $expectedHash='f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4'
    $products=[ordered]@{battle_ai_server=@('battle_ai_server.dll','mod.ini','manager.json');pvp_ai=@('pvp_ai.dll','mod.ini','manager.json')}
}
$exe=Join-Path $game $exeName
if(-not (Test-Path -LiteralPath $exe)) { throw "GameDir must contain $exeName" }
if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ine $expectedHash) { throw "Unsupported $exeName byte identity" }
if(-not (Test-Path -LiteralPath (Join-Path $game 'sdgo\core.dll'))) { throw 'Install the SDGO mod framework first' }
foreach($file in @($loader,'sdgo\core.dll')) {
    if(-not (Test-Path -LiteralPath (Join-Path $Source $file))) { throw "AI package is missing its matching framework file: $file" }
}
$targetLoader=Join-Path $game $loader
if(Test-Path -LiteralPath $targetLoader) {
    $product=(Get-Item -LiteralPath $targetLoader).VersionInfo.ProductName
    $strings=[Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($targetLoader))
    if($product -ne 'SDGO mod framework' -and -not $strings.Contains('sdgo\core.dll')) {
        throw "Existing $loader is not this framework. Use the framework installer to preserve its chain before updating AI."
    }
}
$running=Get-CimInstance Win32_Process -Filter "Name='$exeName'" | Where-Object { $_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq $exe }
if($running) { throw 'Close this exact copy before installing. Other copies are not stopped.' }
$sourceSymbols=Join-Path $Source "sdgo\symbols\$symbolName"
$targetSymbols=Join-Path $game "sdgo\symbols\$symbolName"
$incoming=[IO.File]::ReadAllText($sourceSymbols)
$symbols=if(Test-Path -LiteralPath $targetSymbols) { [IO.File]::ReadAllText($targetSymbols) } else { [regex]::Match($incoming,'(?ms)^\[host\]\r?\n.*?(?=^\[|\z)').Value }
$requires=@()
foreach($id in $products.Keys) {
    $sourceMod=Join-Path $Source "sdgo\mods\$id"
    foreach($file in $products[$id]) { if(-not (Test-Path -LiteralPath (Join-Path $sourceMod $file))) { throw "Missing AI component $id/$file" } }
    $list=[regex]::Match([IO.File]::ReadAllText((Join-Path $sourceMod 'mod.ini')),'(?m)^requires=([^\r\n]+)').Groups[1].Value
    if(-not $list) { throw "Missing dependency manifest for $id" }
    $requires+=$list.Split(',')
}
foreach($name in ($requires | Select-Object -Unique)) {
    $pattern='(?ms)^\['+[regex]::Escape($name)+'\]\r?\n.*?(?=^\[|\z)'
    $section=[regex]::Match($incoming,$pattern)
    if(-not $section.Success) { throw "Missing incoming symbol $name" }
    $replaceText=$section.Value.TrimEnd()+"`r`n`r`n"
    if([regex]::IsMatch($symbols,$pattern)) { $symbols=[regex]::Replace($symbols,$pattern,[Text.RegularExpressions.MatchEvaluator]{param($m) $replaceText}) }
    else { $symbols=$symbols.TrimEnd()+"`r`n`r`n"+$replaceText }
}
function Merge-UserSections([string]$NewManifest,[string]$OldManifest) {
    $oldMod=[regex]::Match($OldManifest,'(?ims)^\[mod\]\r?\n.*?(?=^\[|\z)').Value
    $enabled=[regex]::Match($oldMod,'(?im)^enabled\s*=\s*([01])\s*$')
    if($enabled.Success) {
        $value=$enabled.Groups[1].Value
        $modPattern='(?ims)^\[mod\]\r?\n.*?(?=^\[|\z)'
        $NewManifest=[regex]::Replace($NewManifest,$modPattern,[Text.RegularExpressions.MatchEvaluator]{param($m)
            if([regex]::IsMatch($m.Value,'(?im)^enabled\s*=')) {
                return [regex]::Replace($m.Value,'(?im)^enabled\s*=.*$',('enabled='+$value))
            }
            return [regex]::Replace($m.Value,'(?im)^\[mod\]\r?\n',('[mod]'+"`r`n"+'enabled='+$value+"`r`n"))
        })
    }
    foreach($section in [regex]::Matches($OldManifest,'(?ms)^\[([^\]\r\n]+)\]\r?\n.*?(?=^\[|\z)')) {
        $name=$section.Groups[1].Value
        if($name -ieq 'mod') { continue }
        $pattern='(?ims)^\['+[regex]::Escape($name)+'\]\r?\n.*?(?=^\[|\z)'
        $saved=$section.Value.TrimEnd()+"`r`n`r`n"
        if([regex]::IsMatch($NewManifest,$pattern)) { $NewManifest=[regex]::Replace($NewManifest,$pattern,[Text.RegularExpressions.MatchEvaluator]{param($m) $saved}) }
        else { $NewManifest=$NewManifest.TrimEnd()+"`r`n`r`n"+$saved }
    }
    return $NewManifest
}
$backup=Join-Path $game ('sdgo\backups\battle_ai\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ'))
New-Item -ItemType Directory -Path $backup -Force | Out-Null
foreach($file in @($loader,'sdgo\core.dll')) {
    $target=Join-Path $game $file
    if(Test-Path -LiteralPath $target) { Copy-Item -LiteralPath $target -Destination (Join-Path $backup ([IO.Path]::GetFileName($file))) }
    Copy-Item -LiteralPath (Join-Path $Source $file) -Destination $target -Force
}
if(Test-Path -LiteralPath $targetSymbols) { Copy-Item -LiteralPath $targetSymbols -Destination (Join-Path $backup $symbolName) }
foreach($id in $products.Keys) {
    $sourceMod=Join-Path $Source "sdgo\mods\$id";$targetMod=Join-Path $game "sdgo\mods\$id";$priorDir=Join-Path $backup $id
    New-Item -ItemType Directory -Path $targetMod,$priorDir -Force | Out-Null
    foreach($file in $products[$id]) {
        $target=Join-Path $targetMod $file;$sourceFile=Join-Path $sourceMod $file
        if(Test-Path -LiteralPath $target) { Copy-Item -LiteralPath $target -Destination (Join-Path $priorDir $file) }
        if($file -eq 'mod.ini' -and (Test-Path -LiteralPath $target)) {
            $merged=Merge-UserSections ([IO.File]::ReadAllText($sourceFile)) ([IO.File]::ReadAllText($target))
            [IO.File]::WriteAllText($target,$merged,[Text.UTF8Encoding]::new($false))
        } else { Copy-Item -LiteralPath $sourceFile -Destination $target -Force }
    }
}
New-Item -ItemType Directory -Path (Split-Path -Parent $targetSymbols) -Force | Out-Null
[IO.File]::WriteAllText($targetSymbols,$symbols,[Text.UTF8Encoding]::new($false))
Write-Output "Battle AI $Side components installed: $game"
Write-Output "User settings, config.ini and unrelated mods preserved. Backup: $backup"
