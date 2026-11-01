param([string]$OutDirectory,[switch]$Tests)
$ErrorActionPreference='Stop'
$shopSource=Split-Path -Parent $PSScriptRoot
if (-not $OutDirectory) { $OutDirectory=Join-Path $shopSource 'build\shop-ui' }
New-Item -ItemType Directory -Path $OutDirectory -Force | Out-Null
$shopCsc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $shopCsc)) { $shopCsc=Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe' }
$shopRefs=@('System.dll','System.Core.dll','System.Data.dll','System.Web.Extensions.dll','System.Windows.Forms.dll','System.Drawing.dll') | ForEach-Object { '/reference:'+$_ }
$shopOptions=@('/nologo','/utf8output','/codepage:65001','/optimize+','/warn:4','/warnaserror+')+$shopRefs
$shopFiles=@('Models.cs','Repository.cs','Service.cs','Gui.cs','MainForm.cs','Program.cs') | ForEach-Object { Join-Path $shopSource "owner\shop\$_" }
$shopOldLib=$env:LIB
if ($env:LIB) { $env:LIB=(($env:LIB -split ';') | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Container) }) -join ';' }
try {
    & $shopCsc @shopOptions /target:winexe ('/win32manifest:'+(Join-Path $shopSource 'owner\shop\app.manifest')) ('/out:'+(Join-Path $OutDirectory 'SdgoShopManager.exe')) @shopFiles
    if ($LASTEXITCODE -ne 0) { throw 'Shop UI build failed' }
    if ($Tests) {
        & $shopCsc @shopOptions /target:exe ('/reference:'+(Join-Path $OutDirectory 'SdgoShopManager.exe')) ('/out:'+(Join-Path $OutDirectory 'ShopUiTests.exe')) (Join-Path $shopSource 'tests\ShopUiTests.cs')
        if ($LASTEXITCODE -ne 0) { throw 'Shop UI tests build failed' }
    }
} finally { $env:LIB=$shopOldLib }
Write-Output $OutDirectory
