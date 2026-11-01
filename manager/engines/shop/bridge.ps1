param([Parameter(Mandatory=$true)][string]$Library)
$ErrorActionPreference='Stop'
[Console]::InputEncoding=[Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
Add-Type -Path $Library
while($null -ne ($request=[Console]::ReadLine())) {
    [Console]::WriteLine([Sdgo.Manager.ShopApi]::Execute($request))
}
