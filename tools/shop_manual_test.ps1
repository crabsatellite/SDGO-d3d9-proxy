<# Explicit host-side manual test actions. No login, purchase or account writes. #>
param(
    [Parameter(Mandatory=$true)][string]$WorldDir,
    [Parameter(Mandatory=$true)][string]$StateDir,
    [string]$ClientDir,
    [ValidateSet('Status','Apply','TestOffers','ChangePrice','ClearOffers')][string]$Action='Status',
    [int]$Price=600
)
$ErrorActionPreference='Stop'
$world=(Resolve-Path -LiteralPath $WorldDir).Path
$state=(Resolve-Path -LiteralPath $StateDir).Path
$tool=Join-Path $state 'shop_catalog.py'
$active=Join-Path $state 'shop.json'
$samples=Join-Path $state 'test-offers.json'
$mod=Join-Path $world 'sdgo\mods\shop_catalog'
$helper=Join-Path $mod 'shop_catalog_apply.exe'
if (-not (Test-Path -LiteralPath $helper)) { throw 'Install the host catalog module first' }
function Shop-Python([string[]]$arguments) {
    & python -B -X utf8 $tool @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Shop configuration command failed' }
}
if ($Action -eq 'Status') { Shop-Python -Arguments @('list','--file',$active); return }
$data=Get-Content -LiteralPath $active -Raw -Encoding UTF8 | ConvertFrom-Json
if ($Action -ne 'Apply') {
    if (@($data.products | Where-Object { $_.key -notin @('shoptest-gb','shoptest-mb','shoptest-rental') }).Count) {
        throw 'Test actions require a dedicated catalog; existing host offers preserved. Use Apply for your own definitions.'
    }
}
$backup=$active+'.'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ')+'.bak'
Copy-Item -LiteralPath $active -Destination $backup
$candidate=Join-Path $state ('catalog-test-'+$PID+'.ini')
try {
    if ($Action -eq 'TestOffers') {
        $demo=Get-Content -LiteralPath $samples -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($demo.catalog_id -ne $data.catalog_id) { throw 'Test catalog identity mismatch' }
        Copy-Item -LiteralPath $samples -Destination $active -Force
    } elseif ($Action -eq 'ChangePrice') {
        if (-not @($data.products | Where-Object { $_.key -eq 'shoptest-gb' -and -not $_.retired }).Count) { throw 'Enable TestOffers first' }
        $patch=Join-Path $state 'test-price.json'
        @{price=$Price} | ConvertTo-Json | Set-Content -LiteralPath $patch -Encoding UTF8
        Shop-Python -Arguments @('update','--file',$active,'--key','shoptest-gb','--patch',$patch)
    } elseif ($Action -eq 'ClearOffers') {
        foreach ($product in $data.products) { Shop-Python -Arguments @('delete','--file',$active,'--key',$product.key) }
    }
    Shop-Python -Arguments @('compile','--file',$active,'--output',$candidate)
    & $helper --host-ini (Join-Path $world 'WorldServer.ini') --catalog $candidate --output $mod
    if ($LASTEXITCODE -ne 0) { throw 'Native catalog apply failed; previous startup configuration preserved' }
    Copy-Item -LiteralPath $candidate -Destination (Join-Path $mod 'catalog.ini') -Force
    if ($ClientDir) {
        $display=Join-Path (Resolve-Path -LiteralPath $ClientDir).Path 'sdgo\mods\shop_labels\display.ini'
        if (-not (Test-Path -LiteralPath (Split-Path -Parent $display))) { throw 'Client display patch not installed; server offers were applied' }
        if (Test-Path -LiteralPath $display) { Copy-Item -LiteralPath $display -Destination ($display+'.'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ')+'.bak') }
        Copy-Item -LiteralPath (Join-Path $mod 'runtime-display.ini') -Destination $display -Force
    }
    Write-Output 'Applied. Reopen the native shop; new Unicode overrides load on the next client start.'
} catch {
    # The native engine reports transaction failure. Keep the candidate for
    # diagnosis, along with the previous JSON and startup profile.
    Write-Output "Previous JSON: $backup"
    throw
}
