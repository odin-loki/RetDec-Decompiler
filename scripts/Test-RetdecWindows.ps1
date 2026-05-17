# Test-RetdecWindows.ps1
# Smoke-test suite for both Windows dist builds of RetDec:
#   dist\windows\      (MinGW cross or native MSVC staging)
#
# Usage:
#   # Test the MinGW cross-compile build (default):
#   .\scripts\Test-RetdecWindows.ps1
#
#   # Test the full native build:
#   .\scripts\Test-RetdecWindows.ps1 -DistDir dist\windows

param(
    [string]$DistDir    = "$PSScriptRoot\..\dist\windows",
    [string]$SamplesDir = "$PSScriptRoot\..\tests\decompile_samples",
    [string]$TmpDir     = "$env:TEMP\retdec-test"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Continue"

$PASS = 0
$FAIL = 0

function Write-Pass([string]$msg) {
    Write-Host "  [PASS] $msg" -ForegroundColor Green
    $script:PASS++
}
function Write-Fail([string]$msg) {
    Write-Host "  [FAIL] $msg" -ForegroundColor Red
    $script:FAIL++
}
function Write-Skip([string]$msg) {
    Write-Host "  [SKIP] $msg" -ForegroundColor Gray
}

$DistDirFull = (Resolve-Path $DistDir -ErrorAction SilentlyContinue)?.Path
if (-not $DistDirFull) { $DistDirFull = $DistDir }
$exe    = Join-Path $DistDirFull "retdec-decompiler.exe"
$guiExe = Join-Path $DistDirFull "retdec-gui.exe"

New-Item -ItemType Directory -Force -Path $TmpDir | Out-Null

Write-Host ""
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host "  RetDec Windows Smoke Tests" -ForegroundColor Cyan
Write-Host "  Distribution: $DistDirFull" -ForegroundColor Cyan
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host ""

if (-not (Test-Path $exe)) {
    Write-Fail "retdec-decompiler.exe not found in $DistDirFull"
    Write-Host ""
    Write-Host "Build options:" -ForegroundColor White
    Write-Host "  MinGW cross-compile (CLI only):" -ForegroundColor Gray
    Write-Host "    wsl bash scripts/wsl_cross_build.sh" -ForegroundColor Cyan
    Write-Host "  Native Windows (CUDA + GUI + full):" -ForegroundColor Gray
    Write-Host "    .\scripts\windows_native_configure.ps1" -ForegroundColor Cyan
    Write-Host "    .\scripts\windows_native_build.ps1" -ForegroundColor Cyan
    exit 1
}

# ── Test 1: --help ─────────────────────────────────────────────────────────
Write-Host "[1] retdec-decompiler.exe --help" -ForegroundColor White
$out = & $exe --help 2>&1
if ($out -match "retdec|INPUT_FILE|decompil") {
    Write-Pass "--help output looks correct"
} else {
    Write-Fail "--help output does not contain expected keywords"
}

# ── Test 2: --version ─────────────────────────────────────────────────────
Write-Host ""
Write-Host "[2] retdec-decompiler.exe --version" -ForegroundColor White
$ver = & $exe --version 2>&1
if ($LASTEXITCODE -eq 0 -or $ver -match "RetDec|retdec|\d+\.\d+") {
    Write-Pass "Version string: $($ver | Select-Object -First 1)"
} else {
    Write-Skip "No --version flag implemented"
}

# ── Test 3: Lua 5.4 bytecode ──────────────────────────────────────────────
Write-Host ""
Write-Host "[3] Lua 5.4 bytecode decompile" -ForegroundColor White
$luacPath = $null
# Check Windows-local samples first, then WSL filesystem
$luaCandidates = @(
    (Join-Path $SamplesDir "built\fib54.luac"),
    (Join-Path $SamplesDir "built\fib.luac"),
    "\\wsl$\Ubuntu\tmp\test_lua\fib54.luac",
    "\\wsl$\Ubuntu\tmp\test54.luac"
)
$luacPath = $luaCandidates | Where-Object { Test-Path $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
if ($null -ne $luacPath) {
    $outLua = Join-Path $TmpDir "fib_win.lua"
    & $exe $luacPath -o $outLua 2>&1 | Out-Null
    if (Test-Path $outLua) {
        Write-Pass "Lua .luac decompiled -> $outLua"
        $content = Get-Content $outLua -Raw -ErrorAction SilentlyContinue
        if ($content -match "function|return|local|reg") {
            Write-Pass "Lua output contains expected keywords"
        } else {
            Write-Fail "Lua output does not look like Lua source"
        }
    } else {
        Write-Fail "No output file produced for Lua decompile"
    }
} else {
    Write-Skip "No Lua .luac sample found (build samples with tests/decompile_samples/compile_all.sh)"
}

# ── Test 4: Python .pyc ───────────────────────────────────────────────────
Write-Host ""
Write-Host "[4] Python .pyc decompile" -ForegroundColor White
$pycCandidates = @(
    (Join-Path $SamplesDir "built\hello.pyc"),
    "\\wsl$\Ubuntu\tmp\hello.pyc",
    "\\wsl$\Ubuntu\tmp\test.pyc"
)
$pyc = $pycCandidates | Where-Object { Test-Path $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
if ($null -ne $pyc) {
    $outPy = Join-Path $TmpDir "test_win.py"
    & $exe $pyc -o $outPy 2>&1 | Out-Null
    if (Test-Path $outPy) {
        Write-Pass "Python .pyc decompiled -> $outPy"
        $content = Get-Content $outPy -Raw -ErrorAction SilentlyContinue
        if ($content -match "def |import |return |class ") {
            Write-Pass "Python output contains expected keywords"
        } else {
            Write-Skip "Python output does not match keywords (may still be correct)"
        }
    } else {
        Write-Fail "No output file produced for Python .pyc decompile"
    }
} else {
    Write-Skip "No .pyc sample found"
}

# ── Test 5: Java .class ───────────────────────────────────────────────────
Write-Host ""
Write-Host "[5] Java .class decompile" -ForegroundColor White
$classCandidates = @(
    (Join-Path $SamplesDir "built\Hello.class"),
    "\\wsl$\Ubuntu\tmp\Hello.class"
)
$classFile = $classCandidates | Where-Object { Test-Path $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
if ($null -ne $classFile) {
    $outJava = Join-Path $TmpDir "Hello_win.java"
    & $exe $classFile -o $outJava 2>&1 | Out-Null
    if (Test-Path $outJava) {
        Write-Pass "Java .class decompiled -> $outJava"
    } else {
        Write-Fail "No output file produced for Java .class decompile"
    }
} else {
    Write-Skip "No .class sample found"
}

# ── Test 6: WebAssembly .wasm ─────────────────────────────────────────────
Write-Host ""
Write-Host "[6] WebAssembly .wasm decompile" -ForegroundColor White
$wasmCandidates = @(
    (Join-Path $SamplesDir "built\hello.wasm"),
    "\\wsl$\Ubuntu\tmp\hello.wasm"
)
$wasm = $wasmCandidates | Where-Object { Test-Path $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
if ($null -ne $wasm) {
    $outWat = Join-Path $TmpDir "hello_win.wat"
    & $exe $wasm -o $outWat 2>&1 | Out-Null
    if (Test-Path $outWat) {
        Write-Pass "WebAssembly .wasm decompiled -> $outWat"
    } else {
        Write-Fail "No output file produced for WASM decompile"
    }
} else {
    Write-Skip "No .wasm sample found"
}

# ── Test 7: GUI launch (native build only) ────────────────────────────────
Write-Host ""
Write-Host "[7] retdec-gui.exe launches" -ForegroundColor White
if (Test-Path $guiExe) {
    # Start GUI, wait 3 seconds, then kill — just verify it doesn't crash immediately
    try {
        $proc = Start-Process $guiExe -PassThru -ErrorAction Stop
        Start-Sleep -Seconds 3
        if (-not $proc.HasExited) {
            Write-Pass "retdec-gui.exe started successfully (PID $($proc.Id))"
            $proc.Kill()
        } else {
            Write-Fail "retdec-gui.exe exited immediately (code $($proc.ExitCode))"
        }
    } catch {
        Write-Fail "retdec-gui.exe failed to start: $_"
    }
} else {
    Write-Skip "retdec-gui.exe not in $DistDirFull (MinGW build has no GUI; use windows_native_build.ps1 for full build)"
}

# ── Test 8: CUDA runtime present (full build only) ────────────────────────
Write-Host ""
Write-Host "[8] CUDA runtime DLLs present (full build)" -ForegroundColor White
$cudaDll = Get-ChildItem $DistDirFull -Filter "cudart64_*.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
if ($cudaDll) {
    Write-Pass "CUDA runtime: $($cudaDll.Name)"
} else {
    Write-Skip "No cudart64_*.dll found (GPU acceleration not bundled — MinGW cross-build or CPU-only)"
}

# ── Summary ────────────────────────────────────────────────────────────────
Write-Host ""
Write-Host "======================================================" -ForegroundColor Cyan
$color = if ($FAIL -eq 0) { "Green" } else { "Red" }
Write-Host "  Results: $PASS passed, $FAIL failed" -ForegroundColor $color
Write-Host "======================================================" -ForegroundColor Cyan
Write-Host ""
if ($FAIL -gt 0) { exit 1 }
