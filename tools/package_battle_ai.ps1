param([string]$Config='Release')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$tag=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$name='SDGO-BattleAI-0.3.0-preview-'+$tag
$out=Join-Path $repo ('dist\'+$name)
if(Test-Path -LiteralPath $out) { throw 'Package destination already exists' }
$products=@{
 client=[ordered]@{battle_ai=@('battle_ai.dll','battle_ai.lua','universal_ai.lua','mod.ini','config.example.ini');match_ai=@('match_ai.dll','mod.ini')};
 server=[ordered]@{battle_ai_server=@('battle_ai_server.dll','mod.ini');pvp_ai=@('pvp_ai.dll','mod.ini')}
}
foreach($side in 'client','server') {
    $stage=Join-Path $repo "build\stage\$Config\$side"
    $requires=@()
    foreach($id in $products[$side].Keys) {
        $mod=Join-Path $out "$side\sdgo\mods\$id"
        New-Item -ItemType Directory -Force -Path $mod | Out-Null
        foreach($file in $products[$side][$id]) {
            Copy-Item -LiteralPath (Join-Path $stage "sdgo\mods\$id\$file") -Destination $mod
        }
        $requires+=([regex]::Match([IO.File]::ReadAllText((Join-Path $mod 'mod.ini')),'(?m)^requires=([^\r\n]+)').Groups[1].Value.Split(','))
    }
    $symbolName=if($side -eq 'client') {'GOnline-54BE1568-00C97000.ini'} else {'BattleServer-548A891D-0046C000.ini'}
    $text=[IO.File]::ReadAllText((Join-Path $stage "sdgo\symbols\$symbolName"))
    $fragment="; Additive Battle AI dependencies. Use tools/install_battle_ai.ps1.`r`n"
    foreach($section in (@('host')+($requires | Select-Object -Unique))) {
        $part=[regex]::Match($text,'(?ms)^\['+[regex]::Escape($section)+'\]\r?\n.*?(?=^\[|\z)')
        if(-not $part.Success) { throw "Missing symbol $side/$section" }
        $fragment+=$part.Value.TrimEnd()+"`r`n`r`n"
    }
    $sym=Join-Path $out "$side\sdgo\symbols"
    New-Item -ItemType Directory -Force -Path $sym | Out-Null
    [IO.File]::WriteAllText((Join-Path $sym $symbolName),$fragment,[Text.UTF8Encoding]::new($false))
}
New-Item -ItemType Directory -Force -Path (Join-Path $out 'tools'),(Join-Path $out 'docs') | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'tools\install_battle_ai.ps1') -Destination (Join-Path $out 'tools')
Copy-Item -LiteralPath (Join-Path $repo 'docs\battle-ai.md') -Destination (Join-Path $out 'docs')
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $out
$sourceCommit=(& git -C $repo rev-parse HEAD).Trim()
$files=[ordered]@{}
Get-ChildItem -LiteralPath $out -Recurse -File | ForEach-Object {
    $relative=$_.FullName.Substring($out.Length+1).Replace('\','/')
    $files[$relative]=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest=[ordered]@{schema=1;version='0.3.0-preview';source_commit=$sourceCommit;default_difficulty='Medium';components=@('battle_ai','match_ai','battle_ai_server','pvp_ai');files=$files}
[IO.File]::WriteAllText((Join-Path $out 'package-manifest.json'),($manifest | ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=$out+'.zip'
[IO.Compression.ZipFile]::CreateFromDirectory($out,$zip)
Write-Output $zip
Write-Output ('SHA256 '+(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash)
