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

echo === lib TEMP retdec-capstone2llvmir-simd.lib ===
copy /Y src\capstone2llvmir\retdec-capstone2llvmir.lib %TEMP%\retdec-capstone2llvmir-simd.lib
if errorlevel 1 (
  echo in-tree lib locked, using existing TEMP lib
)
lib /nologo /OUT:%TEMP%\retdec-capstone2llvmir-simd.lib %TEMP%\retdec-capstone2llvmir-simd.lib src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\powerpc\powerpc.cpp.obj src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\powerpc\powerpc_init.cpp.obj
if errorlevel 1 exit /b 1

echo === powerpc_tests.cpp ===
cl %CL_TEST% /Fo%TEMP%\powerpc_tests.cpp.obj /Fd%TEMP%\powerpc_tests.pdb /FS -c %TST%\powerpc_tests.cpp
if errorlevel 1 exit /b 1

echo === link tests-capstone2llvmir ===
powershell -NoProfile -Command "$c = [IO.File]::ReadAllText('CMakeFiles\tests-capstone2llvmir.rsp'); $c = $c.Replace('src\capstone2llvmir\retdec-capstone2llvmir.lib', $env:TEMP + '\retdec-capstone2llvmir-simd.lib'); $c = $c.Replace('tests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\powerpc_tests.cpp.obj', $env:TEMP + '\powerpc_tests.cpp.obj'); [IO.File]::WriteAllText($env:TEMP + '\tests-capstone2llvmir-simd.rsp', $c)"
link /nologo /INCREMENTAL:NO /subsystem:console /OUT:%TEMP%\retdec-tests-capstone2llvmir-simd.exe @%TEMP%\tests-capstone2llvmir-simd.rsp
if errorlevel 1 exit /b 1

echo === run PowerPC leftover SIMD + SPE + QPX tests ===
"%TEMP%\retdec-tests-capstone2llvmir-simd.exe" --gtest_filter=*Powerpc*ISEL*:*Powerpc*CMPB*:*Powerpc*DIVDE*:*Powerpc*VCLZ*:*Powerpc*VAVG*:*Powerpc*VCMP*:*Powerpc*LVE*:*Powerpc*STVE*:*Powerpc*XVRSQRTE*:*Powerpc*VRSQRTE*:*Powerpc*VREFP*:*Powerpc*XVRE*:*Powerpc*XVSQRT*:*Powerpc*VADDUDM*:*Powerpc*VSUBUDM*:*Powerpc*VADDCUW*:*Powerpc*VSUBCUW*:*Powerpc*VRLD*:*Powerpc*VSLO*:*Powerpc*VSRO*:*Powerpc*VPOPCNT*:*Powerpc*VMAX*:*Powerpc*VMIN*:*Powerpc*XVCV*:*Powerpc*VMADDFP*:*Powerpc*VMRG*:*Powerpc*VMULE*:*Powerpc*VMULO*:*Powerpc*VMULUWM*:*Powerpc*VMSUM*:*Powerpc*VMHADD*:*Powerpc*VMHRADD*:*Powerpc*VMLADD*:*Powerpc*VPKPX*:*Powerpc*VUPK*:*Powerpc*VSUM*:*Powerpc*VEXPTE*:*Powerpc*VLOGE*:*Powerpc*XVTDIV*:*Powerpc*XVTSQRT*:*Powerpc*XS*:*Powerpc*XX*:*Powerpc*LXVDSX*:*Powerpc*LXVW4X*:*Powerpc*STXVW4X*:*Powerpc*MFFS*:*Powerpc*MTFS*:*Powerpc*MCRFS*:*Powerpc*VSCR*:*Powerpc*EV*:*Powerpc*QV*:*SPEFSCR*
exit /b %ERRORLEVEL%
