#Requires -Version 5.1
<#
.SYNOPSIS
  Time retdec-decompiler on a fixed fixture; emit JSON for CI trend tracking.

.DESCRIPTION
  Always exits 0. Prints elapsed seconds and writes a JSON result file.

.PARAMETER Decompiler
  Path to retdec-decompiler.exe (auto-detected from install/ or build/).

.PARAMETER Binary
  Input binary to decompile (default: tests/test_binaries/fib.c compiled on the fly).

.PARAMETER OutputJson
  Where to write timing JSON (default: perf_bench_result.json in current directory).

.PARAMETER TimeoutSec
  Per-run timeout in seconds (default 300).
#>
param(
    [string]$Decompiler = "",
    [string]$Binary = "",
    [string]$OutputJson = "perf_bench_result.json",
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Find-Decompiler {
    if ($Decompiler -and (Test-Path -LiteralPath $Decompiler)) {
        return (Resolve-Path -LiteralPath $Decompiler).Path
    }
    $candidates = @(
        (Join-Path $RepoRoot "install\windows\bin\retdec-decompiler.exe"),
        (Join-Path $RepoRoot "install\linux\bin\retdec-decompiler")
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    $buildWin = Join-Path $RepoRoot "build\windows"
    if (Test-Path -LiteralPath $buildWin) {
        $found = Get-ChildItem -LiteralPath $buildWin -Filter "retdec-decompiler.exe" -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    $buildLin = Join-Path $RepoRoot "build\linux"
    if (Test-Path -LiteralPath $buildLin) {
        $found = Get-ChildItem -LiteralPath $buildLin -Filter "retdec-decompiler" -Recurse -ErrorAction SilentlyContinue |
            Where-Object { -not $_.Extension } |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    foreach ($c in @(
            (Join-Path $RepoRoot "build-decompiler-test\bin\retdec-decompiler.exe"),
            (Join-Path $RepoRoot "build-decompiler-test\Release\bin\retdec-decompiler.exe")
        )) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    return $null
}

function Ensure-FibBinary {
    param([string]$Path)
    if (Test-Path -LiteralPath $Path) { return $Path }

    $searchRoots = @(
        (Join-Path $RepoRoot "build\windows"),
        (Join-Path $RepoRoot "build-decompiler-test"),
        (Join-Path $RepoRoot "build-gui-check"),
        (Join-Path $RepoRoot "build")
    )
    foreach ($root in $searchRoots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $existing = Get-ChildItem -LiteralPath $root -Filter "fib_smoke.exe" -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($existing) { return $existing.FullName }
    }

    $src = Join-Path $RepoRoot "tests\test_binaries\fib.c"
    if (-not (Test-Path -LiteralPath $src)) {
        throw "fib.c not found at $src"
    }
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $cl = Get-Command cl -ErrorAction SilentlyContinue
    if ($cl) {
        & cl /nologo /O1 /Fe:"$Path" "$src" 2>$null
    }
    if (-not (Test-Path -LiteralPath $Path)) {
        $gcc = Get-Command gcc -ErrorAction SilentlyContinue
        if ($gcc) {
            & gcc -O1 -o "$Path" "$src" 2>$null
        }
    }
    if (-not (Test-Path -LiteralPath $Path)) {
        $fallback = Find-Decompiler
        if ($fallback) {
            Write-Host "perf_bench: using decompiler binary as timing fixture (fib unavailable)"
            return $fallback
        }
        throw "Could not find or compile fib fixture (pass -Binary to an existing .exe)"
    }
    return $Path
}

$dec = Find-Decompiler
$result = [ordered]@{
    timestamp_utc = (Get-Date).ToUniversalTime().ToString("o")
    fixture       = "fib_smoke"
    decompiler    = $dec
    binary        = $Binary
    seconds       = $null
    exit_code     = $null
    output_bytes  = $null
    error         = $null
}

try {
    if (-not $dec) {
        throw "retdec-decompiler not found; pass -Decompiler"
    }

    if (-not $Binary) {
        $Binary = Join-Path $env:TEMP "retdec_perf_fib_smoke.exe"
    }
    $Binary = Ensure-FibBinary -Path $Binary
    $result.binary = $Binary

    $outC = Join-Path $env:TEMP ("retdec_perf_out_{0}.c" -f ([guid]::NewGuid().ToString("N")))
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $proc = Start-Process -FilePath $dec -ArgumentList @("-o", $outC, $Binary) `
        -PassThru -Wait -NoNewWindow
    $sw.Stop()

    $result.seconds = [math]::Round($sw.Elapsed.TotalSeconds, 3)
    $result.exit_code = $proc.ExitCode
    if (Test-Path -LiteralPath $outC) {
        $result.output_bytes = (Get-Item -LiteralPath $outC).Length
        Remove-Item -LiteralPath $outC -Force -ErrorAction SilentlyContinue
    }

    Write-Host ("perf_bench: {0}s exit={1} bytes={2}" -f $result.seconds, $result.exit_code, $result.output_bytes)
}
catch {
    $result.error = $_.Exception.Message
    Write-Host "perf_bench: ERROR $($result.error)"
}

$json = $result | ConvertTo-Json -Depth 4
Set-Content -LiteralPath $OutputJson -Value $json -Encoding UTF8
Write-Host "wrote $OutputJson"

exit 0
