#Requires -Version 5.1
<#
.SYNOPSIS
  Compile tiny C corpus sources into native regression binaries.

.DESCRIPTION
  Reads tests/decompiler/corpus/sources/*.c and writes executables to
  tests/decompiler/corpus/bin/. Also generates hello.pyc when Python is available.

.PARAMETER OutDir
  Output directory (default: tests/decompiler/corpus/bin under repo root).
#>
param(
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
if (-not $OutDir) {
    $OutDir = Join-Path $RepoRoot "tests\decompiler\corpus\bin"
}
$SrcDir = Join-Path $RepoRoot "tests\decompiler\corpus\sources"
$FixDir = Join-Path $RepoRoot "tests\decompiler\corpus\fixtures"

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
New-Item -ItemType Directory -Path $FixDir -Force | Out-Null

function Compile-CSource {
    param([string]$Src, [string]$Dest)
    if (Test-Path -LiteralPath $Dest) { return $true }

    $cl = Get-Command cl -ErrorAction SilentlyContinue
    if ($cl) {
        & cl /nologo /O1 /Fe:"$Dest" "$Src" 2>$null
        if (Test-Path -LiteralPath $Dest) { return $true }
    }
    $gcc = Get-Command gcc -ErrorAction SilentlyContinue
    if ($gcc) {
        & gcc -O1 -o "$Dest" "$Src" 2>$null
        if (Test-Path -LiteralPath $Dest) { return $true }
    }
    return $false
}

$built = 0
foreach ($name in @("hello", "vector_sort")) {
    $src = Join-Path $SrcDir "$name.c"
    if (-not (Test-Path -LiteralPath $src)) {
        Write-Warning "missing source: $src"
        continue
    }
    $dest = Join-Path $OutDir "$name.exe"
    if (Compile-CSource -Src $src -Dest $dest) {
        Write-Host "built $dest"
        $built++
    } else {
        Write-Warning "failed to compile $name.c"
    }
}

# fib_smoke from shared test binary source when present
$fibSrc = Join-Path $RepoRoot "tests\test_binaries\fib.c"
$fibDest = Join-Path $OutDir "fib_smoke.exe"
if ((Test-Path -LiteralPath $fibSrc) -and -not (Test-Path -LiteralPath $fibDest)) {
    if (Compile-CSource -Src $fibSrc -Dest $fibDest) {
        Write-Host "built $fibDest"
        $built++
    }
}

# minimal WASM header (idempotent)
$wasmDest = Join-Path $FixDir "minimal.wasm"
if (-not (Test-Path -LiteralPath $wasmDest)) {
    [System.IO.File]::WriteAllBytes($wasmDest, [byte[]](0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00))
    Write-Host "wrote $wasmDest"
}

# hello.pyc via Python when available
$pycDest = Join-Path $FixDir "hello.pyc"
$pySrc = Join-Path $SrcDir "hello.py"
if ((Test-Path -LiteralPath $pySrc) -and -not (Test-Path -LiteralPath $pycDest)) {
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command python3 -ErrorAction SilentlyContinue }
    if ($py) {
        & $py -c "import py_compile; py_compile.compile(r'$pySrc', cfile=r'$pycDest', doraise=True)"
        if (Test-Path -LiteralPath $pycDest) {
            Write-Host "built $pycDest"
        }
    } else {
        Write-Warning "Python not found; skipped hello.pyc (see corpus/README.md)"
    }
}

Write-Host "corpus fixtures: $built native binary(ies) in $OutDir"
exit 0
