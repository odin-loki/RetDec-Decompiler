@echo off
REM Repo root = parent of scripts\. Override runtime dir: set RETDEC_WIN_RUNTIME=...
set "REPO_ROOT=%~dp0.."
if defined RETDEC_WIN_RUNTIME (
  cd /d "%RETDEC_WIN_RUNTIME%"
) else (
  cd /d "%REPO_ROOT%\dist\windows"
)
set PASS=0
set FAIL=0

echo =======================================================
echo Windows RetDec Decompiler Test Suite
echo =======================================================

echo.
echo [TEST 1] Simple PE decompilation (test_hello.exe)
if exist test_hello.exe (
    retdec-decompiler.exe test_hello.exe -o test_out_1.c --silent
    if %errorlevel%==0 (
        if exist test_out_1.c (
            echo PASS: test_hello.exe decompiled successfully
            set /a PASS+=1
        ) else (
            echo FAIL: no output file produced
            set /a FAIL+=1
        )
    ) else (
        echo FAIL: decompiler returned error code %errorlevel%
        set /a FAIL+=1
    )
) else (
    echo SKIP: test_hello.exe not found
)

echo.
echo [TEST 2] retdec-fileinfo on PE binary
if exist test_hello.exe (
    retdec-fileinfo.exe test_hello.exe --silent 2>nul
    if %errorlevel%==0 (
        echo PASS: retdec-fileinfo succeeded
        set /a PASS+=1
    ) else (
        echo FAIL: retdec-fileinfo failed with %errorlevel%
        set /a FAIL+=1
    )
) else (
    echo SKIP: test_hello.exe not found
)

echo.
echo [TEST 3] retdec-decompiler --version
retdec-decompiler.exe --version 2>&1 | findstr /i "retdec\|version" >nul
if %errorlevel%==0 (
    echo PASS: version output found
    set /a PASS+=1
) else (
    echo FAIL: no version output
    set /a FAIL+=1
)

echo.
echo =======================================================
echo RESULTS: %PASS% passed, %FAIL% failed
echo =======================================================
