@echo off
REM Windows profiling helper for the staged decompiler + test PE.
REM Default runtime: dist\windows (same as wsl_build / native install staging).
REM Override: set DECOMPILER_DIR=... before calling, or rely on build-win\win-runtime fallback.
REM
REM Tools used (Windows):
REM   1. Plain run - baseline
REM   2. Application Verifier (if installed) - heap corruption
REM   3. Debug build with assertions (if available)
REM
REM For Valgrind/ASan/UBSan: use WSL or build with -fsanitize=address
REM See docs/developer_guide.md (Debugging) and docs/BUILD_REFERENCE.md.

set "REPO_ROOT=%~dp0.."
if not defined DECOMPILER_DIR set "DECOMPILER_DIR=%REPO_ROOT%\dist\windows"
set "DECOMPILER=%DECOMPILER_DIR%\retdec-decompiler.exe"
set "BINARY=%DECOMPILER_DIR%\test_hello.exe"
if not exist "%DECOMPILER%" if exist "%REPO_ROOT%\build-win\win-runtime\retdec-decompiler.exe" (
  set "DECOMPILER_DIR=%REPO_ROOT%\build-win\win-runtime"
  set "DECOMPILER=%DECOMPILER_DIR%\retdec-decompiler.exe"
  set "BINARY=%DECOMPILER_DIR%\test_hello.exe"
)
set "OUT_DIR=%REPO_ROOT%\profile_output"
set "LOG=%OUT_DIR%\profile_run.log"

if not exist "%DECOMPILER%" (
    echo ERROR: retdec-decompiler not found at %DECOMPILER%
    exit /b 1
)

mkdir "%OUT_DIR%" 2>nul
echo Profile run at %date% %time% > "%LOG%"
echo. >> "%LOG%"

echo === 1. Baseline run ===
echo 1. Baseline run >> "%LOG%"
"%DECOMPILER%" "%BINARY%" -o "%OUT_DIR%\out_baseline.c" --silent 2>> "%LOG%"
if %errorlevel% neq 0 (
    echo FAIL: Baseline run failed with exit code %errorlevel%
    echo FAIL: exit %errorlevel% >> "%LOG%"
) else (
    echo PASS: Baseline run succeeded
    echo PASS >> "%LOG%"
)

echo.
echo === 2. Verbose run (check for warnings) ===
echo 2. Verbose run >> "%LOG%"
"%DECOMPILER%" "%BINARY%" -o "%OUT_DIR%\out_verbose.c" >> "%LOG%" 2>&1
echo Exit: %errorlevel% >> "%LOG%"

echo.
echo === 3. Larger binary (cmd.exe) ===
echo 3. cmd.exe run >> "%LOG%"
"%DECOMPILER%" "%SystemRoot%\System32\cmd.exe" -o "%OUT_DIR%\out_cmd.c" --silent 2>> "%LOG%"
if errorlevel 1 (
    echo WARN: cmd.exe decompilation failed - may be expected for large binary
    echo WARN >> "%LOG%"
) else (
    echo PASS: cmd.exe decompiled
    echo PASS >> "%LOG%"
)

echo.
echo Profile log: %LOG%
exit /b 0
