# Bounded GameTest application rule; never changes a firewall profile or notifications.
# https://learn.microsoft.com/windows/win32/api/netfw/nn-netfw-inetfwrule
param(
    [Parameter(Mandatory)][ValidateSet('Install','Remove')][string]$Action,
    [Parameter(Mandatory)][string]$Program,
    [Parameter(Mandatory)][Guid]$RunId,
    [Parameter(Mandatory)][string]$RemoteIPv4
)
$ErrorActionPreference='Stop'
$added=$false
try {
    $programPath=[IO.Path]::GetFullPath($Program)
    if (-not [IO.Path]::IsPathRooted($Program) -or [IO.Path]::GetFileName($programPath) -ne 'GOnline.exe') {throw 'Expected an absolute copied GOnline.exe path'}
    $address=[Net.IPAddress]::Parse($RemoteIPv4)
    if ($address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {throw 'Expected an explicit IPv4 test endpoint'}
    $name='SDGO.GameTest.'+$RunId.ToString()
    $description='Owned temporary native GameTest '+$RunId.ToString()
    $policy=New-Object -ComObject HNetCfg.FwPolicy2
    $existing=$null
    try {$existing=$policy.Rules.Item($name)} catch {}
    if ($existing -and (-not [string]::Equals($existing.ApplicationName,$programPath,[StringComparison]::OrdinalIgnoreCase) -or $existing.Description -ne $description)) {throw 'Firewall rule ownership mismatch'}
    if ($Action -eq 'Install') {
        if ($existing) {throw 'A firewall rule already exists for this run'}
        if (-not (Test-Path -LiteralPath $programPath -PathType Leaf)) {throw 'Copied game is missing'}
        $rule=New-Object -ComObject HNetCfg.FwRule
        $rule.Name=$name
        $rule.Description=$description
        $rule.ApplicationName=$programPath
        $rule.Protocol=256
        $rule.Direction=1
        $rule.Action=1
        $rule.Profiles=7
        $rule.RemoteAddresses=$address.ToString()
        $rule.EdgeTraversal=$false
        $rule.Enabled=$true
        $policy.Rules.Add($rule)
        $added=$true
        $readback=$policy.Rules.Item($name)
        if (-not $readback.Enabled -or $readback.Action -ne 1 -or $readback.Direction -ne 1 -or $readback.Protocol -ne 256 -or $readback.Profiles -ne 7 -or $readback.RemoteAddresses -ne $address.ToString() -or -not [string]::Equals($readback.ApplicationName,$programPath,[StringComparison]::OrdinalIgnoreCase)) {throw 'Firewall rule readback mismatch'}
        @{status='installed';name=$name;program=$programPath;remote_ipv4=$address.ToString();profiles=7;direction='inbound';edge_traversal=$false}|ConvertTo-Json -Compress
    } else {
        if ($existing) {$policy.Rules.Remove($name)}
        $remaining=$null
        try {$remaining=$policy.Rules.Item($name)} catch {}
        if ($remaining) {throw 'Owned firewall rule was not removed'}
        @{status='removed';name=$name;program=$programPath}|ConvertTo-Json -Compress
    }
} catch {
    if ($added) {try {$policy.Rules.Remove($name)} catch {}}
    Write-Error ('Quiet GameTest firewall setup failed: '+$_.Exception.Message)
    exit 1
}
