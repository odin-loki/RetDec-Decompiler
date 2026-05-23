# scripts/parity_ctest.ps1
# CTest-friendly CLI vs GUI headless decompile parity (output hash; timing optional).
#
# Usage:
#   .\scripts\parity_ctest.ps1 -Decompiler D:\bin\retdec-decompiler.exe `
#       -Gui D:\bin\retdec-gui.exe -Binary D:\fib.exe -WorkDir D:\work

param(
    [Parameter(Mandatory = $true)][string]$Decompiler,
    [Parameter(Mandatory = $true)][string]$Gui,
    [Parameter(Mandatory = $true)][string]$Binary,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [switch]$Fast,
    [switch]$CheckTiming
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Decompiler)) { throw "Missing decompiler: $Decompiler" }
if (-not (Test-Path -LiteralPath $Gui)) { throw "Missing GUI: $Gui" }
if (-not (Test-Path -LiteralPath $Binary)) { throw "Missing binary: $Binary" }

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$FastJson = Join-Path $RepoRoot "src\gui\resources\llvm_passes_fast.json"

$binName = Split-Path -Leaf $Binary
$workBinary = Join-Path $WorkDir $binName
Copy-Item -LiteralPath $Binary -Destination $workBinary -Force

$absBinary = (Resolve-Path -LiteralPath $workBinary).Path
$base = [System.IO.Path]::ChangeExtension($absBinary, $null)

$outCli = Join-Path $WorkDir "parity_cli.c"
$outGui = "${base}.gui-decompiled.c"
$timingFile = Join-Path $WorkDir "retdec-gui-decompile-timing.txt"

Remove-Item $outCli, $outGui, "${base}.gui-decompiled.config.json", $timingFile -Force -ErrorAction SilentlyContinue

Write-Host "parity_ctest: binary=$absBinary workdir=$WorkDir fast=$Fast"

# (1) CLI
$cliArgs = @($absBinary, "-o", $outCli, "-f", "plain", "-s")
if ($Fast) {
    if (-not (Test-Path -LiteralPath $FastJson)) { throw "Missing fast preset: $FastJson" }
    $cliArgs += @("--backend-no-opts", "--disable-static-code-detection",
                   "--llvm-passes-json", $FastJson)
}
$swCli = [System.Diagnostics.Stopwatch]::StartNew()
& $Decompiler @cliArgs 2>&1 | Out-Null
$swCli.Stop()
if ($LASTEXITCODE -ne 0) { throw "CLI decompile failed (exit $LASTEXITCODE)" }
if (-not (Test-Path -LiteralPath $outCli)) { throw "CLI output missing: $outCli" }

# (2) GUI headless (run from GUI directory so retdec-decompiler is found beside retdec-gui)
$env:RETDEC_GUI_HEADLESS = "1"
$env:QT_QPA_PLATFORM = "offscreen"
$guiArgs = @("--headless", "--headless-decompile", $absBinary)
if ($Fast) { $guiArgs = @("--headless", "--headless-decompile", "--fast-decompile", $absBinary) }

$swGui = [System.Diagnostics.Stopwatch]::StartNew()
Push-Location (Split-Path -Parent $Gui)
try {
    & $Gui @guiArgs 2>&1 | Out-Null
} finally {
    Pop-Location
}
$swGui.Stop()
if ($LASTEXITCODE -ne 0) { throw "GUI decompile failed (exit $LASTEXITCODE)" }

$guiOutputPath = $outGui
$guiSubMs = 0
if (Test-Path -LiteralPath $timingFile) {
    foreach ($line in Get-Content -LiteralPath $timingFile) {
        if ($line -match '^DECOMPILE_MS=(\d+)') { $guiSubMs = [int64]$Matches[1] }
        if ($line -match '^OUTPUT=(.+)$') { $guiOutputPath = $Matches[1].Trim() }
    }
}
if (-not (Test-Path -LiteralPath $guiOutputPath)) { throw "GUI output missing: $guiOutputPath" }

$parityGuiCopy = Join-Path $WorkDir "parity_gui.c"
Copy-Item -LiteralPath $guiOutputPath -Destination $parityGuiCopy -Force

$h1 = (Get-FileHash -LiteralPath $outCli -Algorithm SHA256).Hash
$h2 = (Get-FileHash -LiteralPath $guiOutputPath -Algorithm SHA256).Hash
$match = ($h1 -eq $h2)

@{
    cli_sha256       = $h1
    gui_sha256       = $h2
    byte_identical   = $match
    cli_output       = $outCli
    gui_output       = $guiOutputPath
    cli_seconds      = $swCli.Elapsed.TotalSeconds
    gui_wall_seconds = $swGui.Elapsed.TotalSeconds
    gui_subprocess_ms = $guiSubMs
    fast             = [bool]$Fast
    check_timing     = [bool]$CheckTiming
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $WorkDir "parity_result.json")

Write-Host "CLI SHA256: $($h1.Substring(0, 16))... ($((Get-Item -LiteralPath $outCli).Length) bytes)"
Write-Host "GUI SHA256: $($h2.Substring(0, 16))... ($((Get-Item -LiteralPath $guiOutputPath).Length) bytes)"
Write-Host "byte-identical: $match"

if (-not $match) {
    Write-Error "SHA256 mismatch between CLI and GUI decompiler outputs"
    exit 1
}

if ($CheckTiming) {
    $cliSec = $swCli.Elapsed.TotalSeconds
    $guiSubSec = if ($guiSubMs -gt 0) { $guiSubMs / 1000.0 } else { $swGui.Elapsed.TotalSeconds }
    $delta = [math]::Abs($cliSec - $guiSubSec)
    $pct = if ($cliSec -gt 0) { ($delta / $cliSec) * 100.0 } else { 0 }
    Write-Host ("Timing: CLI={0:F1}s GUI subprocess={1:F1}s delta={2:F1}s ({3:F0}%)" -f $cliSec, $guiSubSec, $delta, $pct)
    if ($pct -gt 5) {
        Write-Error "GUI subprocess differs from CLI by more than 5%"
        exit 1
    }
}

Write-Host "PASS: parity_ctest"
exit 0
