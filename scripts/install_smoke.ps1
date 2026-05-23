# scripts/install_smoke.ps1
# Smoke-test a RetDec cmake --install tree (Windows).
#
# Usage:
#   .\scripts\install_smoke.ps1
#   .\scripts\install_smoke.ps1 -InstallDir install\windows
#   .\scripts\install_smoke.ps1 -Binary D:\fib.exe

#requires -Version 5.1

param(
    [string]$InstallDir = "",
    [string]$Binary = ""
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Resolve-InstallRoot([string]$Dir) {
    if ([string]::IsNullOrWhiteSpace($Dir)) {
        return Join-Path $RepoRoot "install\windows"
    }
    if ([System.IO.Path]::IsPathRooted($Dir)) {
        return $Dir
    }
    return (Join-Path $RepoRoot $Dir)
}

function Get-SmokeBinary([string]$Explicit) {
    if (-not [string]::IsNullOrWhiteSpace($Explicit) -and (Test-Path -LiteralPath $Explicit)) {
        return (Resolve-Path -LiteralPath $Explicit).Path
    }

    $fibFixture = Join-Path $RepoRoot "tests\decompiler\fixtures\fib.exe"
    if (Test-Path -LiteralPath $fibFixture) {
        return (Resolve-Path -LiteralPath $fibFixture).Path
    }

    $decompilerTests = Join-Path $RepoRoot "tests\decompiler"
    if (Test-Path -LiteralPath $decompilerTests) {
        $candidates = @(Get-ChildItem -LiteralPath $decompilerTests -Filter "*.exe" -Recurse -File -ErrorAction SilentlyContinue)
        if ($candidates.Count -gt 0) {
            return ($candidates | Sort-Object Length | Select-Object -First 1).FullName
        }
    }

    return $null
}

$installRoot = Resolve-InstallRoot $InstallDir

if (-not (Test-Path -LiteralPath $installRoot)) {
    Write-Host "SKIP: install directory not present: $installRoot"
    exit 0
}

$binDir = Join-Path $installRoot "bin"
$decompiler = Join-Path $binDir "retdec-decompiler.exe"
$gui = Join-Path $binDir "retdec-gui.exe"
$fileinfo = Join-Path $binDir "retdec-fileinfo.exe"

foreach ($tool in @(
    @{ Path = $decompiler; Name = "retdec-decompiler.exe" },
    @{ Path = $gui;         Name = "retdec-gui.exe" },
    @{ Path = $fileinfo;    Name = "retdec-fileinfo.exe" }
)) {
    if (-not (Test-Path -LiteralPath $tool.Path)) {
        throw "Missing $($tool.Name) in $binDir"
    }
}

Write-Host "install_smoke: install=$installRoot bin=$binDir"

Push-Location $binDir
try {
    Write-Host "==> retdec-decompiler --help"
    & $decompiler --help 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "retdec-decompiler --help failed (exit $LASTEXITCODE)"
    }

    Write-Host "==> retdec-gui --help (or --version)"
    & $gui --help 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        & $gui --version 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "retdec-gui --help and --version failed"
        }
    }

    $sample = Get-SmokeBinary $Binary
    if (-not $sample) {
        throw "No decompile fixture found (expected tests/decompiler/fixtures/fib.exe or other .exe under tests/decompiler)"
    }

    Write-Host "==> retdec-gui --headless --headless-decompile $sample"
    $env:RETDEC_GUI_HEADLESS = "1"
    $env:QT_QPA_PLATFORM = "offscreen"

    $workDir = Join-Path $env:TEMP "retdec-install-smoke"
    New-Item -ItemType Directory -Force -Path $workDir | Out-Null
    $workBinary = Join-Path $workDir (Split-Path -Leaf $sample)
    Copy-Item -LiteralPath $sample -Destination $workBinary -Force
    $absBinary = (Resolve-Path -LiteralPath $workBinary).Path

    $base = [System.IO.Path]::ChangeExtension($absBinary, $null)
    Remove-Item "${base}.gui-decompiled.c", "${base}.gui-decompiled.config.json" -Force -ErrorAction SilentlyContinue

    & $gui --headless --headless-decompile $absBinary 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "GUI headless decompile failed (exit $LASTEXITCODE)"
    }

    $guiOut = "${base}.gui-decompiled.c"
    if (-not (Test-Path -LiteralPath $guiOut)) {
        throw "GUI decompile output missing: $guiOut"
    }
    $outSize = (Get-Item -LiteralPath $guiOut).Length
    if ($outSize -eq 0) {
        throw "GUI decompile output empty: $guiOut"
    }
    Write-Host "OK: $guiOut ($outSize bytes)"
}
finally {
    Pop-Location
}

Write-Host "PASS: install_smoke"
exit 0
