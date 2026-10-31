# Isolated installer regression; never touches an actual game/server folder.
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$work=Join-Path $repo ('build\frp_install_test_run\'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
if(-not ('SdgoFrpSymbolTest.Ini' -as [type])){
Add-Type -Namespace SdgoFrpSymbolTest -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet=CharSet.Unicode)]
public static extern uint GetPrivateProfileString(string section,string key,string def,System.Text.StringBuilder value,uint size,string file);
'@
}
function Write-Text($path,$text){New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path)|Out-Null;[IO.File]::WriteAllText($path,$text)}
function Assert($condition,$message){if(-not $condition){throw "FAIL $message"};Write-Host "ok $message"}
function Fake-Exe($path){
    $bytes=New-Object byte[] 0x100
    [BitConverter]::GetBytes([int]0x40).CopyTo($bytes,0x3c)
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($bytes,0x40)
    [BitConverter]::GetBytes([uint32]0x12345678).CopyTo($bytes,0x48)
    [BitConverter]::GetBytes([uint32]0x1000).CopyTo($bytes,0x90)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path)|Out-Null
    [IO.File]::WriteAllBytes($path,$bytes)
}
function Refresh-Inventory($source){
    $inventory=[ordered]@{}
    foreach($file in Get-ChildItem -LiteralPath $source -File -Recurse -Force){
        if($file.FullName -eq (Join-Path $source 'SHA256.json')){continue}
        $relative=$file.FullName.Substring($source.Length+1).Replace('\','/')
        $inventory[$relative]=(Get-FileHash -LiteralPath $file.FullName).Hash
    }
    Write-Text (Join-Path $source 'SHA256.json') ($inventory|ConvertTo-Json)
}
function Install($target,$side,$source,$profile,$remove=$false,$fails=$false,$refresh=$true,$reason='Symbol conflict'){
    if($refresh -and -not $remove){Refresh-Inventory $source}
    $arguments=@('-Target',$target,'-Role',$side,'-Source',$source)
    if($profile){$arguments+=@('-Profile',$profile)}
    if($remove){$arguments+='-Uninstall'}
    $previous=$ErrorActionPreference
    try{$ErrorActionPreference='Continue';$output=& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\install_frp.ps1') @arguments 2>&1}
    finally{$ErrorActionPreference=$previous}
    if($fails){Assert ($LASTEXITCODE -ne 0 -and ($output -join "`n").Contains($reason)) "$reason rejected before installation";return}
    if($LASTEXITCODE -ne 0){throw ($output -join "`n")}
}
foreach($side in @('Client','Agent')){
    $lower=if($side -eq 'Client'){'client'}else{'agent'}
    $exeName=if($side -eq 'Client'){'GOnline'}else{'AgentServer'}
    $ids=if($side -eq 'Client'){@('net_route')}else{@('agent_relay_log_fix','agent_udp_pool')}
    $source=Join-Path $work "kit $lower";$target=Join-Path $work "existing $lower"
    Write-Text (Join-Path $source 'install_common.ps1') ([IO.File]::ReadAllText((Join-Path $repo 'tools\install_common.ps1')))
    Fake-Exe (Join-Path $target "$exeName.exe")
    $hostKey="$exeName-12345678-00001000"
    $hostText="[host]`r`nimage=$exeName.exe`r`ntimestamp=0x12345678`r`nsize_of_image=0x00001000`r`n"
    Write-Text (Join-Path $source "payload\$lower\symbols\$hostKey.ini") ($hostText+"[network.new]`r`nkind=data`r`nrva=0x80`r`n")
    foreach($id in $ids){
        Write-Text (Join-Path $source "payload\$lower\mods\$id\mod.ini") "[mod]`r`nid=$id`r`nenabled=0`r`nversion=0.2.0`r`n[settings]`r`nAlpha=1`r`n"
        Write-Text (Join-Path $source "payload\$lower\mods\$id\$id.dll") 'new network DLL'
        Write-Text (Join-Path $target "sdgo\mods\$id\mod.ini") "[mod]`r`nid=$id`r`nenabled=1`r`nversion=0.1.0`r`n[settings]`r`nAlpha=7`r`n"
    }
    $profile=Join-Path $work "$lower-profile.ini"
    $settings=if($side -eq 'Client'){"LoginIPv4=192.0.2.41`r`nLoginPort=15123`r`nRouteCount=1`r`nRoute1=tcp,192.0.2.41,15123,node.example.invalid,45123"}else{"FirstPort=24100`r`nPortCount=4"}
    Write-Text $profile ("[settings]`r`n"+$settings)
    $ai=Join-Path $target 'sdgo\mods\match_ai\mod.ini'
    $global=Join-Path $target 'sdgo\sdgo.ini'
    $core=Join-Path $target 'sdgo\core.dll'
    $symbols=Join-Path $target "sdgo\symbols\$hostKey.ini"
    Write-Text $ai "[mod]`r`nid=match_ai`r`nversion=newer-than-frp`r`nrequires=twenty.new.symbols"
    Write-Text $global "[mods]`r`nmatch_ai=1`r`nmission_ai=1`r`nnet_probe=0`r`nnet_route=0"
    Write-Text $core 'outdated core must be replaced'
    Write-Text $symbols ($hostText+"; keep this comment`r`n[twenty.new.symbols]`r`nkind=data`r`nrva=0x90`r`n")
    $oldSymbols=[IO.File]::ReadAllText($symbols)
    $boot=Join-Path $source "framework\$lower"
    $loader=if($side -eq 'Client'){'d3d9.dll'}else{'winmm.dll'}
    New-Item -ItemType Directory -Force -Path (Join-Path $boot 'sdgo\mods'),(Join-Path $boot 'sdgo\symbols') | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $boot $loader),[Text.Encoding]::Unicode.GetBytes('sdgo\core.dll latest loader'))
    [IO.File]::WriteAllBytes((Join-Path $target $loader),[Text.Encoding]::Unicode.GetBytes('sdgo\core.dll old loader'))
    Write-Text (Join-Path $boot 'sdgo\core.dll') 'latest framework core'
    Write-Text (Join-Path $boot 'sdgo\sdgo.ini') "[core]`r`nlog_level=1"
    Write-Text (Join-Path $source 'framework\VERSION') '0.0.0'
    Write-Text (Join-Path $source 'owner-tools\owner-profile.example.json') '{}'
    foreach($installer in @('install_frp.ps1','install_client.ps1','install_server.ps1')){Copy-Item -LiteralPath (Join-Path $repo "tools\$installer") -Destination $source}
    if($side -eq 'Agent'){
        [IO.File]::WriteAllText($symbols,$oldSymbols,[Text.Encoding]::Unicode)
    }
    $protected=@($ai,$global)
    $hashes=@{};foreach($p in $protected){$hashes[$p]=(Get-FileHash -LiteralPath $p).Hash}
    Install $target $side $source $profile
    foreach($p in $protected){Assert ((Get-FileHash -LiteralPath $p).Hash -eq $hashes[$p]) "$side preserves $([IO.Path]::GetFileName($p))"}
    Assert ([IO.File]::ReadAllText($core) -eq 'latest framework core') "$side replaces outdated core with bundled version"
    $installedLoader=Join-Path $target $loader
    Assert ((Get-FileHash -LiteralPath $installedLoader).Hash -eq (Get-FileHash -LiteralPath (Join-Path $boot $loader)).Hash) "$side replaces outdated loader"
    $protected+=@($core,$installedLoader);foreach($p in @($core,$installedLoader)){$hashes[$p]=(Get-FileHash -LiteralPath $p).Hash}
    Assert ([IO.File]::ReadAllText($symbols).StartsWith($oldSymbols)) "$side preserves newer symbols and comments"
    Assert ([IO.File]::ReadAllText($symbols).Contains('[network.new]')) "$side appends network symbols"
    if($side -eq 'Agent'){
        $encoded=[IO.File]::ReadAllBytes($symbols)
        Assert ($encoded[0] -eq 255 -and $encoded[1] -eq 254 -and [Text.Encoding]::Unicode.GetString($encoded).Contains('[network.new]')) 'UTF-16 symbol encoding remains valid'
        $value=New-Object Text.StringBuilder 64
        [void][SdgoFrpSymbolTest.Ini]::GetPrivateProfileString('network.new','rva','missing',$value,64,$symbols)
        Assert ($value.ToString() -eq '0x80') 'Windows INI API reads appended UTF-16 symbol'
    }
    $network=if($side -eq 'Client'){'net_route'}else{'agent_udp_pool'}
    $config=Join-Path $target "sdgo\mods\$network\config.ini"
    Assert ([IO.File]::ReadAllText($config) -eq [IO.File]::ReadAllText($profile)) "$side applies explicit profile"
    Write-Text (Join-Path $source 'owner-tools\owner-profile.example.json') '{"owner":"edited template"}'
    Install $target $side $source $profile $false $false $false
    Assert $true "$side owner template editing keeps executable checksums usable"
    Install $target $side $source $config
    Assert ([IO.File]::ReadAllText($config) -eq [IO.File]::ReadAllText($profile)) "$side accepts an in-place profile"
    Install $target $side $source $null
    Assert ([IO.File]::ReadAllText($config) -eq [IO.File]::ReadAllText($profile)) "$side update preserves connection profile"
    Assert ([IO.File]::ReadAllText((Join-Path $target "sdgo\mods\$network\mod.ini")).Contains('Alpha=7')) "$side preserves legacy settings"
    $fragment=Join-Path $source "payload\$lower\symbols\$hostKey.ini"
    Write-Text $fragment ($hostText+"[twenty.new.symbols]`r`nkind=data`r`nrva=0x91`r`n")
    $before=(Get-FileHash -LiteralPath $symbols).Hash
    Install $target $side $source $profile $false $true
    Assert ((Get-FileHash -LiteralPath $symbols).Hash -eq $before) "$side conflict makes no symbol writes"
    Write-Text $fragment ($hostText+"[network.new]`r`nkind=data`r`nrva=0x80`r`n")
    Refresh-Inventory $source
    $corrupt=Join-Path $source "payload\$lower\mods\$network\$network.dll"
    Write-Text $corrupt 'stale DLL mixed into a current kit'
    Install $target $side $source $profile $false $true $false 'Kit checksum mismatch'
    Write-Text $corrupt 'new network DLL'
    Refresh-Inventory $source
    $unexpected=Join-Path $boot 'sdgo\mods\foreign\mod.ini'
    Write-Text $unexpected '[mod]'
    Install $target $side $source $profile $false $true $false 'Unexpected framework file'
    Assert (-not(Test-Path -LiteralPath (Join-Path $target 'sdgo\mods\foreign'))) "$side refuses unlisted mod before any copy"
    Remove-Item -LiteralPath $unexpected
    Install $target $side $source $null $true
    foreach($id in $ids){Assert (-not(Test-Path -LiteralPath (Join-Path $target "sdgo\mods\$id"))) "$side removes only $id"}
    foreach($p in $protected){Assert ((Get-FileHash -LiteralPath $p).Hash -eq $hashes[$p]) "$side uninstall preserves $([IO.Path]::GetFileName($p))"}
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $target 'sdgo\install-receipts') -Filter config.ini -Recurse).Count -gt 0) "$side uninstall retains configuration"
    # Fresh target bootstraps the same generic framework, then adds only FRP.
    Write-Text $fragment ($hostText+"[network.new]`r`nkind=data`r`nrva=0x80`r`n")
    $fresh=Join-Path $work "fresh $lower second layout"
    Fake-Exe (Join-Path $fresh "$exeName.exe")
    Install $fresh $side $source $null $false $true $true 'First install requires -Profile'
    Assert (-not(Test-Path -LiteralPath (Join-Path $fresh 'sdgo\core.dll'))) "$side unconfigured first install makes no framework writes"
    Install $fresh $side $source $profile
    Assert (Test-Path -LiteralPath (Join-Path $fresh $loader)) "$side fresh install provides loader"
    Assert ([IO.File]::ReadAllText((Join-Path $fresh 'sdgo\core.dll')) -eq 'latest framework core') "$side fresh install provides common core"
    Assert (@(Get-ChildItem -LiteralPath (Join-Path $fresh 'sdgo\mods') -Directory).Count -eq $ids.Count) "$side fresh install adds only selected mods"
    Copy-Item -LiteralPath (Join-Path $repo 'tools\install_frp.ps1') -Destination $source
    Refresh-Inventory $source
    $defaultOutput=& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $source 'install_frp.ps1') -Target $fresh -Role $side -Profile $profile 2>&1
    Assert ($LASTEXITCODE -eq 0) "$side package defaults Source to its own directory"
}
Write-Host "ALL FRP INCREMENTAL INSTALL CHECKS PASSED`nReceipt: $work"
