<# Install/update only room_random in one STOPPED BattleServer folder. No restart. #>
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
if(Test-Path -LiteralPath (Join-Path $mod 'room_random.dll')){
    try{$modLock=[IO.File]::Open((Join-Path $mod 'room_random.dll'),[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None);$modLock.Close()}
    catch{throw 'room_random.dll is in use; no files changed'}
}
$backup=Join-Path $sdgo ('backups\room_random\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')+'-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $backup,$mod,(Split-Path -Parent $symbols)|Out-Null
foreach($file in @($symbols,(Join-Path $mod 'room_random.dll'),(Join-Path $mod 'mod.ini'),$config)){
    if(Test-Path -LiteralPath $file -PathType Leaf){Copy-Item -LiteralPath $file -Destination (Join-Path $backup ([IO.Path]::GetFileName($file)))}
}
Copy-Item -LiteralPath (Join-Path $sourceMod 'room_random.dll') -Destination $mod -Force
Copy-Item -LiteralPath (Join-Path $sourceMod 'mod.ini') -Destination $mod -Force
if(-not (Test-Path -LiteralPath $config)){
    # UTF-16 INI supports Unicode host paths through the Windows profile API.
    [IO.File]::WriteAllText($config,"[settings]`r`nDriver=SQL Server`r`nQueryTimeoutSeconds=2`r`nMaxRowsPerPlayer=4096`r`n",[Text.Encoding]::Unicode)
}
if(-not [RoomRandomInstall.Ini]::WritePrivateProfileString('settings','DatabaseIni',$DatabaseIni,$config)){throw 'Could not save DatabaseIni; originals are in the backup folder'}
[IO.File]::WriteAllText($symbols,$text,(New-Object Text.UTF8Encoding $false))
if((Get-FileHash -LiteralPath (Join-Path $mod 'room_random.dll')).Hash -ne (Get-FileHash -LiteralPath (Join-Path $sourceMod 'room_random.dll')).Hash){throw 'Installed DLL verification failed'}
if((IniValue $config 'settings' 'DatabaseIni') -ne $DatabaseIni){throw 'Installed configuration verification failed'}
Write-Host "room_random installed in $ServerRoot"
Write-Host "Backups: $backup"
Write-Host 'Start this server normally. Add !rand to a room title. No database script or client update is required.'
