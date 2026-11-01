<# Additive shop patch installation. No game/server process is stopped or started. #>
param(
    [Parameter(Mandatory=$true)][string]$Target,
    [ValidateSet('Labels','Catalog')][string]$Role,
    [string]$PackageRoot,
    [Alias('Profile')][string]$ConfigFile,
    [string]$Executable,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
if (-not $PackageRoot) { $PackageRoot = Split-Path -Parent $MyInvocation.MyCommand.Path }
$shopPackage = (Resolve-Path -LiteralPath $PackageRoot).Path
$shopRoot = (Resolve-Path -LiteralPath $Target).Path
$shopManifestPath = Join-Path $shopPackage 'package.json'
if (-not (Test-Path -LiteralPath $shopManifestPath)) { throw 'Package has no package.json' }
$shopManifest = Get-Content -LiteralPath $shopManifestPath -Raw | ConvertFrom-Json
if (-not $Role) { $Role = $shopManifest.role }
if ($Role -notin @('Labels','Catalog') -or $Role -ne $shopManifest.role) { throw 'Package role mismatch' }
$shopId = $(if ($Role -eq 'Labels') { 'shop_labels' } else { 'shop_catalog' })
if (-not $Executable) { $Executable = $(if ($Role -eq 'Labels') { 'GOnline.exe' } else { 'WorldServer.exe' }) }
if ($Role -eq 'Labels' -and $Executable -ne 'GOnline.exe') { throw 'The client patch supports the native GOnline.exe filename' }
if ([IO.Path]::GetFileName($Executable) -ne $Executable -or $Executable -notlike '*.exe') { throw 'Executable must be a filename in the target folder' }
$shopExe = Join-Path $shopRoot $Executable
if (-not (Test-Path -LiteralPath $shopExe)) { throw "Missing target executable: $Executable" }

function Shop-SafeChild([string]$root, [string]$relative) {
    $full = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $full.StartsWith($root.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Path escapes the selected shop directory' }
    $full
}
function Shop-Hash([string]$file) {
    $stream=[IO.File]::OpenRead($file)
    $hash=[Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','') }
    finally { $stream.Dispose(); $hash.Dispose() }
}
function Shop-Ini([string]$file) {
    $data = @{}; $section = ''
    if (-not (Test-Path -LiteralPath $file)) { return $data }
    foreach ($line in [IO.File]::ReadAllLines($file)) {
        $text = $line.Trim()
        if ($text -match '^\[([^\]]+)\]$') {
            $section = $Matches[1]
            if ($data.ContainsKey($section)) { throw "Duplicate INI section: $section" }
            $data[$section] = @{}
        } elseif ($section -and $text -and -not $text.StartsWith(';') -and $text.Contains('=')) {
            $at = $text.IndexOf('='); $key = $text.Substring(0,$at).Trim()
            if ($data[$section].ContainsKey($key)) { throw "Duplicate INI key: $section/$key" }
            $data[$section][$key] = $text.Substring($at+1).Trim()
        }
    }
    return $data
}
function Shop-WriteIni([string]$file, $data) {
    $lines = New-Object 'System.Collections.Generic.List[string]'
    foreach ($section in $data.Keys | Sort-Object) {
        $lines.Add("[$section]")
        foreach ($key in $data[$section].Keys | Sort-Object) { $lines.Add("$key=$($data[$section][$key])") }
        $lines.Add('')
    }
    [IO.File]::WriteAllLines($file,$lines,[Text.Encoding]::Unicode)
}
function Shop-SymbolEncoding([byte[]]$bytes) {
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 255 -and $bytes[1] -eq 254) { return [Text.Encoding]::Unicode }
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 254 -and $bytes[1] -eq 255) { return [Text.Encoding]::BigEndianUnicode }
    [Text.Encoding]::ASCII
}
function Shop-FindBytes([byte[]]$bytes,[byte[]]$pattern,[int]$start) {
    for ($i=$start;$i -le $bytes.Length-$pattern.Length;$i++) {
        $same=$true
        for ($j=0;$j -lt $pattern.Length;$j++) { if ($bytes[$i+$j] -ne $pattern[$j]) { $same=$false;break } }
        if ($same) { return $i }
    }
    -1
}
function Shop-AppendSymbol([string]$file,[string]$section,$entry) {
    $bytes=[IO.File]::ReadAllBytes($file);$encoding=Shop-SymbolEncoding $bytes
    $text="`r`n; sdgo-shop-begin:$section`r`n[$section]`r`n"
    foreach ($key in $entry.Keys | Sort-Object) { $text+="$key=$($entry[$key])`r`n" }
    $text+="; sdgo-shop-end:$section`r`n"
    $extra=$encoding.GetBytes($text);$combined=New-Object byte[] ($bytes.Length+$extra.Length)
    [Array]::Copy($bytes,0,$combined,0,$bytes.Length);[Array]::Copy($extra,0,$combined,$bytes.Length,$extra.Length)
    [IO.File]::WriteAllBytes($file,$combined)
}
function Shop-RemoveSymbol([string]$file,[string]$section) {
    $bytes=[IO.File]::ReadAllBytes($file);$encoding=Shop-SymbolEncoding $bytes
    $begin=$encoding.GetBytes("`r`n; sdgo-shop-begin:$section`r`n")
    $end=$encoding.GetBytes("; sdgo-shop-end:$section`r`n")
    $first=Shop-FindBytes $bytes $begin 0
    if ($first -lt 0) { return }
    $last=Shop-FindBytes $bytes $end ($first+$begin.Length)
    if ($last -lt 0) { throw 'Incomplete shop symbol ownership marker; symbol file preserved' }
    $stop=$last+$end.Length;$out=New-Object byte[] ($bytes.Length-($stop-$first))
    [Array]::Copy($bytes,0,$out,0,$first);[Array]::Copy($bytes,$stop,$out,$first,$bytes.Length-$stop)
    [IO.File]::WriteAllBytes($file,$out)
}
function Shop-PE([string]$file) {
    $bytes = [IO.File]::ReadAllBytes($file)
    if ($bytes.Length -lt 256 -or [BitConverter]::ToUInt16($bytes,0) -ne 0x5A4D) { throw 'Not a PE executable' }
    $pe = [BitConverter]::ToInt32($bytes,0x3C)
    if ($pe -lt 0 -or $pe+0x60 -gt $bytes.Length -or [BitConverter]::ToUInt32($bytes,$pe) -ne 0x4550) { throw 'Invalid PE header' }
    @{ Bytes=$bytes; PE=$pe; Stamp=[BitConverter]::ToUInt32($bytes,$pe+8); ImageSize=[BitConverter]::ToUInt32($bytes,$pe+0x50) }
}
function Shop-RvaOffset($pe, [uint32]$rva) {
    $sections = [BitConverter]::ToUInt16($pe.Bytes,$pe.PE+6)
    $table = $pe.PE+24+[BitConverter]::ToUInt16($pe.Bytes,$pe.PE+20)
    for ($i=0;$i -lt $sections;$i++) {
        $at=$table+$i*40
        $va=[BitConverter]::ToUInt32($pe.Bytes,$at+12)
        $size=[BitConverter]::ToUInt32($pe.Bytes,$at+16)
        if ($rva -ge $va -and $rva-$va -lt $size) { return [int]([BitConverter]::ToUInt32($pe.Bytes,$at+20)+$rva-$va) }
    }
    throw 'Symbol RVA has no file bytes'
}
function Shop-OurLoader([string]$file) {
    if (-not (Test-Path -LiteralPath $file)) { return $false }
    if ((Get-Item -LiteralPath $file).VersionInfo.ProductName -eq 'SDGO mod framework') { return $true }
    [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($file)).Contains('sdgo\core.dll')
}

$shopPE = Shop-PE $shopExe
$shopExpectedStamp = $(if ($Role -eq 'Labels') { [uint32]0x54BE1568 } else { [uint32]0x548A86E3 })
$shopExpectedSize = $(if ($Role -eq 'Labels') { [uint32]0x00C97000 } else { [uint32]0x0010C000 })
if ($shopPE.Stamp -ne $shopExpectedStamp -or $shopPE.ImageSize -ne $shopExpectedSize) { throw 'This native build is not supported by the shop package' }
$shopBusy = Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath -eq $shopExe }
if ($shopBusy) { throw "Close only this target executable before installing: $shopExe" }

$shopMod = Shop-SafeChild $shopRoot "sdgo\mods\$shopId"
$shopSymbols = Shop-SafeChild $shopRoot ('sdgo\symbols\{0}-{1:X8}-{2:X8}.ini' -f [IO.Path]::GetFileNameWithoutExtension($Executable),$shopPE.Stamp,$shopPE.ImageSize)
$shopReceiptPath = Join-Path $shopMod 'shop-install.json'

if ($Uninstall) {
    if (-not (Test-Path -LiteralPath $shopMod)) { Write-Output 'Shop mod already absent'; return }
    $receipt = $(if (Test-Path -LiteralPath $shopReceiptPath) { Get-Content -LiteralPath $shopReceiptPath -Raw | ConvertFrom-Json } else { $null })
    if ($Role -eq 'Catalog' -and (Test-Path -LiteralPath (Join-Path $shopMod 'catalog.ini'))) {
        $hostIni = [IO.Path]::ChangeExtension($shopExe,'.ini')
        $modData = Shop-Ini (Join-Path $shopMod 'mod.ini')
        if ($modData.ContainsKey('settings') -and $modData.settings.ContainsKey('HostIni') -and $modData.settings.HostIni) { $hostIni = Shop-SafeChild $shopRoot $modData.settings.HostIni }
        & (Join-Path $shopMod 'shop_catalog_apply.exe') --host-ini $hostIni --catalog (Join-Path $shopMod 'catalog.ini') --output $shopMod --disable
        if ($LASTEXITCODE -ne 0) { throw 'Could not retire managed database offers; mod kept for recovery' }
    }
    if ($Role -eq 'Labels' -and $receipt -and (Test-Path -LiteralPath $shopSymbols)) {
        $data = Shop-Ini $shopSymbols
        foreach ($property in $receipt.addedSymbols.PSObject.Properties) {
            $section=$property.Name
            if (-not $data.ContainsKey($section)) { continue }
            $same=$true
            foreach ($value in $property.Value.PSObject.Properties) {
                if ($data[$section][$value.Name] -ne $value.Value) { $same=$false }
            }
            if ($same -and $data[$section].Count -eq @($property.Value.PSObject.Properties).Count) { Shop-RemoveSymbol $shopSymbols $section }
        }
    }
    $backupRoot=Shop-SafeChild $shopRoot 'sdgo\shop-backups'
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    $destination=Shop-SafeChild $backupRoot ($shopId+'-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ'))
    if (-not $shopMod.StartsWith($shopRoot+'\',[StringComparison]::OrdinalIgnoreCase) -or -not $destination.StartsWith($backupRoot+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unsafe mod backup move' }
    Move-Item -LiteralPath $shopMod -Destination $destination
    Write-Output "$shopId uninstalled; configuration retained in $destination"
    return
}

# Validate the entire package before changing an installation.
foreach ($property in $shopManifest.files.PSObject.Properties) {
    $file=Shop-SafeChild $shopPackage ($property.Name.Replace('/','\'))
    if (-not (Test-Path -LiteralPath $file) -or (Shop-Hash $file) -ne $property.Value) { throw "Invalid package file: $($property.Name)" }
}
$shopFragment=$null; $shopCurrent=Shop-Ini $shopSymbols; $shopAdded=@{}
if ($Role -eq 'Labels') {
    $shopFragment=Shop-Ini (Join-Path $shopPackage 'symbols.ini')
    if (Test-Path -LiteralPath $shopSymbols) {
        if (-not $shopCurrent.ContainsKey('host')) { throw 'Existing symbol file has no host identity; installation preserved' }
        foreach ($key in @('timestamp','size_of_image')) {
            $value=[string]$shopCurrent.host[$key]
            $expected=[Convert]::ToUInt32($shopFragment.host[$key].Replace('0x',''),16)
            $actual=$(if ($value -match '^0x') { [Convert]::ToUInt32($value.Substring(2),16) } else { [Convert]::ToUInt32($value,10) })
            if ($actual -ne $expected) { throw 'Existing symbol file describes another host; installation preserved' }
        }
    }
    foreach ($section in $shopFragment.Keys) {
        if ($section -eq 'host') { continue }
        $entry=$shopFragment[$section]
        $rva=[Convert]::ToUInt32($entry.rva.Replace('0x',''),16)
        $offset=Shop-RvaOffset $shopPE $rva
        $expect=@($entry.expect -split '\s+' | ForEach-Object { [Convert]::ToByte($_,16) })
        if ($offset+$expect.Count -gt $shopPE.Bytes.Length) { throw 'Truncated native symbol' }
        for ($i=0;$i -lt $expect.Count;$i++) { if ($shopPE.Bytes[$offset+$i] -ne $expect[$i]) { throw "Native signature mismatch: $section" } }
        if ($shopCurrent.ContainsKey($section)) {
            foreach ($key in $entry.Keys) { if ($shopCurrent[$section][$key] -ne $entry[$key]) { throw "Existing symbol conflicts: $section" } }
        } else { $shopAdded[$section]=$entry }
    }
}
$shopConfigText=$null
if ($ConfigFile) {
    $configPath=(Resolve-Path -LiteralPath $ConfigFile).Path
    $configData=Shop-Ini $configPath
    foreach ($section in $configData.Keys) {
        if ($section -ne 'shop' -and $(if ($Role -eq 'Labels') { $section -notmatch '^option:[0-9]+$' } else { $section -notmatch '^product:[A-Za-z0-9_.-]+$' })) { throw 'Configuration contains an unexpected section; do not pass a server credential INI' }
    }
    if ($Role -eq 'Catalog' -and (-not $configData.ContainsKey('shop') -or $configData.shop.Schema -ne '1' -or -not $configData.shop.CatalogId)) { throw 'Invalid catalog profile header' }
    $shopConfigText=[IO.File]::ReadAllText($configPath)
}

# Bootstrap only missing framework components. Existing shared core/configuration
# and existing mods are left in place; unrelated loader wrappers are chained.
$shopLoader=$(if ($Role -eq 'Labels') { 'd3d9.dll' } else { 'winmm.dll' })
$shopSide=$(if ($Role -eq 'Labels') { 'client' } else { 'server' })
$shopFramework=Join-Path $shopPackage "framework\$shopSide"
$shopLoaderPath=Join-Path $shopRoot $shopLoader
$shopCore=Join-Path $shopRoot 'sdgo\core.dll'
$shopGlobal=Join-Path $shopRoot 'sdgo\sdgo.ini'
if (Test-Path -LiteralPath $shopCore) {
    if ((Get-Item -LiteralPath $shopCore).VersionInfo.ProductName -ne 'SDGO mod framework') { throw 'Existing core.dll is not a recognized SDGO framework; installation preserved' }
}
$shopChainName=[IO.Path]::GetFileNameWithoutExtension($shopLoader)+'_chain.dll'
$shopNeedsChain=(Test-Path -LiteralPath $shopLoaderPath) -and -not (Shop-OurLoader $shopLoaderPath)
if ($shopNeedsChain -and (Test-Path -LiteralPath (Join-Path $shopRoot $shopChainName))) { throw 'Loader chain already occupied; installation preserved' }
$shopGlobalData=$(if ($shopNeedsChain) { Shop-Ini $shopGlobal } else { $null })
$oldSettings=Shop-Ini (Join-Path $shopMod 'mod.ini')
$shopPrevious=$(if (Test-Path -LiteralPath $shopReceiptPath) { Get-Content -LiteralPath $shopReceiptPath -Raw | ConvertFrom-Json } else { $null })
New-Item -ItemType Directory -Path (Join-Path $shopRoot 'sdgo\symbols'),$shopMod -Force | Out-Null
if (-not (Test-Path -LiteralPath $shopCore)) { Copy-Item -LiteralPath (Join-Path $shopFramework 'sdgo\core.dll') -Destination $shopCore }
if (-not (Shop-OurLoader $shopLoaderPath)) {
    if (Test-Path -LiteralPath $shopLoaderPath) {
        $chainName=$shopChainName
        $chain=Join-Path $shopRoot $chainName
        Rename-Item -LiteralPath $shopLoaderPath -NewName $chainName
        $global=$shopGlobalData
        if (-not $global.ContainsKey('loader')) { $global.loader=@{} }
        $global.loader['chain_'+[IO.Path]::GetFileNameWithoutExtension($shopLoader)]=$chainName
        Shop-WriteIni $shopGlobal $global
    }
    Copy-Item -LiteralPath (Join-Path $shopFramework $shopLoader) -Destination $shopLoaderPath
}
if (-not (Test-Path -LiteralPath $shopGlobal)) { Copy-Item -LiteralPath (Join-Path $shopFramework 'sdgo\sdgo.ini') -Destination $shopGlobal }
if ($Role -eq 'Labels') {
    if (-not (Test-Path -LiteralPath $shopSymbols)) {
        $baseSymbols=Join-Path $shopFramework 'sdgo\symbols\GOnline-54BE1568-00C97000.ini'
        if (Test-Path -LiteralPath $baseSymbols) { Copy-Item -LiteralPath $baseSymbols -Destination $shopSymbols }
        else { Shop-WriteIni $shopSymbols @{host=$shopFragment.host} }
    }
    foreach ($section in $shopAdded.Keys | Sort-Object) { Shop-AppendSymbol $shopSymbols $section $shopAdded[$section] }
}
$payload=Join-Path $shopPackage "payload\$shopId"
foreach ($file in Get-ChildItem -LiteralPath $payload -File) { Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $shopMod $file.Name) -Force }
$modIni=Join-Path $shopMod 'mod.ini'; $data=Shop-Ini $modIni
if ($oldSettings.ContainsKey('settings')) { foreach ($key in $oldSettings.settings.Keys) { $data.settings[$key]=$oldSettings.settings[$key] } }
if ($oldSettings.ContainsKey('mod') -and $oldSettings.mod.ContainsKey('enabled')) { $data.mod.enabled=$oldSettings.mod.enabled }
if ($shopConfigText) {
    $name=$(if ($Role -eq 'Labels') { 'display.ini' } else { 'catalog.ini' })
    $file=Join-Path $shopMod $name
    if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination ($file+'.'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffffffZ')+'.bak') }
    [IO.File]::WriteAllText($file,$shopConfigText,[Text.Encoding]::Unicode)
    if ($Role -eq 'Catalog') { $data.mod.enabled='1' }
}
Shop-WriteIni $modIni $data
if ($shopPrevious) {
    foreach ($property in $shopPrevious.addedSymbols.PSObject.Properties) { if (-not $shopAdded.ContainsKey($property.Name)) { $shopAdded[$property.Name]=$property.Value } }
}
@{schema_version=1;role=$Role;version=$shopManifest.version;mod=$shopId;addedSymbols=$shopAdded;installed_at=[DateTime]::UtcNow.ToString('o')} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $shopReceiptPath -Encoding UTF8
Write-Output "$shopId installed. Other mods and existing shared framework files preserved."
if ($Role -eq 'Catalog' -and -not (Test-Path -LiteralPath (Join-Path $shopMod 'catalog.ini'))) { Write-Output 'Catalog remains disabled until a host-owned profile is imported.' }
