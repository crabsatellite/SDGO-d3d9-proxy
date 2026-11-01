$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$hashList = Join-Path $root "SHA256SUMS.txt"
if (-not (Test-Path -LiteralPath $hashList)) { throw "missing SHA256SUMS.txt" }

$checked = 0
foreach ($line in [IO.File]::ReadAllLines($hashList)) {
    if (-not $line.Trim()) { continue }
    $match = [regex]::Match($line, '^([0-9A-Fa-f]{64})\s+\*(.+)$')
    if (-not $match.Success) { throw "bad SHA256SUMS line: $line" }
    $relative = $match.Groups[2].Value.Replace('/', '\')
    $full = [IO.Path]::GetFullPath((Join-Path $root $relative))
    $prefix = $root.TrimEnd('\') + '\'
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "path escapes package root: $relative"
    }
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { throw "missing $relative" }
    $actual = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
    if ($actual -ine $match.Groups[1].Value) { throw "hash mismatch: $relative" }
    ++$checked
}

$manifest = Get-Content -LiteralPath (Join-Path $root "manifest.json") -Raw | ConvertFrom-Json
if ($manifest.mod.id -ne "capsule_currency_fix") { throw "unexpected mod id" }
if ($manifest.hook.ownedSymbol -ne "capsule.activity.load_items") { throw "unexpected hook symbol" }
if ($manifest.hook.conflictsWithCapsuleCatalog) { throw "manifest reports a hook conflict" }
if (Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object FullName -Match 'capsule_catalog') {
    throw "capsule_catalog file found in standalone package"
}

Write-Host "package verification passed: $checked files, standalone currency fix, no catalog payload"
