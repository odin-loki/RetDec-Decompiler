#requires -Version 5.1
# Leftover: looks for retdec-qwen3-runner.exe, which does not ship (DOC-06 / B5).
# Product neural path is RETDEC_NEURAL_REFINE + RETDEC_NEURAL_MODEL.
param(
    [Parameter(Mandatory = $true)]
    [string] $Model,
    [string] $Runner = "",
    [string] $Preset = "full-windows-release",
    [string[]] $Args = @(),
    [switch] $VerboseMoe
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "retdec-paths.ps1")

if (-not $Runner) {
    $root = Get-RetDecRepoRoot
    $candidates = @(
        (Join-Path $root "build\$Preset\src\qwen3_runner\retdec-qwen3-runner.exe"),
        (Join-Path $root "install\$Preset\bin\retdec-qwen3-runner.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $Runner = $c; break }
    }
}
if (-not $Runner -or -not (Test-Path $Runner)) {
    Write-Error "retdec-qwen3-runner not found. Pass -Runner or build preset $Preset."
}

$env:RETDEC_QWEN3_TRACE = "1"
if ($VerboseMoe) { $env:RETDEC_QWEN3_TRACE_VERBOSE = "1" }
else { Remove-Item env:RETDEC_QWEN3_TRACE_VERBOSE -ErrorAction SilentlyContinue }

Write-Host "RETDEC_QWEN3_TRACE=1 $(if ($VerboseMoe) { 'RETDEC_QWEN3_TRACE_VERBOSE=1 ' })& `"$Runner`" `"$Model`" $($Args -join ' ')"
& $Runner $Model @Args
