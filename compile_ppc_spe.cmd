@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d C:\Users\odinl\OneDrive\Desktop\RetDec\build\windows || exit /b 1

set ROOT=C:\Users\odinl\OneDrive\Desktop\RetDec
set SRC=%ROOT%\src\capstone2llvmir
set TST=%ROOT%\tests\capstone2llvmir

set CL_COMMON=/nologo /TP -DCAPSTONE_AARCH64_COMPAT_HEADER -DCAPSTONE_SYSTEMZ_COMPAT_HEADER -DRAPIDJSON_HAS_CXX11_RVALUE_REFS=1 -DRAPIDJSON_HAS_STDSTRING=1 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING -I%ROOT%\include -I%ROOT%\src -external:I%ROOT%\build\windows\external\src\capstone-project\arch -external:I%ROOT%\build\windows\deps\install\capstone\include -external:I%ROOT%\build\windows\external\src\llvm-project\llvm\include -external:I%ROOT%\build\windows\deps\install\llvm\include -external:I%ROOT%\deps\rapidjson\include -external:W0 /W0 /MD /O2 /Ob2 /DNDEBUG -std:c++17

set CL_TEST=/nologo /TP -DCAPSTONE_AARCH64_COMPAT_HEADER -DCAPSTONE_SYSTEMZ_COMPAT_HEADER -DNOMINMAX -DRAPIDJSON_HAS_CXX11_RVALUE_REFS=1 -DRAPIDJSON_HAS_STDSTRING=1 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING -I%ROOT%\tests -I%ROOT%\src -I%ROOT%\include -external:I%ROOT%\build\windows\external\src\capstone-project\arch -external:I%ROOT%\build\windows\deps\install\capstone\include -external:I%ROOT%\build\windows\external\src\llvm-project\llvm\include -external:I%ROOT%\build\windows\deps\install\llvm\include -external:I%ROOT%\deps\rapidjson\include -external:I%ROOT%\build\windows\deps\install\keystone\include -external:I%ROOT%\build\windows\deps\install\googletest\include -external:W0 /W0 /MD /O2 /Ob2 /DNDEBUG -std:c++17 /bigobj

echo === powerpc.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\powerpc\powerpc.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\powerpc\powerpc.cpp
if errorlevel 1 exit /b 1

echo === powerpc_init.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\powerpc\powerpc_init.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\powerpc\powerpc_init.cpp
if errorlevel 1 exit /b 1

echo === lib TEMP retdec-capstone2llvmir-spe.lib ===
copy /Y src\capstone2llvmir\retdec-capstone2llvmir.lib %TEMP%\retdec-capstone2llvmir-spe.lib
if errorlevel 1 (
  echo in-tree lib locked, using existing TEMP lib
)
lib /nologo /OUT:%TEMP%\retdec-capstone2llvmir-spe.lib %TEMP%\retdec-capstone2llvmir-spe.lib src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\powerpc\powerpc.cpp.obj src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\powerpc\powerpc_init.cpp.obj
if errorlevel 1 exit /b 1

echo === powerpc_tests.cpp ===
cl %CL_TEST% /Fotests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\powerpc_tests.cpp.obj /Fdtests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\ /FS -c %TST%\powerpc_tests.cpp
if errorlevel 1 exit /b 1

echo === link tests-capstone2llvmir ===
powershell -NoProfile -Command "(Get-Content -Raw CMakeFiles\tests-capstone2llvmir.rsp) -replace 'src\\capstone2llvmir\\retdec-capstone2llvmir.lib', ($env:TEMP + '\retdec-capstone2llvmir-spe.lib') | Set-Content -NoNewline %TEMP%\tests-capstone2llvmir-spe.rsp"
link /nologo /INCREMENTAL:NO /subsystem:console /OUT:%TEMP%\retdec-tests-capstone2llvmir-spe.exe @%TEMP%\tests-capstone2llvmir-spe.rsp
if errorlevel 1 exit /b 1

echo === run PowerPC SPE + QPX tests ===
"%TEMP%\retdec-tests-capstone2llvmir-spe.exe" --gtest_filter=*Powerpc*EV*:*Powerpc*QV*:*SPEFSCR*:*BRINC*:*MFOCRF*:*MTOCRF*
exit /b %ERRORLEVEL%
