param([string]$ClientRoot,[string]$ServerIni,[string]$Workspace)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms
if (-not $ClientRoot) {
    $dialog=New-Object Windows.Forms.FolderBrowserDialog
    $dialog.Description='选择含有 GOnline.exe 和 GPackData.zpk 的游戏客户端'
    if ($dialog.ShowDialog() -ne 'OK') { return }
    $ClientRoot=$dialog.SelectedPath
}
if (-not $ServerIni) {
    $dialog=New-Object Windows.Forms.OpenFileDialog
    $dialog.Title='选择本机服务端的 StoreServer.ini（凭据仅供本机助手使用）'
    $dialog.Filter='StoreServer.ini|StoreServer.ini|INI files|*.ini'
    if ($dialog.ShowDialog() -ne 'OK') { return }
    $ServerIni=$dialog.FileName
}
if (-not $Workspace) { $Workspace=Join-Path $env:LOCALAPPDATA 'SDGOAssistant\operators' }
[IO.Directory]::CreateDirectory($Workspace) | Out-Null
$python=Get-Command python -ErrorAction SilentlyContinue
if (-not $python) { throw '宿主助手需要 Python，以及 requirements-operator-assistant.txt 中的依赖。玩家客户端不需要 Python。' }
$snapshot=Join-Path $Workspace 'source'
if (-not (Test-Path -LiteralPath $snapshot)) {
    & $python.Source -B -X utf8 (Join-Path $PSScriptRoot 'operator_snapshot.py') --server-ini $ServerIni --output $snapshot
    if ($LASTEXITCODE -ne 0) { throw '副官配置只读导入未完成。' }
}
function Quote-AssistantArg([string]$Value) {
    '"'+([regex]::Replace([regex]::Replace($Value,'(\\*)"','$1$1\"'),'(\\+)$','$1$1'))+'"'
}
$arguments=@('-B','-X','utf8',(Quote-AssistantArg (Join-Path $PSScriptRoot 'mod_assistant.py')),
    '--workspace',(Quote-AssistantArg $Workspace),'--client',(Quote-AssistantArg $ClientRoot),
    '--snapshot',(Quote-AssistantArg $snapshot),'--server-ini',(Quote-AssistantArg $ServerIni))
$started=[DateTime]::UtcNow
$process=Start-Process -FilePath $python.Source -ArgumentList $arguments -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $Workspace 'assistant.stdout.log') `
    -RedirectStandardError (Join-Path $Workspace 'assistant.stderr.log')
$session=Join-Path $Workspace 'assistant-session.json'
for ($attempt=0;$attempt -lt 100;$attempt++) {
    if ((Test-Path -LiteralPath $session) -and [IO.File]::GetLastWriteTimeUtc($session) -ge $started) {
        $data=Get-Content -LiteralPath $session -Raw -Encoding UTF8 | ConvertFrom-Json
        Start-Process $data.url
        Write-Output ('副官助手已打开。PID '+$process.Id)
        return
    }
    if ($process.HasExited) { throw '助手未能启动，请查看工作目录中的 assistant.stderr.log。' }
    Start-Sleep -Milliseconds 100
}
throw '助手启动超时，请查看工作目录中的日志。'
