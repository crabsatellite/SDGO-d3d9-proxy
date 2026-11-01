param([string]$BuildDir='build',[string]$Config='Release')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$build=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($BuildDir)){$BuildDir}else{Join-Path $repo $BuildDir}))
$work=Join-Path $build ('shop_manager_ownership\'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$ini=Join-Path $work 'no-database.ini'
[IO.File]::WriteAllText($ini,"[fixture]`nNoCredentials=1`n",[Text.Encoding]::ASCII)
$output=& (Join-Path $build "$Config\shop_catalog_mod_test.exe") (Join-Path $build "$Config\shop_catalog.dll") $ini $work
if($LASTEXITCODE -ne 0){throw "Shop host attempted startup application or failed registration: $output"}
if(Test-Path -LiteralPath (Join-Path $work 'catalog.ini')){throw 'Host startup created a shop profile'}
Write-Host 'PASS Manager ownership: supported host registers without database settings or startup publication'
