@echo off
REM Profile RetDec decompiler using sanitizers and debug build.
REM Run from project root. Requires prior build.
REM
REM Usage:
REM   scripts\profile_decompiler.bat [binary_to_decompile]
REM
REM If no binary given, uses cmd.exe as test input.

set "BINARY=%~1"
if "%BINARY%"=="" set "BINARY=%SystemRoot%\System32\cmd.exe"

set "BUILD_DIR=build"
set "DECOMPILER=%BUILD_DIR%\src\retdec-decompiler\retdec-decompiler.exe"
if not exist "%DECOMPILER%" set "DECOMPILER=%BUILD_DIR%\src\retdec-decompiler\Release\retdec-decompiler.exe"
if not exist "%DECOMPILER%" set "DECOMPILER=%BUILD_DIR%\src\retdec-decompiler\Debug\retdec-decompiler.exe"

if not exist "%DECOMPILER%" (
    echo ERROR: retdec-decompiler not found. Build first.
    exit /b 1
)

echo Profiling: %DECOMPILER%
echo Input: %BINARY%
echo.

REM Set ASan/UBSan env for better error reporting
set ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:print_legend=1
set UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1

echo === Running decompiler (check for sanitizer output) ===
"%DECOMPILER%" "%BINARY%" -o decompiled_output.c
set EXIT=%ERRORLEVEL%
echo.
echo Exit code: %EXIT%
exit /b %EXIT%
