<# Update room_random, core and installed AI DLLs in one STOPPED BattleServer. No restart. #>
param(
    [Parameter(Mandatory=$true)][string]$ServerRoot,
    [Parameter(Mandatory=$true)][string]$DatabaseIni,
    [string]$Source
)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if(-not $Source){
    $Source=if(Test-Path -LiteralPath (Join-Path $repo 'server\sdgo\mods\room_random')){Join-Path $repo 'server'}else{Join-Path $repo 'build\stage\Release\server'}
}
$Source=(Resolve-Path -LiteralPath $Source).Path
$ServerRoot=(Resolve-Path -LiteralPath $ServerRoot).Path
$DatabaseIni=(Resolve-Path -LiteralPath $DatabaseIni).Path
$exe=Join-Path $ServerRoot 'BattleServer.exe'
if(-not (Test-Path -LiteralPath $exe -PathType Leaf)){throw 'ServerRoot must be one BattleServer.exe folder'}
if((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne 'F74D16037FC8F4FE49A8041488AABD1948DF1B1A9F2D125DE9DAD4A2347B63A4'){
    throw 'Unsupported BattleServer executable; no files changed'
}
# An exclusive writable open is only a lock check; no executable bytes are written.
# Refuse a running image rather than stopping any process or service.
try{$imageLock=[IO.File]::Open($exe,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$imageLock.Close()}
catch{throw 'Stop this BattleServer before installing; no process was stopped'}
$sdgo=Join-Path $ServerRoot 'sdgo'
foreach($required in @((Join-Path $sdgo 'core.dll'),(Join-Path $ServerRoot 'winmm.dll'))){
    if(-not (Test-Path -LiteralPath $required -PathType Leaf)){throw 'Install the mod framework first; no files changed'}
}
$hostName='BattleServer-548A891D-0046C000.ini'
$sourceSymbol=Join-Path $Source "sdgo\symbols\$hostName"
$sourceMod=Join-Path $Source 'sdgo\mods\room_random'
foreach($path in @($sourceSymbol,(Join-Path $sourceMod 'room_random.dll'),(Join-Path $sourceMod 'mod.ini'))){
    if(-not (Test-Path -LiteralPath $path -PathType Leaf)){throw "Incomplete source package: $path"}
}
if(-not ('RoomRandomInstall.Ini' -as [type])){
    Add-Type -Namespace RoomRandomInstall -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet=CharSet.Unicode)]
public static extern uint GetPrivateProfileString(string section,string key,string def,System.Text.StringBuilder value,uint size,string file);
[DllImport("kernel32.dll", CharSet=CharSet.Unicode)]
public static extern bool WritePrivateProfileString(string section,string key,string value,string file);
'@
}
function IniValue($file,$section,$key){
    $buffer=New-Object Text.StringBuilder 2048
    [void][RoomRandomInstall.Ini]::GetPrivateProfileString($section,$key,'',$buffer,2048,$file)
    $buffer.ToString()
}
foreach($key in @('DBServerName','DBName','ID','PWD')){
    if(-not (IniValue $DatabaseIni 'DB Game' $key)){throw 'DatabaseIni must contain the native DB Game connection settings; no files changed'}
}
function Sections($path){
    $result=[ordered]@{};$name=''
    foreach($line in [IO.File]::ReadAllLines($path)){
        if($line -match '^\s*\[([^\]]+)\]\s*$'){$name=$Matches[1];$result[$name]=[ordered]@{}}
        elseif($name -and $line -match '^\s*([^;#=][^=]*)=(.*)$'){$result[$name][$Matches[1].Trim()]=$Matches[2].Trim()}
    }
    return ,$result
}
function Address($value){if($value.StartsWith('0x')){[Convert]::ToUInt32($value.Substring(2),16)}else{[uint32]$value}}
$symbols=Join-Path $sdgo "symbols\$hostName"
$incoming=Sections $sourceSymbol
$text=if(Test-Path -LiteralPath $symbols){[IO.File]::ReadAllText($symbols)}else{''}
$existing=if($text){Sections $symbols}else{[ordered]@{}}
foreach($name in $incoming.Keys){
    if($existing.Contains($name)){
        if($name -eq 'host'){
            foreach($key in @('timestamp','size_of_image')){
                if((Address $incoming[$name][$key]) -ne (Address $existing[$name][$key])){throw 'Existing symbol host differs; no files changed'}
            }
        }elseif((Address $incoming[$name]['rva']) -ne (Address $existing[$name]['rva']) -or $incoming[$name]['kind'] -ne $existing[$name]['kind']){
            throw "Existing symbol $name conflicts; no files changed"
        }
        continue # Preserve existing expectations, comments and other mods' symbols.
    }
    $text+="`r`n[$name]`r`n"
    foreach($key in $incoming[$name].Keys){$text+="$key=$($incoming[$name][$key])`r`n"}
}
$mod=Join-Path $sdgo 'mods\room_random'
$config=Join-Path $mod 'config.ini'
# The shared grammar lives in core and the three server mods. Update only the
# AI mods already installed by the owner; preserve their manifests/settings.
$updates=[ordered]@{
    'core.dll'='core.dll'
    'mods\room_random\room_random.dll'='mods\room_random\room_random.dll'
    'mods\room_random\mod.ini'='mods\room_random\mod.ini'
    'mods\room_random\manager.json'='mods\room_random\manager.json'
}
foreach($id in @('pvp_ai','coop_ai')){
    $relative="mods\$id\$id.dll"
    if(Test-Path -LiteralPath (Join-Path $sdgo $relative) -PathType Leaf){
        $manifest=Join-Path $sdgo "mods\$id\mod.ini"
        if((IniValue $manifest 'mod' 'id') -ne $id -or (IniValue $manifest 'mod' 'dll') -ne "$id.dll"){
            throw "Existing $id manifest is unsupported; no files changed"
        }
        $updates[$relative]=$relative
    }
}
foreach($relative in $updates.Keys){
    if(-not (Test-Path -LiteralPath (Join-Path $Source "sdgo\$relative") -PathType Leaf)){
        throw "Incomplete source package: $relative; no files changed"
    }
    $destination=Join-Path $sdgo $relative
    if(Test-Path -LiteralPath $destination -PathType Leaf){
        try{$fileLock=[IO.File]::Open($destination,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$fileLock.Close()}
        catch{throw "Framework/mod file $relative is in use; no files changed"}
    }
}
$backup=Join-Path $sdgo ('backups\room_random\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')+'-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $backup,$mod,(Split-Path -Parent $symbols)|Out-Null
foreach($relative in @($updates.Keys)+@("symbols\$hostName",'mods\room_random\config.ini')){
    $file=Join-Path $sdgo $relative
    if(Test-Path -LiteralPath $file -PathType Leaf){
        $saved=Join-Path $backup $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $saved)|Out-Null
        Copy-Item -LiteralPath $file -Destination $saved
    }
}
foreach($relative in $updates.Keys){
    Copy-Item -LiteralPath (Join-Path $Source "sdgo\$relative") -Destination (Join-Path $sdgo $relative) -Force
}
if(-not (Test-Path -LiteralPath $config)){
    # UTF-16 INI supports Unicode host paths through the Windows profile API.
    [IO.File]::WriteAllText($config,"[settings]`r`nDriver=SQL Server`r`nQueryTimeoutSeconds=2`r`nMaxRowsPerPlayer=4096`r`n",[Text.Encoding]::Unicode)
}
if(-not [RoomRandomInstall.Ini]::WritePrivateProfileString('settings','DatabaseIni',$DatabaseIni,$config)){throw 'Could not save DatabaseIni; originals are in the backup folder'}
[IO.File]::WriteAllText($symbols,$text,(New-Object Text.UTF8Encoding $false))
foreach($relative in $updates.Keys){
    if((Get-FileHash -LiteralPath (Join-Path $sdgo $relative)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $Source "sdgo\$relative")).Hash){throw "Installed file verification failed: $relative"}
}
if((IniValue $config 'settings' 'DatabaseIni') -ne $DatabaseIni){throw 'Installed configuration verification failed'}
Write-Host "room_random installed in $ServerRoot"
Write-Host "Backups: $backup"
Write-Host 'Core and already installed pvp_ai/coop_ai DLLs updated for shared room commands; AI settings preserved.'
Write-Host 'Start this server normally. Add !rand to a room title. No database script or client update is required.'
