#Requires -Version 5.1
<#
.SYNOPSIS
  Stage the llama.cpp-native Unsloth Qwen 3.5 9B Q4_K_M GGUF under models/.
#>
[CmdletBinding()]
param(
    [string]$DestDir = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($DestDir)) {
    if ($env:RETDEC_MODEL_DIR) { $DestDir = $env:RETDEC_MODEL_DIR }
    else { $DestDir = Join-Path $RepoRoot "models" }
}
$Dest = Join-Path $DestDir "Qwen3.5-9B-Q4_K_M.gguf"
$Sha = "03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8"
$HfUrl = "https://huggingface.co/unsloth/Qwen3.5-9B-GGUF/resolve/main/Qwen3.5-9B-Q4_K_M.gguf"

New-Item -ItemType Directory -Force -Path $DestDir | Out-Null

function Get-Sha256Hex([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-AlreadyOk([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    return ((Get-Item -LiteralPath $Path).Length -gt 1000000000)
}

function Assert-Sha([string]$Path) {
    $got = Get-Sha256Hex $Path
    if ($got -ne $Sha) {
        throw "SHA-256 mismatch for $Path`n  expected $Sha`n  got      $got"
    }
}

function Write-Exports([string]$Path) {
    Write-Host "[ok] $Path"
    Write-Host "     `$env:RETDEC_NEURAL_MODEL='$Path'"
    Write-Host "     refine is on by default; `$env:RETDEC_NEURAL_REFINE='0' disables"
    Write-Host "     `$env:RETDEC_NEURAL_MODEL_SHA256='$Sha'"
}

foreach ($cand in @($Dest, (Join-Path $DestDir "Qwen3.5-9B-Q4_K_M.unsloth.gguf"))) {
    if (Test-AlreadyOk $cand) {
        Assert-Sha $cand
        Write-Exports $cand
        exit 0
    }
}

Write-Host "Downloading $HfUrl"
& curl.exe -L --retry 5 --retry-delay 10 --continue-at - -o $Dest $HfUrl
if ($LASTEXITCODE -ne 0) { throw "curl.exe failed ($LASTEXITCODE)" }
Assert-Sha $Dest
Write-Exports $Dest
