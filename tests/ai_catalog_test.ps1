param([string]$BuildDir='build/catalog', [string]$Config='Release')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$build=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($BuildDir)){$BuildDir}else{Join-Path $repo $BuildDir}))
$run=Join-Path $build ('catalog_test_run/'+[guid]::NewGuid().ToString('N'))
foreach($role in 'client','server') {
    foreach($case in 'off','on','conflict','changed','deferred','corrupt','missing','rollback','off','on') {
        $dir=Join-Path $run ($role+'-'+$case+'-'+[guid]::NewGuid().ToString('N'))
        $mod=Join-Path $dir 'sdgo/mods/ai_catalog'
        New-Item -ItemType Directory -Force -Path $mod | Out-Null
        Copy-Item -LiteralPath (Join-Path $build "$Config/ai_catalog_test.exe") -Destination $dir
        Copy-Item -LiteralPath (Join-Path $build "$Config/core.dll") -Destination (Join-Path $dir 'sdgo/core.dll')
        Copy-Item -LiteralPath (Join-Path $build "catalog_$role/$Config/ai_catalog.dll") -Destination $mod
        if($case -eq 'rollback') {
            $other=Join-Path $dir 'sdgo/mods/other_feature'
            New-Item -ItemType Directory -Force -Path $other | Out-Null
            Copy-Item -LiteralPath (Join-Path $build "$Config/catalog_conflict_mod.dll") -Destination $other
            $server=if($role -eq 'server'){1}else{0}
            "[mod]`nid=other_feature`napi=1`nhosts=*`norder=10`ndll=catalog_conflict_mod.dll`n[settings]`nServer=$server`n" |
                Set-Content -LiteralPath (Join-Path $other 'mod.ini') -Encoding ASCII
        }
        $output=& (Join-Path $dir 'ai_catalog_test.exe') $role $case 2>&1
        $output | Set-Content -LiteralPath (Join-Path $dir 'result.txt') -Encoding UTF8
        if($LASTEXITCODE -ne 0 -or ($output -join "`n") -notmatch 'ALL CATALOG CHECKS PASSED') {
            throw "$role/$case failed ($LASTEXITCODE): $($output -join [Environment]::NewLine). Receipt: $dir"
        }
        Write-Host "$role/$case : $($output -join ' ')"
    }
}
Write-Host "ALL REAL CATALOG CASES PASSED. Receipt: $run"
