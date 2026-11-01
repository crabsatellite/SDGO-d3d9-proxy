param(
    [Parameter(Mandatory = $true)][string]$Workspace,
    [string]$Runtime,
    [Parameter(Mandatory = $true)][string]$StoreServerIni,
    [ValidateRange(0, 65535)][int]$Port = 0,
    [ValidateRange(1, 300)][int]$HeartbeatTimeout = 15,
    [string]$AssistantRoot,
    [string]$AssistantWorkspace,
    [string]$AssistantEntry = 'tools/mod_assistant.py',
    [string]$AssistantFactory = 'make_server',
    [string]$AssistantWorkspaceFactory = 'Workspace',
    [string]$AssistantClient,
    [string]$AssistantSnapshot,
    [string]$AssistantModuleBinary,
    [string]$AssistantServerIni,
    [string[]]$AssistantOption = @(),
    [string]$Python,
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

function Quote-ProcessArgument([string]$Value) {
    '"' + ([regex]::Replace(
        [regex]::Replace($Value, '(\\*)"', '$1$1\"'),
        '(\\+)$', '$1$1')) + '"'
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

function Write-AtomicJson([string]$Path, $Value) {
    $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    try {
        $json = $Value | ConvertTo-Json -Depth 5
        [IO.File]::WriteAllText($temporary, $json + "`n",
            [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
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

function Get-ProcessIdentity([int]$ProcessId) {
    try {
        $process = Get-CimInstance Win32_Process `
            -Filter "ProcessId=$ProcessId" -ErrorAction Stop
    } catch {
        throw "无法读取 PID $ProcessId 的完整进程身份；未启动或停止任何进程。"
    }
    if ($null -eq $process) { return $null }
    if (-not $process.ExecutablePath -or -not $process.CommandLine -or
        -not $process.CreationDate) {
        throw "PID $ProcessId 的命令行或创建时间不可读；未启动或停止任何进程。"
    }
    [pscustomobject]@{
        pid = [int]$process.ProcessId
        executable_path = Get-FullPath $process.ExecutablePath
        command_line = [string]$process.CommandLine
        creation_filetime_utc =
            ([datetime]$process.CreationDate).ToUniversalTime().ToFileTimeUtc()
    }
}

function Assert-OwnedWorker($Receipt, $Identity, [string]$ExpectedRuntime,
        [string]$ExpectedWorker, [string]$ExpectedServerIni) {
    if ($null -eq $Identity) { return $false }
    $differences = @()
    if ($Identity.pid -ne [int]$Receipt.pid) { $differences += 'pid' }
    if (-not (Test-SamePath $Identity.executable_path $Receipt.executable_path)) {
        $differences += 'executable'
    }
    if ([long]$Identity.creation_filetime_utc -ne
        [long]$Receipt.creation_filetime_utc) {
        $differences += 'creation_time'
    }
    if (-not [string]::Equals(
            $Identity.command_line, [string]$Receipt.command_line,
            [StringComparison]::Ordinal)) {
        $differences += 'command_line'
    }
    if (-not (Test-SamePath $Receipt.worker_script $ExpectedWorker)) {
        $differences += 'worker_script'
    }
    if (-not (Test-SamePath $Receipt.runtime_root $ExpectedRuntime)) {
        $differences += 'runtime'
    }
    if (-not (Test-SamePath $Receipt.server_ini $ExpectedServerIni)) {
        $differences += 'server_ini'
    }
    if ($Identity.command_line.IndexOf(
            $ExpectedWorker, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        $differences += 'worker_argument'
    }
    if ($Identity.command_line.IndexOf(
            $ExpectedRuntime, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        $differences += 'runtime_argument'
    }
    if ($differences.Count) {
        throw "PID $($Receipt.pid) 与已登记钱包 worker 的完整身份不一致（$($differences -join ',')）；未触碰该进程。"
    }
    return $true
}

function Read-FreshWalletStatus([string]$RuntimeRoot, [int]$MaximumAge) {
    $path = Join-Path $RuntimeRoot 'wallet-status.ini'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw '钱包 worker 尚未发布状态。'
    }
    $values = @{}
    $opened = $false
    foreach ($raw in Get-Content -LiteralPath $path -Encoding ASCII) {
        $line = $raw.TrimEnd("`r")
        if (-not $line) { continue }
        if ($line.StartsWith('[')) {
            if ($opened -or $line -ne '[wallet]') {
                throw '钱包 worker 状态节无效。'
            }
            $opened = $true
            continue
        }
        $equals = $line.IndexOf('=')
        if (-not $opened -or $equals -le 0) {
            throw '钱包 worker 状态行无效。'
        }
        $name = $line.Substring(0, $equals)
        if ($values.ContainsKey($name)) {
            throw '钱包 worker 状态包含重复字段。'
        }
        $values[$name] = $line.Substring($equals + 1)
    }
    $heartbeat = 0L
    if (-not $opened -or $values.SchemaVersion -ne '1' -or
        $values.Healthy -ne '1' -or
        $values.Revision -ne 'sdgo-native-gc-ledger-v1' -or
        $values.ScopeId -notmatch '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$' -or
        -not [long]::TryParse($values.HeartbeatUnix, [ref]$heartbeat)) {
        throw '钱包 worker 状态不健康。'
    }
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    if ($heartbeat -gt $now + 5 -or $now -gt $heartbeat + $MaximumAge) {
        throw '钱包 worker 状态已过期。'
    }
    [pscustomobject]@{
        scope_id = [string]$values.ScopeId
        heartbeat_unix = $heartbeat
    }
}

$workspaceRoot = Get-FullPath $Workspace
if (-not $Runtime) { $Runtime = Join-Path $workspaceRoot 'runtime' }
$runtimeRoot = Get-FullPath $Runtime
$serverIni = Get-FullPath $StoreServerIni
$toolRootPath = Get-FullPath $ToolRoot
$workerScript = Get-FullPath (Join-Path $toolRootPath 'reward_worker.py')
$assistantScript = Get-FullPath (Join-Path $toolRootPath 'reward_assistant.py')
$stopScript = Get-FullPath (Join-Path $PSScriptRoot 'Stop-Battle-Rewards.ps1')
$codeRoot = Get-FullPath (Split-Path -Parent $PSScriptRoot)

if (-not (Test-Path -LiteralPath $serverIni -PathType Leaf)) {
    throw 'StoreServer.ini 不存在。'
}
foreach ($script in @($workerScript, $assistantScript, $stopScript)) {
    if (-not (Test-Path -LiteralPath $script -PathType Leaf)) {
        throw "缺少奖励入口文件：$script"
    }
}
if (-not $Python) {
    $command = Get-Command python -ErrorAction SilentlyContinue
    if (-not $command) { throw '服主奖励入口需要 Python。' }
    $Python = $command.Source
}
$pythonPath = Get-FullPath $Python
if (-not (Test-Path -LiteralPath $pythonPath -PathType Leaf)) {
    throw 'Python 可执行文件不存在。'
}
if ($AssistantRoot -and -not $AssistantWorkspace) {
    throw '共享助手模式需要 -AssistantWorkspace。'
}
if (-not $AssistantRoot -and
    ($AssistantWorkspace -or $AssistantClient -or $AssistantSnapshot -or
     $AssistantModuleBinary -or $AssistantServerIni -or $AssistantOption.Count)) {
    throw '助手构造参数需要同时提供 -AssistantRoot。'
}

[IO.Directory]::CreateDirectory($workspaceRoot) | Out-Null
[IO.Directory]::CreateDirectory($runtimeRoot) | Out-Null
$receiptPath = Join-Path $workspaceRoot 'battle-reward-wallet-worker.json'
$lockPath = Join-Path $workspaceRoot 'battle-reward-wallet-worker.lock'
$ownsNewWorker = $false
$newProcess = $null
$lock = $null
$setupFailure = $null

try {
    $lock = Open-OwnerLock $lockPath

    # This publishes the canonical native policy before either worker or
    # assistant starts. It performs no SQL operation.
    $initialize = @'
import sys
sys.path.insert(0, sys.argv[1])
from owner.rewards.runtime_control import RuntimeControl
from owner.rewards.workspace import Workspace
Workspace(sys.argv[2], RuntimeControl(sys.argv[2], sys.argv[3], int(sys.argv[4])))
'@
    & $pythonPath -B -X utf8 -c $initialize $codeRoot $workspaceRoot `
        $runtimeRoot $HeartbeatTimeout
    if ($LASTEXITCODE -ne 0) {
        throw '原生奖励策略初始化失败。'
    }

    $workerPid = $null
    if (Test-Path -LiteralPath $receiptPath -PathType Leaf) {
        $receipt = Read-WorkerReceipt $receiptPath
        if (-not (Test-SamePath $receipt.runtime_root $runtimeRoot) -or
            -not (Test-SamePath $receipt.worker_script $workerScript) -or
            -not (Test-SamePath $receipt.server_ini $serverIni)) {
            throw '已有钱包 worker 收据属于另一套路径；未触碰该进程。'
        }
        $identity = Get-ProcessIdentity ([int]$receipt.pid)
        if ($null -eq $identity) {
            Remove-Item -LiteralPath $receiptPath -Force
        } else {
            [void](Assert-OwnedWorker $receipt $identity $runtimeRoot `
                $workerScript $serverIni)
            $status = Read-FreshWalletStatus $runtimeRoot $HeartbeatTimeout
            if ($receipt.scope_id -and
                -not [string]::Equals($receipt.scope_id, $status.scope_id,
                    [StringComparison]::OrdinalIgnoreCase)) {
                throw '钱包 worker 的数据库 scope 与收据不一致；未启动重复实例。'
            }
            $workerPid = [int]$receipt.pid
            Write-Host "复用已登记的钱包 worker，PID $workerPid。"
        }
    }

    if ($null -eq $workerPid) {
        $workerArguments = @(
            '-B', '-X', 'utf8', $workerScript,
            '--server-ini', $serverIni,
            '--runtime', $runtimeRoot
        )
        $workerCommandLine = ($workerArguments |
            ForEach-Object { Quote-ProcessArgument ([string]$_) }) -join ' '
        $newProcess = Start-Process -FilePath $pythonPath `
            -ArgumentList $workerCommandLine -WorkingDirectory $codeRoot `
            -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $workspaceRoot 'wallet-worker.stdout.log') `
            -RedirectStandardError (Join-Path $workspaceRoot 'wallet-worker.stderr.log')
        $ownsNewWorker = $true
        $identity = $null
        for ($attempt = 0; $attempt -lt 50 -and $null -eq $identity; $attempt++) {
            if ($newProcess.HasExited) {
                throw '钱包 worker 启动失败；请查看 wallet-worker.stderr.log。'
            }
            $identity = Get-ProcessIdentity $newProcess.Id
            if ($null -eq $identity) { Start-Sleep -Milliseconds 100 }
        }
        if ($null -eq $identity) { throw '无法读取新钱包 worker 的进程身份。' }
        $receipt = [ordered]@{
            schema_version = 1
            component = 'battle_reward_wallet_worker'
            pid = $identity.pid
            creation_filetime_utc = $identity.creation_filetime_utc
            executable_path = $identity.executable_path
            command_line = $identity.command_line
            worker_script = $workerScript
            runtime_root = $runtimeRoot
            server_ini = $serverIni
            scope_id = ''
        }
        Write-AtomicJson $receiptPath $receipt

        $status = $null
        $lastStatusError = '钱包 worker 尚未发布状态。'
        for ($attempt = 0; $attempt -lt 150 -and $null -eq $status; $attempt++) {
            if ($newProcess.HasExited) {
                throw '钱包 worker 在发布健康状态前退出；请查看 wallet-worker.stderr.log。'
            }
            try {
                $status = Read-FreshWalletStatus $runtimeRoot $HeartbeatTimeout
            } catch {
                $lastStatusError = $_.Exception.Message
                Start-Sleep -Milliseconds 100
            }
        }
        if ($null -eq $status) { throw $lastStatusError }
        $receipt.scope_id = $status.scope_id
        Write-AtomicJson $receiptPath $receipt
        $workerPid = $identity.pid
        Write-Host "钱包 worker 已启动，PID $workerPid。"
    }
} catch {
    $setupFailure = $_
} finally {
    if ($null -ne $lock) {
        $lock.Dispose()
        $lock = $null
    }
}

if ($null -ne $setupFailure) {
    if ($ownsNewWorker) {
        if (Test-Path -LiteralPath $receiptPath -PathType Leaf) {
            & $stopScript -Workspace $workspaceRoot -Runtime $runtimeRoot `
                -ToolRoot $toolRootPath
        } elseif ($null -ne $newProcess -and -not $newProcess.HasExited) {
            Stop-Process -Id $newProcess.Id -Force
            Wait-Process -Id $newProcess.Id -ErrorAction SilentlyContinue
        }
    }
    throw $setupFailure
}

$assistantArguments = @(
    '-B', '-X', 'utf8', $assistantScript,
    '--workspace', $workspaceRoot,
    '--runtime', $runtimeRoot,
    '--heartbeat-timeout', [string]$HeartbeatTimeout,
    '--port', [string]$Port
)
if ($AssistantRoot) {
    $assistantArguments += @(
        '--assistant-root', (Get-FullPath $AssistantRoot),
        '--assistant-entry', $AssistantEntry,
        '--assistant-factory', $AssistantFactory,
        '--assistant-workspace-factory', $AssistantWorkspaceFactory,
        '--assistant-workspace', (Get-FullPath $AssistantWorkspace)
    )
    foreach ($pair in @(
        @('--assistant-client', $AssistantClient),
        @('--assistant-snapshot', $AssistantSnapshot),
        @('--assistant-module-binary', $AssistantModuleBinary),
        @('--assistant-server-ini', $AssistantServerIni)
    )) {
        if ($pair[1]) {
            $assistantArguments += @($pair[0], (Get-FullPath $pair[1]))
        }
    }
    foreach ($option in $AssistantOption) {
        $assistantArguments += @('--assistant-option', $option)
    }
    Write-Host '将启动新的共享助手实例；本入口不会停止已占用端口的旧助手。'
}

try {
    & $pythonPath @assistantArguments
    if ($LASTEXITCODE -ne 0) {
        throw "奖励助手退出，代码 $LASTEXITCODE。"
    }
} finally {
    if ($ownsNewWorker) {
        if (Test-Path -LiteralPath $receiptPath -PathType Leaf) {
            & $stopScript -Workspace $workspaceRoot -Runtime $runtimeRoot `
                -ToolRoot $toolRootPath
        } elseif ($null -ne $newProcess -and -not $newProcess.HasExited) {
            # This fallback applies only to the exact Process object created by
            # this invocation before a durable receipt could be written.
            Stop-Process -Id $newProcess.Id -Force
            Wait-Process -Id $newProcess.Id -ErrorAction SilentlyContinue
        }
    }
}
