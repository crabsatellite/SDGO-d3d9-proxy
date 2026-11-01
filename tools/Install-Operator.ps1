param(
    [Parameter(Mandatory=$true)][string]$ClientRoot,
    [string]$PackageRoot=$PSScriptRoot
)
$ErrorActionPreference='Stop'
$client=[IO.Path]::GetFullPath($ClientRoot)
$package=[IO.Path]::GetFullPath($PackageRoot)
$exe=Join-Path $client 'GOnline.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw 'GOnline.exe was not found in the selected client.' }
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92') {
    throw 'This operator module does not support the selected game build.'
}
if (-not (Test-Path -LiteralPath (Join-Path $client 'sdgo\core.dll'))) { throw 'Install the SDGO mod framework first.' }
$running=Get-CimInstance Win32_Process -Filter "Name='GOnline.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -and [IO.Path]::GetFullPath($_.ExecutablePath) -eq $exe }
if ($running) { throw 'Close this client before importing its operator update.' }
$manifest=Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.module -ne 'operator_catalog' -or $manifest.schema_version -ne 1) { throw 'Unsupported operator package.' }
foreach ($property in $manifest.files.PSObject.Properties) {
    $source=[IO.Path]::GetFullPath((Join-Path $package $property.Name))
    if (-not $source.StartsWith($package+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Package path leaves its directory.' }
    if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or
        (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $property.Value) { throw 'Package integrity check failed.' }
}
$symbolFile=Join-Path $client 'sdgo\symbols\GOnline-54BE1568-00C97000.ini'
if (-not (Test-Path -LiteralPath $symbolFile)) { throw 'The framework symbol file is missing.' }
$fragment=Get-Content -LiteralPath (Join-Path $package 'symbols.fragment.ini') -Raw -Encoding UTF8
$sections=[ordered]@{}
$current=''
foreach ($line in ($fragment -split '\r?\n')) {
    if ($line -match '^\[(.+)\]$') { $current=$Matches[1]; if ($current -like 'operator.*') { $sections[$current]=@($line) } }
    elseif ($current -like 'operator.*') { $sections[$current]+=$line }
}
if ($sections.Count -ne 8) { throw 'The operator symbol fragment is incomplete.' }
$old=[IO.File]::ReadAllText($symbolFile)
$lines=New-Object 'System.Collections.Generic.List[string]'
$skip=$false
foreach ($line in ($old -split '\r?\n')) {
    if ($line -match '^\[(.+)\]$') { $skip=$sections.Contains($Matches[1]) }
    if (-not $skip) { $lines.Add($line) }
}
foreach ($section in $sections.Values) { foreach ($line in $section) { $lines.Add($line) } }
$newSymbols=($lines -join "`r`n")+"`r`n"
$receiptRoot=Join-Path $client ('sdgo\operator-update-backups\'+$manifest.revision)
[IO.Directory]::CreateDirectory($receiptRoot) | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $receiptRoot 'symbols.ini'))) {
    [IO.File]::WriteAllText((Join-Path $receiptRoot 'symbols.ini'),$old,(New-Object Text.UTF8Encoding $false))
}
$prefix='client/sdgo/mods/operator_catalog/'
$owned=@($manifest.files.PSObject.Properties | Where-Object { $_.Name.StartsWith($prefix,[StringComparison]::Ordinal) })
foreach ($property in $owned) {
    $relative=$property.Name.Substring(7).Replace('/','\')
    $target=[IO.Path]::GetFullPath((Join-Path $client $relative))
    $moduleRoot=[IO.Path]::GetFullPath((Join-Path $client 'sdgo\mods\operator_catalog'))
    if (-not $target.StartsWith($moduleRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Update leaves its own module directory.' }
    $backup=Join-Path $receiptRoot $relative
    if ((Test-Path -LiteralPath $target) -and -not (Test-Path -LiteralPath $backup)) {
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($backup)) | Out-Null
        [IO.File]::Copy($target,$backup,$false)
    }
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
    [IO.File]::Copy((Join-Path $package $property.Name),$target,$true)
}
[IO.File]::WriteAllText($symbolFile,$newSymbols,(New-Object Text.UTF8Encoding $false))
[IO.File]::WriteAllText((Join-Path $receiptRoot 'receipt.json'),($manifest | ConvertTo-Json -Depth 8),(New-Object Text.UTF8Encoding $false))
Write-Output ('Operator configuration imported: '+$manifest.revision)
Write-Output 'Use your normal game launcher. Other modules and player nicknames were preserved.'
