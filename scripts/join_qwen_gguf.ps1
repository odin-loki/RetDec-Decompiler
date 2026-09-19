#Requires -Version 5.1
<#
.SYNOPSIS
  Concatenate GitHub Release GGUF parts (.partaa, .partab, …) and verify SHA-256.
#>
[CmdletBinding()]
param(
    [string]$PartDir = ".",
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
$Prefix = "Qwen3.5-9B-Q4_K_M.gguf.part"

$parts = Get-ChildItem -LiteralPath $PartDir -File -Filter "$Prefix*" |
    Sort-Object Name
if ($parts.Count -eq 0) {
    throw "no parts matching $PartDir\$Prefix* — download them from the GitHub Release"
}

New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
if (Test-Path -LiteralPath $Dest) { Remove-Item -LiteralPath $Dest -Force }

$out = [System.IO.File]::Create($Dest)
try {
    foreach ($p in $parts) {
        Write-Host "append $($p.Name)"
        $in = [System.IO.File]::OpenRead($p.FullName)
        try { $in.CopyTo($out) }
        finally { $in.Dispose() }
    }
} finally {
    $out.Dispose()
}

$got = (Get-FileHash -LiteralPath $Dest -Algorithm SHA256).Hash.ToLowerInvariant()
if ($got -ne $Sha) {
    throw "SHA-256 mismatch for $Dest`n  expected $Sha`n  got      $got"
}

Write-Host "[ok] $Dest"
Write-Host "     refine is on by default; `$env:RETDEC_NEURAL_REFINE='0' disables"
Write-Host "     `$env:RETDEC_NEURAL_MODEL='$Dest'"
Write-Host "     `$env:RETDEC_NEURAL_MODEL_SHA256='$Sha'"
