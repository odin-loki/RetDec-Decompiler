#requires -Version 5.1
<#
.SYNOPSIS
  Unpack a packed binary (when applicable), then decompile with retdec-decompiler.

.DESCRIPTION
  1. Optionally runs retdec-fileinfo for diagnostics.
  2. Runs retdec-unpacker (Python wrapper or native binary).
  3. Runs retdec-decompiler on the unpacked file, or the original if unpacking
     was not needed.

.EXAMPLE
  .\scripts\unpack_and_decompile.ps1 C:\samples\packed.exe

.EXAMPLE
  .\scripts\unpack_and_decompile.ps1 .\sample.exe -Output .\out.c -KeepUnpacked
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $InputFile,

    [Alias("o")]
    [string] $Output = "",

    [switch] $KeepUnpacked,

    [switch] $Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Show-Usage {
    @"
Usage: .\scripts\unpack_and_decompile.ps1 INPUT [-Output OUTPUT] [-KeepUnpacked] [-Help]

  INPUT            Binary to unpack and decompile
  -Output, -o      Decompiler output path (default: INPUT.c beside input)
  -KeepUnpacked    Do not delete the intermediate *-unpacked file
  -Help            Show this synopsis

Steps:
  1. Optional retdec-fileinfo probe (informational)
  2. retdec-unpacker (Python wrapper or native binary)
  3. retdec-decompiler on unpacked file, or original if nothing to unpack

Exit code: same as retdec-decompiler (0 = success).

.EXAMPLE
  .\scripts\unpack_and_decompile.ps1 C:\samples\packed.exe

.EXAMPLE
  .\scripts\unpack_and_decompile.ps1 .\sample.exe -Output .\out.c -KeepUnpacked
"@
}

if ($Help) {
    Show-Usage
    exit 0
}

if ([string]::IsNullOrWhiteSpace($InputFile)) {
    Show-Usage
    Write-Error "Missing required parameter: InputFile"
    exit 1
}

. (Join-Path $PSScriptRoot "retdec-paths.ps1")

$repoRoot = Get-RetDecRepoRoot
$installBin = Join-Path (Get-RetDecInstallDir) "bin"
$buildBin = Join-Path (Get-RetDecBuildDir) "bin"

function Resolve-RetDecTool {
    param([string] $Name)
    foreach ($dir in @($installBin, $buildBin)) {
        $candidate = Join-Path $dir $Name
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    return $null
}

$InputFile = (Resolve-Path -LiteralPath $InputFile).Path

if (-not $Output) {
    $base = [System.IO.Path]::GetFileNameWithoutExtension($InputFile)
    if (-not $base) { $base = $InputFile }
    $Output = Join-Path (Split-Path -Parent $InputFile) ($base + ".c")
}

$decompiler = Resolve-RetDecTool "retdec-decompiler.exe"
if (-not $decompiler) {
    $decompiler = Resolve-RetDecTool "retdec-decompiler"
}
if (-not $decompiler) {
    throw "retdec-decompiler not found (build/install or add to PATH)"
}

$fileinfoPy = Join-Path $PSScriptRoot "retdec-fileinfo.py"
if (Test-Path -LiteralPath $fileinfoPy) {
    Write-Host "[unpack_and_decompile] Probing input with fileinfo..."
    & python $fileinfoPy $InputFile --silent 2>$null
}

$workInput = $InputFile
$unpacked = $InputFile + "-unpacked"
$unpackRan = $false

$unpackerPy = Join-Path $PSScriptRoot "retdec-unpacker.py"
if (Test-Path -LiteralPath $unpackerPy) {
    Write-Host "[unpack_and_decompile] Running retdec-unpacker..."
    & python $unpackerPy $InputFile -o $unpacked --extended-exit-codes
    $unpackRc = $LASTEXITCODE
    if (($unpackRc -eq 0 -or $unpackRc -eq 1 -or $unpackRc -eq 3) -and (Test-Path -LiteralPath $unpacked)) {
        $workInput = $unpacked
        $unpackRan = $true
        Write-Host "[unpack_and_decompile] Using unpacked file: $workInput"
    } else {
        Write-Host "[unpack_and_decompile] Unpacker: nothing to do or failed (rc=$unpackRc); decompiling original."
    }
} else {
    $unpacker = Resolve-RetDecTool "retdec-unpacker.exe"
    if (-not $unpacker) { $unpacker = Resolve-RetDecTool "retdec-unpacker" }
    if ($unpacker) {
        Write-Host "[unpack_and_decompile] Running $unpacker..."
        & $unpacker $InputFile -o $unpacked
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $unpacked)) {
            $workInput = $unpacked
            $unpackRan = $true
        }
    }
}

Write-Host "[unpack_and_decompile] Decompiling: $workInput -> $Output"
& $decompiler -o $Output $workInput
$decRc = $LASTEXITCODE

if ($unpackRan -and -not $KeepUnpacked) {
    Remove-Item -LiteralPath $unpacked -Force -ErrorAction SilentlyContinue
}

exit $decRc
