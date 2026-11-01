param([string]$BuildDir='build',[string]$Python='python')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$build=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($BuildDir)){$BuildDir}else{Join-Path $repo $BuildDir}))
$services=Join-Path $build 'manager-services'
New-Item -ItemType Directory -Force -Path $services | Out-Null
$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if(-not (Test-Path -LiteralPath $compiler)){$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'}
$library=Join-Path $services 'SdgoShopServices.dll'
$source=Join-Path $repo 'manager\engines\shop'
& $compiler /nologo /target:library ("/out:"+$library) /r:System.Data.dll /r:System.Web.Extensions.dll `
    (Join-Path $source 'Models.cs') (Join-Path $source 'Repository.cs') (Join-Path $source 'Service.cs') (Join-Path $source 'ManagerApi.cs')
if($LASTEXITCODE -ne 0){throw 'Manager business assembly build failed'}
$oldLibrary=$env:SDGO_MANAGER_SERVICES
$oldCache=$env:PYINSTALLER_CONFIG_DIR
try {
    $env:SDGO_MANAGER_SERVICES=$library
    $env:PYINSTALLER_CONFIG_DIR=Join-Path $build 'manager-cache'
    & $Python -m PyInstaller --noconfirm --distpath (Join-Path $build 'manager') --workpath (Join-Path $build 'manager-freeze') (Join-Path $repo 'manager\SDGOModManager.spec')
    if($LASTEXITCODE -ne 0){throw 'Manager EXE build failed; see the concrete error above'}
} finally {$env:SDGO_MANAGER_SERVICES=$oldLibrary;$env:PYINSTALLER_CONFIG_DIR=$oldCache}
Get-Item -LiteralPath (Join-Path $build 'manager\SDGO Mod Manager.exe') | Select-Object FullName,Length
