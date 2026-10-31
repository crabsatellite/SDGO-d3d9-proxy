<#
.SYNOPSIS
  Update the bundled framework and add/update/remove the FRP network mods.
.EXAMPLE
  .\install_frp.ps1 -Target D:\Games\SDGO -Role Client -Profile D:\export\client\sdgo\mods\net_route\config.ini
.EXAMPLE
  .\install_frp.ps1 -Target D:\Server\Agent1 -Role Agent -Uninstall
#>
param(
    [Parameter(Mandatory=$true)][string]$Target,
    [ValidateSet('Client','Agent')][string]$Role='Client',
    [string]$Source,
    [string]$Profile,
    [switch]$Uninstall
)
$ErrorActionPreference='Stop'
if(-not $Source){$Source=Split-Path -Parent $MyInvocation.MyCommand.Path}
$root=(Resolve-Path -LiteralPath $Target).Path
$sourceRoot=(Resolve-Path -LiteralPath $Source).Path
$exeName=if($Role -eq 'Client'){'GOnline.exe'}else{'AgentServer.exe'}
$exe=Join-Path $root $exeName
if(-not(Test-Path -LiteralPath $exe -PathType Leaf)){throw "Missing $exeName at $root"}
if($Profile -and $Uninstall){throw 'Profile and Uninstall cannot be combined'}
foreach($part in @($root,(Join-Path $root 'sdgo'),(Join-Path $root 'sdgo\mods'),(Join-Path $root 'sdgo\symbols'),(Join-Path $root 'sdgo\install-receipts'))){
    if((Test-Path -LiteralPath $part) -and ((Get-Item -LiteralPath $part).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Linked target is not supported: $part"}
}
$running=Get-CimInstance Win32_Process -Filter "Name='$exeName'"
if($running | Where-Object { $_.ExecutablePath -eq $exe }){
    throw "Close $exeName in this target first. No process was stopped."
}
if($running | Where-Object {-not $_.ExecutablePath}){
    # An elevated unrelated client must not block a separate copy. Probe only
    # this executable's sharing lock, without writing or stopping any process.
    try{$probe=[IO.File]::Open($exe,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None);$probe.Close()}
    catch{throw "Cannot establish that this target is closed; no process was stopped: $exe"}
}
$mods=if($Role -eq 'Client'){@('net_route')}else{@('agent_relay_log_fix','agent_udp_pool')}
$stamp=[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8)
$receipt=Join-Path $root "sdgo\install-receipts\frp-$stamp"
function Assert-PlainTree([string]$path){
    if(Test-Path -LiteralPath $path){
        $items=@(Get-Item -LiteralPath $path)+@(Get-ChildItem -LiteralPath $path -Recurse -Force)
        if($items | Where-Object {$_.Attributes -band [IO.FileAttributes]::ReparsePoint}){throw "Linked files are not supported: $path"}
    }
}
function Hash-File([string]$file){
    $algorithm=[Security.Cryptography.SHA256]::Create();$stream=[IO.File]::OpenRead($file)
    try{return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','')}
    finally{$stream.Dispose();$algorithm.Dispose()}
}
function Append-Symbols([string]$path,[string]$text){
    $encoding=[Text.UTF8Encoding]::new($false)
    if(Test-Path -LiteralPath $path){
        $reader=[IO.StreamReader]::new($path,$true)
        try{[void]$reader.ReadToEnd();$encoding=$reader.CurrentEncoding}finally{$reader.Dispose()}
    }
    [IO.File]::AppendAllText($path,$text,$encoding)
}
foreach($id in $mods){Assert-PlainTree (Join-Path $root "sdgo\mods\$id")}
if($Uninstall){
    # Move only the named mods into a non-discovery directory; retain all settings.
    foreach($id in $mods){
        $path=[IO.Path]::GetFullPath((Join-Path $root "sdgo\mods\$id"))
        $expected=[IO.Path]::GetFullPath((Join-Path $root 'sdgo\mods'))+[IO.Path]::DirectorySeparatorChar
        if(-not $path.StartsWith($expected,[StringComparison]::OrdinalIgnoreCase)){throw 'Uninstall scope escaped mods directory'}
        if(Test-Path -LiteralPath $path){
            New-Item -ItemType Directory -Force -Path $receipt | Out-Null
            Move-Item -LiteralPath $path -Destination (Join-Path $receipt $id)
            Write-Host "Removed only $id (configuration retained in $receipt)"
        }
    }
    Write-Host 'Shared core, loader, symbols, sdgo.ini and other mods were retained. Restart the target to apply.'
    return
}
if(-not ('SdgoFrpInstall.Ini' -as [type])){
Add-Type -Namespace SdgoFrpInstall -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet=CharSet.Unicode)]
public static extern bool WritePrivateProfileString(string section,string key,string value,string file);
'@
}
function Read-Ini([string]$path){
    $sections=[ordered]@{};$section=''
    foreach($line in [IO.File]::ReadAllLines($path)){
        $text=$line.Trim()
        if($text.StartsWith(';') -or $text.StartsWith('#') -or -not $text){continue}
        if($text -match '^\[([^\]]+)\]$'){
            $section=$Matches[1];if($sections.Contains($section)){throw "Duplicate section in $path"}
            $sections[$section]=[ordered]@{};continue
        }
        $at=$text.IndexOf('=')
        if($section -and $at -gt 0){
            $key=$text.Substring(0,$at).Trim()
            if($sections[$section].Contains($key)){throw "Duplicate key [$section] $key in $path"}
            $sections[$section][$key]=$text.Substring($at+1).Trim()
        }
    }
    return ,$sections
}
$stream=[IO.File]::OpenRead($exe)
try{
    $reader=New-Object IO.BinaryReader($stream);$stream.Position=0x3c;$nt=$reader.ReadInt32()
    $stream.Position=$nt;if($reader.ReadUInt32() -ne 0x4550){throw 'Invalid PE target'}
    $stream.Position=$nt+8;$timestamp=$reader.ReadUInt32()
    $stream.Position=$nt+0x18+0x38;$imageSize=$reader.ReadUInt32()
}finally{$stream.Close()}
$hostKey='{0}-{1:X8}-{2:X8}' -f [IO.Path]::GetFileNameWithoutExtension($exe),$timestamp,$imageSize
$side=if($Role -eq 'Client'){'client'}else{'agent'}
$payload=Join-Path $sourceRoot "payload\$side"
$fragment=Join-Path $payload "symbols\$hostKey.ini"
if(-not(Test-Path -LiteralPath $fragment)){throw "Unsupported host $hostKey; no files changed"}
$manifestPath=Join-Path $sourceRoot 'SHA256.json'
if(-not(Test-Path -LiteralPath $manifestPath)){throw 'Incomplete kit: SHA256.json missing; no files changed'}
$inventory=ConvertFrom-Json ([IO.File]::ReadAllText($manifestPath))
$loaderName=if($Role -eq 'Client'){'d3d9.dll'}else{'winmm.dll'}
$requiredFiles=@('install_frp.ps1','install_client.ps1','install_server.ps1','install_common.ps1','framework/VERSION',
    "framework/$side/$loaderName","framework/$side/sdgo/core.dll","framework/$side/sdgo/sdgo.ini",
    "payload/$side/symbols/$hostKey.ini")
foreach($id in $mods){$requiredFiles+=@("payload/$side/mods/$id/$id.dll","payload/$side/mods/$id/mod.ini")}
foreach($relative in $requiredFiles){
    if(-not $inventory.PSObject.Properties[$relative]){throw "Incomplete kit inventory: $relative; no files changed"}
}
foreach($entry in $inventory.PSObject.Properties){
    # Owners edit these inputs while following the tutorial. They are never
    # executed or copied as active runtime settings by the installer.
    if($entry.Name -eq 'owner-tools/owner-profile.example.json' -or $entry.Name.EndsWith('/config.example.ini') -or $entry.Name.EndsWith('.md')){continue}
    $candidate=[IO.Path]::GetFullPath((Join-Path $sourceRoot ($entry.Name.Replace('/','\'))))
    if(-not $candidate.StartsWith($sourceRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){
        throw 'Kit inventory escaped its directory; no files changed'
    }
    Assert-PlainTree $candidate
    if($entry.Value -notmatch '^[a-fA-F0-9]{64}$' -or -not(Test-Path -LiteralPath $candidate -PathType Leaf) -or (Hash-File $candidate) -ine $entry.Value){
        throw "Kit checksum mismatch: $($entry.Name); no files changed"
    }
}
# The framework updater recursively copies this tree. Reject unlisted files so
# an old build or another mod cannot slip into the shared framework update.
$frameworkTree=Join-Path $sourceRoot "framework\$side"
Assert-PlainTree $frameworkTree
foreach($file in Get-ChildItem -LiteralPath $frameworkTree -File -Recurse -Force){
    $relative=$file.FullName.Substring($sourceRoot.Length+1).Replace('\','/')
    if(-not $inventory.PSObject.Properties[$relative]){throw "Unexpected framework file: $relative; no files changed"}
}
$addition=Read-Ini $fragment
foreach($pair in @(@('timestamp',$timestamp),@('size_of_image',$imageSize))){
    if([Convert]::ToUInt32($addition['host'][$pair[0]],16) -ne $pair[1]){throw 'Symbol fragment does not match target'}
}
$symbolPath=Join-Path $root "sdgo\symbols\$hostKey.ini"
Assert-PlainTree $symbolPath
$append=''
if(Test-Path -LiteralPath $symbolPath){
    $existing=Read-Ini $symbolPath
    foreach($section in $addition.Keys){
        if($existing.Contains($section)){
            foreach($key in $addition[$section].Keys){
                if($existing[$section][$key] -cne $addition[$section][$key]){throw "Symbol conflict: [$section] $key. Existing symbols retained; no files changed."}
            }
        }else{
            $append+="`r`n[$section]`r`n"
            foreach($key in $addition[$section].Keys){$append+="$key=$($addition[$section][$key])`r`n"}
        }
    }
}else{$append=[IO.File]::ReadAllText($fragment)}
foreach($id in $mods){
    $dir=Join-Path $payload "mods\$id";Assert-PlainTree $dir
    foreach($file in @('mod.ini',"$id.dll")){if(-not(Test-Path -LiteralPath (Join-Path $dir $file))){throw "Incomplete payload for $id"}}
}
$networkId=if($Role -eq 'Client'){'net_route'}else{'agent_udp_pool'}
$installedNetwork=Join-Path $root "sdgo\mods\$networkId"
if(-not $Profile -and -not(Test-Path -LiteralPath (Join-Path $installedNetwork 'mod.ini')) -and -not(Test-Path -LiteralPath (Join-Path $installedNetwork 'config.ini'))){
    throw 'First install requires -Profile with the owner configuration; no files changed'
}
$profileBytes=$null
if($Profile){
    $Profile=(Resolve-Path -LiteralPath $Profile).Path
    $settings=Read-Ini $Profile
    if($settings.Count -ne 1 -or -not $settings.Contains('settings')){throw 'Profile must contain only [settings]'}
    $required=if($Role -eq 'Client'){@('LoginIPv4','LoginPort','RouteCount')}else{@('FirstPort','PortCount')}
    foreach($key in $required){if(-not $settings['settings'].Contains($key)){throw "Missing profile setting $key"}}
    $profileBytes=[IO.File]::ReadAllBytes($Profile)
}
# Always update core and loader to this kit. User settings and other mods remain.
$bootstrap=Join-Path $sourceRoot "framework\$side"
$loaderName=if($Role -eq 'Client'){'d3d9.dll'}else{'winmm.dll'}
function Framework-Version([string]$file){
    $v=(Get-Item -LiteralPath $file).VersionInfo
    return [version]('{0}.{1}.{2}.{3}' -f $v.FileMajorPart,$v.FileMinorPart,$v.FileBuildPart,$v.FilePrivatePart)
}
$versionManifest=Join-Path $sourceRoot 'framework\VERSION'
$declaredVersion=$null
if(Test-Path -LiteralPath $versionManifest){$declaredVersion=[version](([IO.File]::ReadAllText($versionManifest).Trim())+'.0')}
foreach($relative in @('sdgo\core.dll',$loaderName)){
    $candidate=Join-Path $bootstrap $relative;$installed=Join-Path $root $relative
    if(-not(Test-Path -LiteralPath $candidate -PathType Leaf)){throw "Missing bundled framework file: $relative"}
    if($declaredVersion -and (Framework-Version $candidate) -ne $declaredVersion){throw "Stale framework file in this kit: $relative. Rebuild or obtain the latest kit; no files changed."}
    Assert-PlainTree $installed
    if(Test-Path -LiteralPath $installed){
        $isFramework=$relative -eq 'sdgo\core.dll' -or (Get-Item -LiteralPath $installed).VersionInfo.ProductName -eq 'SDGO mod framework'
        if($isFramework -and (Framework-Version $installed) -gt (Framework-Version $candidate)){
            throw "This kit is older than the installed framework ($relative). Obtain the latest kit; no files changed."
        }
    }
}
New-Item -ItemType Directory -Force -Path $receipt | Out-Null
foreach($relative in @('sdgo\core.dll',$loaderName)){
    $installed=Join-Path $root $relative
    if(Test-Path -LiteralPath $installed){Copy-Item -LiteralPath $installed -Destination (Join-Path $receipt ([IO.Path]::GetFileName($relative)))}
}
& {
    $bootstrap=Join-Path $sourceRoot "framework\$side"
    $installer=Join-Path $sourceRoot $(if($Role -eq 'Client'){'install_client.ps1'}else{'install_server.ps1'})
    $arguments=if($Role -eq 'Client'){@('-GameDir',$root)}else{@('-ServerRoot',$root,'-Role','Agent')}
    $previousPreference=$ErrorActionPreference
    try{
        $ErrorActionPreference='Continue'
        $bootstrapOutput=& powershell -NoProfile -ExecutionPolicy Bypass -File $installer @arguments -Source $bootstrap 2>&1
    }finally{$ErrorActionPreference=$previousPreference}
    if($LASTEXITCODE -ne 0){throw "Framework bootstrap failed: $($bootstrapOutput -join [Environment]::NewLine)"}
    foreach($relative in @('sdgo\core.dll',$loaderName)){
        if((Hash-File (Join-Path $root $relative)) -ne (Hash-File (Join-Path $bootstrap $relative))){throw "Framework update verification failed: $relative"}
    }
    Write-Host "Updated core and loader to the bundled version $(Framework-Version (Join-Path $root 'sdgo\core.dll')); adding FRP mods below"
}
New-Item -ItemType Directory -Force -Path $receipt,(Split-Path -Parent $symbolPath) | Out-Null
if(Test-Path -LiteralPath $symbolPath){Copy-Item -LiteralPath $symbolPath -Destination (Join-Path $receipt 'symbols-before.ini')}
if($append){Append-Symbols $symbolPath ("`r`n"+$append)}
foreach($id in $mods){
    $destination=Join-Path $root "sdgo\mods\$id"
    if(Test-Path -LiteralPath $destination){Copy-Item -LiteralPath $destination -Destination (Join-Path $receipt $id) -Recurse}
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    $old=if(Test-Path -LiteralPath (Join-Path $destination 'mod.ini')){Read-Ini (Join-Path $destination 'mod.ini')}else{$null}
    Copy-Item -LiteralPath (Join-Path $payload "mods\$id\$id.dll") -Destination $destination -Force
    $template=Join-Path $payload "mods\$id\config.example.ini"
    if(Test-Path -LiteralPath $template){Copy-Item -LiteralPath $template -Destination $destination -Force}
    $manifest=Join-Path $destination 'mod.ini'
    Copy-Item -LiteralPath (Join-Path $payload "mods\$id\mod.ini") -Destination $manifest -Force
    # Preserve legacy settings; a separate config.ini takes precedence at runtime.
    if($old -and $old.Contains('settings')){foreach($key in $old['settings'].Keys){[void][SdgoFrpInstall.Ini]::WritePrivateProfileString('settings',$key,$old['settings'][$key],$manifest)}}
    $enabled=if($old -and $old.Contains('mod') -and $old['mod'].Contains('enabled')){$old['mod']['enabled']}else{'1'}
    if($Profile){$enabled='1'}
    [void][SdgoFrpInstall.Ini]::WritePrivateProfileString('mod','enabled',$enabled,$manifest)
    if($Profile -and $id -eq $(if($Role -eq 'Client'){'net_route'}else{'agent_udp_pool'})){
        [IO.File]::WriteAllBytes((Join-Path $destination 'config.ini'),$profileBytes)
    }
    Write-Host "Installed $id; other mods and global switches retained"
}
Write-Host "Receipt and prior settings: $receipt"
Write-Host 'Use the existing game launcher. Restart the target after changing settings. No FRP account is needed on players computers.'
