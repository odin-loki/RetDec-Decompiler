# scripts/parity_bench.ps1
# Compare retdec-decompiler CLI wall time vs GUI subprocess wall time.
#
# Usage (from repo root):
#   .\scripts\parity_bench.ps1
#   .\scripts\parity_bench.ps1 -Binary _profile_run\target.exe -Fast

param(
    [string]$Binary = "_profile_run\target.exe",
    [switch]$Fast,
    [string]$InstallDir = "install\windows\bin"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $RepoRoot

$Dec = Join-Path $InstallDir "retdec-decompiler.exe"
$Gui = Join-Path $InstallDir "retdec-gui.exe"
$FastJson = (Resolve-Path "src\gui\resources\llvm_passes_fast.json").Path

if (-not (Test-Path $Dec)) { throw "Missing $Dec - build and install first." }
if (-not (Test-Path $Gui)) { throw "Missing $Gui - build and install first." }
if (-not (Test-Path $Binary)) { throw "Missing benchmark binary: $Binary" }

$absBinary = (Resolve-Path $Binary).Path
$base = [System.IO.Path]::ChangeExtension($absBinary, $null)
$outCli = "${base}_parity_cli.c"
$outGui = "${base}.gui-decompiled.c"

Remove-Item $outCli, $outGui, "${base}.gui-decompiled.config.json" -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "===== GUI vs CLI PARITY BENCHMARK ====="
Write-Host "Binary: $absBinary ($([math]::Round((Get-Item $absBinary).Length/1MB, 2)) MB)"
Write-Host "Fast preset: $Fast"
Write-Host ""

Write-Host "(1) CLI subprocess (stdout/stderr -> null):"
$cliArgs = @($absBinary, "-o", $outCli, "-f", "plain", "-s")
if ($Fast) {
    $cliArgs += @("--backend-no-opts", "--disable-static-code-detection",
                   "--llvm-passes-json", $FastJson)
}
$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $Dec @cliArgs 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "CLI decompile failed (exit $LASTEXITCODE)" }
$sw.Stop()
$cliSec = $sw.Elapsed.TotalSeconds
Write-Host ("  {0:F1} s" -f $cliSec)
Write-Host ""

Write-Host "(2) GUI subprocess (retdec-gui --headless-decompile, file redirect):"
$env:RETDEC_GUI_HEADLESS = "1"
$env:QT_QPA_PLATFORM = "offscreen"
$guiArgs = @("--headless", "--headless-decompile", $absBinary)
if ($Fast) { $guiArgs = @("--headless", "--headless-decompile", "--fast-decompile", $absBinary) }

$timingFile = Join-Path $env:TEMP "retdec-gui-decompile-timing.txt"
Remove-Item $timingFile -Force -ErrorAction SilentlyContinue

$sw = [System.Diagnostics.Stopwatch]::StartNew()
& $Gui @guiArgs | Out-Null
$sw.Stop()
$guiWallSec = $sw.Elapsed.TotalSeconds

$guiSubMs = 0
$guiOutputPath = $outGui
if (Test-Path $timingFile) {
    foreach ($line in Get-Content $timingFile) {
        if ($line -match '^DECOMPILE_MS=(\d+)') { $guiSubMs = [int64]$Matches[1] }
        if ($line -match '^OUTPUT=(.+)$') { $guiOutputPath = $Matches[1].Trim() }
    }
}
$guiSubSec = if ($guiSubMs -gt 0) { $guiSubMs / 1000.0 } else { $guiWallSec }

if (-not (Test-Path $guiOutputPath)) { throw "GUI decompile did not produce $guiOutputPath" }
Write-Host ("  subprocess: {0:F1} s (wall including Qt: {1:F1} s)" -f $guiSubSec, $guiWallSec)
Write-Host ""

$h1 = (Get-FileHash $outCli -Algorithm SHA256).Hash
$h2 = (Get-FileHash $guiOutputPath -Algorithm SHA256).Hash
Write-Host "(3) Output parity:"
Write-Host "  CLI .c: $($h1.Substring(0,16))... ($((Get-Item $outCli).Length) bytes)"
Write-Host "  GUI .c: $($h2.Substring(0,16))... ($((Get-Item $guiOutputPath).Length) bytes)"
Write-Host ("  byte-identical: {0}" -f ($h1 -eq $h2))
Write-Host ""

$delta = [math]::Abs($cliSec - $guiSubSec)
$pct = if ($cliSec -gt 0) { ($delta / $cliSec) * 100.0 } else { 0 }
Write-Host "(4) Timing delta:"
Write-Host ("  |CLI - GUI subprocess| = {0:F1} s ({1:F0}%)" -f $delta, $pct)
if ($pct -le 5) {
    Write-Host "  PASS - GUI subprocess within 5% of CLI"
    exit 0
} else {
    Write-Host "  FAIL - GUI subprocess differs from CLI by more than 5%"
    exit 1
}
