#Requires -Version 5.1
<#
.SYNOPSIS
    Download Sysinternals ProcDump (if needed) and launch retdec-gui.exe under it
    so an unhandled exception writes a full-memory .dmp next to the script.

.PARAMETER GuiDir
    Folder containing retdec-gui.exe (default: dist\windows\debuggable).
#>
param(
    [string]$GuiDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ($GuiDir -eq "") { $GuiDir = Join-Path $RepoRoot "dist\windows\debuggable" }
$exe = Join-Path $GuiDir "retdec-gui.exe"
if (-not (Test-Path $exe)) {
    Write-Error "retdec-gui.exe not found at $exe`nRun scripts\windows_prepare_debuggable_gui.ps1 first."
    exit 1
}

$tools = Join-Path $RepoRoot "tools\procdump"
$procdump = Join-Path $tools "Procdump64.exe"
New-Item -ItemType Directory -Force -Path $tools | Out-Null

if (-not (Test-Path $procdump)) {
    Write-Host "Downloading Sysinternals ProcDump..." -ForegroundColor Cyan
    $zip = Join-Path $tools "Procdump.zip"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri "https://download.sysinternals.com/files/Procdump.zip" -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $tools -Force
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
}

$dumps = Join-Path $RepoRoot "_crash_dumps"
New-Item -ItemType Directory -Force -Path $dumps | Out-Null

Write-Host ""
Write-Host "Launching under ProcDump - on crash, a .dmp file appears in:" -ForegroundColor Yellow
Write-Host "  $dumps" -ForegroundColor White
Write-Host ""
Write-Host "Reproduce the AI Assistant crash, then zip the newest .dmp for analysis." -ForegroundColor Gray
Write-Host ""

Push-Location $GuiDir
try {
    # -e = dump on unhandled exception; -ma = full memory; -x = dump folder then image
    & $procdump -accepteula -e -ma -x $dumps $exe
} finally {
    Pop-Location
}
