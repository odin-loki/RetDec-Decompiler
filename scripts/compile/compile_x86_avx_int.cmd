@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d C:\Users\odinl\OneDrive\Desktop\RetDec\build\windows || exit /b 1

set STAGING=leftover-x86
if not exist %STAGING% mkdir %STAGING%

set CLCMD=cl.exe /nologo /TP -DCAPSTONE_AARCH64_COMPAT_HEADER -DCAPSTONE_SYSTEMZ_COMPAT_HEADER -DRAPIDJSON_HAS_CXX11_RVALUE_REFS=1 -DRAPIDJSON_HAS_STDSTRING=1 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING -IC:\Users\odinl\OneDrive\Desktop\RetDec\include -IC:\Users\odinl\OneDrive\Desktop\RetDec\src -external:IC:\Users\odinl\OneDrive\Desktop\RetDec\build\windows\external\src\capstone-project\arch -external:IC:\Users\odinl\OneDrive\Desktop\RetDec\build\windows\deps\install\capstone\include -external:IC:\Users\odinl\OneDrive\Desktop\RetDec\build\windows\external\src\llvm-project\llvm\include -external:IC:\Users\odinl\OneDrive\Desktop\RetDec\build\windows\deps\install\llvm\include -external:IC:\Users\odinl\OneDrive\Desktop\RetDec\deps\rapidjson\include -external:W0 /W0 /MD /O2 /Ob2 /DNDEBUG -std:c++17 /FS

echo Compiling x86.cpp
%CLCMD% /Fo%STAGING%\x86.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c C:\Users\odinl\OneDrive\Desktop\RetDec\src\capstone2llvmir\x86\x86.cpp
if errorlevel 1 exit /b 1

echo Compiling x86_init.cpp
%CLCMD% /Fo%STAGING%\x86_init.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c C:\Users\odinl\OneDrive\Desktop\RetDec\src\capstone2llvmir\x86\x86_init.cpp
if errorlevel 1 exit /b 1

echo Compiling x86_sse.cpp
%CLCMD% /Fo%STAGING%\x86_sse.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c C:\Users\odinl\OneDrive\Desktop\RetDec\src\capstone2llvmir\x86\x86_sse.cpp
if errorlevel 1 exit /b 1

echo Compiling x86_tests.cpp
%CLCMD% -DNOMINMAX -IC:\Users\odinl\OneDrive\Desktop\RetDec\tests -external:IC:\Users\odinl\OneDrive\Desktop\RetDec\build\windows\deps\install\keystone\include -external:IC:\Users\odinl\OneDrive\Desktop\RetDec\build\windows\deps\install\googletest\include /bigobj /Fo%STAGING%\x86_tests.cpp.obj /Fd%STAGING%\ -c C:\Users\odinl\OneDrive\Desktop\RetDec\tests\capstone2llvmir\x86_tests.cpp
if errorlevel 1 exit /b 1

echo Snapshot lib and sibling test objs
copy /Y src\capstone2llvmir\retdec-capstone2llvmir.lib %STAGING%\retdec-capstone2llvmir.lib >nul
if errorlevel 1 exit /b 1
for %%F in (arm_tests.cpp.obj arm64_tests.cpp.obj exceptions_tests.cpp.obj mips_tests.cpp.obj powerpc_tests.cpp.obj riscv_tests.cpp.obj sparc_tests.cpp.obj sysz_tests.cpp.obj xcore_tests.cpp.obj) do (
	if exist tests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\%%F (
		for %%S in (tests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\%%F) do if not %%~zS==0 copy /Y tests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\%%F %STAGING%\ >nul
	)
	if not exist %STAGING%\%%F (
		echo missing %STAGING%\%%F
		exit /b 1
	)
	for %%S in (%STAGING%\%%F) do if %%~zS==0 (
		echo zero-byte %STAGING%\%%F
		exit /b 1
	)
)

echo Publishing objs into CMake paths and replacing lib members
copy /Y %STAGING%\x86.cpp.obj src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\x86\x86.cpp.obj >nul
if errorlevel 1 exit /b 1
copy /Y %STAGING%\x86_init.cpp.obj src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\x86\x86_init.cpp.obj >nul
if errorlevel 1 exit /b 1
copy /Y %STAGING%\x86_sse.cpp.obj src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\x86\x86_sse.cpp.obj >nul
if errorlevel 1 exit /b 1
copy /Y %STAGING%\x86_tests.cpp.obj tests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\x86_tests.cpp.obj >nul
if errorlevel 1 exit /b 1
lib /nologo %STAGING%\retdec-capstone2llvmir.lib src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\x86\x86.cpp.obj src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\x86\x86_init.cpp.obj src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\x86\x86_sse.cpp.obj
if errorlevel 1 exit /b 1
lib /nologo src\capstone2llvmir\retdec-capstone2llvmir.lib src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\x86\x86.cpp.obj src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\x86\x86_init.cpp.obj src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\x86\x86_sse.cpp.obj
if errorlevel 1 exit /b 1

echo Writing staged rsp
powershell -NoProfile -Command ^
  "(Get-Content -Raw 'tests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.rsp') -replace 'tests\\capstone2llvmir\\CMakeFiles\\tests-capstone2llvmir.dir\\','leftover-x86\' -replace 'src\\capstone2llvmir\\retdec-capstone2llvmir.lib','leftover-x86\retdec-capstone2llvmir.lib' | Set-Content -NoNewline 'leftover-x86\tests.rsp'"
if errorlevel 1 exit /b 1

echo Relinking staged tests
link.exe /nologo @leftover-x86\tests.rsp /out:leftover-x86\retdec-tests-capstone2llvmir.exe /implib:leftover-x86\retdec-tests-capstone2llvmir.lib /pdb:leftover-x86\retdec-tests-capstone2llvmir.pdb /version:0.0 /INCREMENTAL:NO /subsystem:console
if errorlevel 1 exit /b 1

copy /Y leftover-x86\retdec-tests-capstone2llvmir.exe tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe >nul
if errorlevel 1 exit /b 1

echo === run x86 tests ===
leftover-x86\retdec-tests-capstone2llvmir.exe --gtest_filter=*BLCFILL*:*TZMSK*:*PSHUFW*:*PFADD*:*PSWAPD*:*VPBROADCASTMB2Q*:*VPCONFLICTD*:*VPCOMB*
set TEST_RC=%ERRORLEVEL%
echo tests exit %TEST_RC%
exit /b %TEST_RC%
