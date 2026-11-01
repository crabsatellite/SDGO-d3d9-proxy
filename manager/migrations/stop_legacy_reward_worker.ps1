param(
    [Parameter(Mandatory = $true)][string]$Workspace,
    [string]$Runtime,
    [string]$ToolRoot = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'

function Get-FullPath([string]$Value) {
    [IO.Path]::GetFullPath($Value)
}

function Test-SamePath([string]$Left, [string]$Right) {
    [string]::Equals((Get-FullPath $Left), (Get-FullPath $Right),
        [StringComparison]::OrdinalIgnoreCase)
}

function Open-OwnerLock([string]$Path) {
    $lastError = $null
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        try {
            return [IO.File]::Open($Path, [IO.FileMode]::OpenOrCreate,
                [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        } catch [IO.IOException] {
            $lastError = $_.Exception
            Start-Sleep -Milliseconds 100
        }
    }
    throw [InvalidOperationException]::new(
        '另一个奖励入口正在更新 worker 状态；请稍后重试。', $lastError)
}

function Read-WorkerReceipt([string]$Path) {
    try {
        $value = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 |
            ConvertFrom-Json
    } catch {
        throw '钱包 worker 收据无法读取；未触碰任何进程。'
    }
    if ($value.schema_version -ne 1 -or
        $value.component -ne 'battle_reward_wallet_worker' -or
        -not $value.pid -or -not $value.creation_filetime_utc -or
        -not $value.executable_path -or -not $value.command_line -or
        -not $value.worker_script -or -not $value.runtime_root) {
        throw '钱包 worker 收据结构无效；未触碰任何进程。'
    }
    $value
}

function Test-ProcessExited([int]$ProcessId) {
    $process = $null
    try {
        $process = [Diagnostics.Process]::GetProcessById($ProcessId)
    } catch [ArgumentException] {
        return $true
    } catch {
        throw "无法确认 PID $ProcessId 是否仍在运行；未停止任何进程。"
    }
    try {
        return $process.HasExited
    } catch {
        throw "无法确认 PID $ProcessId 是否仍在运行；未停止任何进程。"
    } finally {
        $process.Dispose()
    }
}

function Get-ProcessIdentity([int]$ProcessId) {
    try {
        $process = Get-CimInstance Win32_Process `
            -Filter "ProcessId=$ProcessId" -ErrorAction Stop
    } catch {
        if (Test-ProcessExited $ProcessId) { return $null }
        throw "无法读取 PID $ProcessId 的完整进程身份；未停止任何进程。"
    }
    if ($null -eq $process) { return $null }
    if (-not $process.ExecutablePath -or -not $process.CommandLine -or
        -not $process.CreationDate) {
        if (Test-ProcessExited $ProcessId) { return $null }
        throw "PID $ProcessId 的命令行或创建时间不可读；未停止任何进程。"
    }
    [pscustomobject]@{
        pid = [int]$process.ProcessId
        executable_path = Get-FullPath $process.ExecutablePath
        command_line = [string]$process.CommandLine
        creation_filetime_utc =
            ([datetime]$process.CreationDate).ToUniversalTime().ToFileTimeUtc()
    }
}

function Write-StoppedStatus([string]$RuntimeRoot, [string]$Scope) {
    if ($Scope -notmatch '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$') {
        return
    }
    $processed = '0'
    $replays = '0'
    $status = Join-Path $RuntimeRoot 'wallet-status.ini'
    if (Test-Path -LiteralPath $status -PathType Leaf) {
        foreach ($line in Get-Content -LiteralPath $status -Encoding ASCII) {
            if ($line -match '^Processed=([0-9]+)$') { $processed = $Matches[1] }
            if ($line -match '^Replays=([0-9]+)$') { $replays = $Matches[1] }
        }
    }
    $rows = @(
        '[wallet]',
        'SchemaVersion=1',
        'Healthy=0',
        ('HeartbeatUnix=' + [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()),
        ('ScopeId=' + $Scope),
        'Revision=sdgo-native-gc-ledger-v1',
        'LastError=wallet_worker_stopped',
        ('Processed=' + $processed),
        ('Replays=' + $replays)
    )
    [IO.Directory]::CreateDirectory($RuntimeRoot) | Out-Null
    $temporary = $status + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        [IO.File]::WriteAllText($temporary, ($rows -join "`r`n") + "`r`n",
            [Text.Encoding]::ASCII)
        Move-Item -LiteralPath $temporary -Destination $status -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

$workspaceRoot = Get-FullPath $Workspace
if (-not $Runtime) { $Runtime = Join-Path $workspaceRoot 'runtime' }
$runtimeRoot = Get-FullPath $Runtime
$workerScript = Get-FullPath (Join-Path (Get-FullPath $ToolRoot) 'reward_worker.py')
$receiptPath = Join-Path $workspaceRoot 'battle-reward-wallet-worker.json'
$lockPath = Join-Path $workspaceRoot 'battle-reward-wallet-worker.lock'
if (-not (Test-Path -LiteralPath $workspaceRoot -PathType Container)) {
    Write-Output '奖励工作区不存在；没有进程需要停止。'
    return
}

$lock = Open-OwnerLock $lockPath
try {
    if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
        Write-Output '没有已登记的钱包 worker。'
        return
    }
    $receipt = Read-WorkerReceipt $receiptPath
    if (-not (Test-SamePath $receipt.runtime_root $runtimeRoot) -or
        -not (Test-SamePath $receipt.worker_script $workerScript)) {
        throw '钱包 worker 收据属于另一套路径；未触碰任何进程。'
    }
    $identity = Get-ProcessIdentity ([int]$receipt.pid)
    if ($null -eq $identity) {
        Remove-Item -LiteralPath $receiptPath -Force
        Write-StoppedStatus $runtimeRoot ([string]$receipt.scope_id)
        Write-Output '已清理退出进程留下的钱包 worker 收据。'
        return
    }
    $differences = @()
    if ($identity.pid -ne [int]$receipt.pid) { $differences += 'pid' }
    if (-not (Test-SamePath $identity.executable_path $receipt.executable_path)) {
        $differences += 'executable'
    }
    if ([long]$identity.creation_filetime_utc -ne
        [long]$receipt.creation_filetime_utc) {
        $differences += 'creation_time'
    }
    if (-not [string]::Equals(
            $identity.command_line, [string]$receipt.command_line,
            [StringComparison]::Ordinal)) {
        $differences += 'command_line'
    }
    if ($identity.command_line.IndexOf(
            $workerScript, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        $differences += 'worker_argument'
    }
    if ($identity.command_line.IndexOf(
            $runtimeRoot, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        $differences += 'runtime_argument'
    }
    if ($differences.Count) {
        throw "PID $($receipt.pid) 与已登记钱包 worker 的完整身份不一致（$($differences -join ',')）；未触碰该进程。"
    }

    try {
        Stop-Process -Id $identity.pid -Force
        Wait-Process -Id $identity.pid -ErrorAction SilentlyContinue
    } catch {
        if (-not (Test-ProcessExited $identity.pid)) { throw }
    }
    Remove-Item -LiteralPath $receiptPath -Force
    Write-StoppedStatus $runtimeRoot ([string]$receipt.scope_id)
    Write-Output "已停止钱包 worker，PID $($identity.pid)。"
} finally {
    $lock.Dispose()
}
