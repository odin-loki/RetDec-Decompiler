@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d C:\Users\odinl\OneDrive\Desktop\RetDec\build\windows || exit /b 1

set ROOT=C:\Users\odinl\OneDrive\Desktop\RetDec
set SRC=%ROOT%\src\capstone2llvmir
set TST=%ROOT%\tests\capstone2llvmir

set CL_COMMON=/nologo /TP -DCAPSTONE_AARCH64_COMPAT_HEADER -DCAPSTONE_SYSTEMZ_COMPAT_HEADER -DRAPIDJSON_HAS_CXX11_RVALUE_REFS=1 -DRAPIDJSON_HAS_STDSTRING=1 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING -I%ROOT%\include -I%ROOT%\src -external:I%ROOT%\build\windows\external\src\capstone-project\arch -external:I%ROOT%\build\windows\deps\install\capstone\include -external:I%ROOT%\build\windows\external\src\llvm-project\llvm\include -external:I%ROOT%\build\windows\deps\install\llvm\include -external:I%ROOT%\deps\rapidjson\include -external:W0 /W0 /MD /O2 /Ob2 /DNDEBUG -std:c++17

set CL_TEST=/nologo /TP -DCAPSTONE_AARCH64_COMPAT_HEADER -DCAPSTONE_SYSTEMZ_COMPAT_HEADER -DNOMINMAX -DRAPIDJSON_HAS_CXX11_RVALUE_REFS=1 -DRAPIDJSON_HAS_STDSTRING=1 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING -I%ROOT%\tests -I%ROOT%\src -I%ROOT%\include -external:I%ROOT%\build\windows\external\src\capstone-project\arch -external:I%ROOT%\build\windows\deps\install\capstone\include -external:I%ROOT%\build\windows\external\src\llvm-project\llvm\include -external:I%ROOT%\build\windows\deps\install\llvm\include -external:I%ROOT%\deps\rapidjson\include -external:I%ROOT%\build\windows\deps\install\keystone\include -external:I%ROOT%\build\windows\deps\install\googletest\include -external:W0 /W0 /MD /O2 /Ob2 /DNDEBUG -std:c++17 /bigobj

echo === mips.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\mips\mips.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\mips\mips.cpp
if errorlevel 1 exit /b 1

echo === mips_init.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\mips\mips_init.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\mips\mips_init.cpp
if errorlevel 1 exit /b 1

echo === sysz.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\sysz\sysz.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\sysz\sysz.cpp
if errorlevel 1 exit /b 1

echo === sysz_init.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\sysz\sysz_init.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\sysz\sysz_init.cpp
if errorlevel 1 exit /b 1

echo === sparc.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\sparc\sparc.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\sparc\sparc.cpp
if errorlevel 1 exit /b 1

echo === sparc_init.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\sparc\sparc_init.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\sparc\sparc_init.cpp
if errorlevel 1 exit /b 1

echo === arm.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm\arm.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\arm\arm.cpp
if errorlevel 1 exit /b 1

echo === lib retdec-capstone2llvmir.lib ===
lib /nologo src\capstone2llvmir\retdec-capstone2llvmir.lib ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\mips\mips.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\mips\mips_init.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\sysz\sysz.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\sysz\sysz_init.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\sparc\sparc.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\sparc\sparc_init.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm\arm.cpp.obj
if errorlevel 1 exit /b 1

echo === mips_tests.cpp ===
cl %CL_TEST% /Fotests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\mips_tests.cpp.obj /Fdtests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\ /FS -c %TST%\mips_tests.cpp
if errorlevel 1 exit /b 1

echo === sysz_tests.cpp ===
cl %CL_TEST% /Fotests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\sysz_tests.cpp.obj /Fdtests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\ /FS -c %TST%\sysz_tests.cpp
if errorlevel 1 exit /b 1

echo === sparc_tests.cpp ===
cl %CL_TEST% /Fotests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\sparc_tests.cpp.obj /Fdtests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\ /FS -c %TST%\sparc_tests.cpp
if errorlevel 1 exit /b 1

echo === arm_tests.cpp ===
cl %CL_TEST% /Fotests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\arm_tests.cpp.obj /Fdtests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\ /FS -c %TST%\arm_tests.cpp
if errorlevel 1 exit /b 1

echo === link tests-capstone2llvmir ===
link /nologo /INCREMENTAL:NO /subsystem:console /OUT:tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe @CMakeFiles\tests-capstone2llvmir.rsp
if errorlevel 1 exit /b 1

echo === run crypto analogue tests ===
tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe --gtest_filter=*MIPS_INS_CRC32B*:*MIPS_INS_CRC32W*:*MIPS_INS_CRC32CB*:*SYSZ_INS_KM*:*SYSZ_INS_KIMD*:*SYSZ_INS_PCC*:*SYSZ_INS_KMCTR*:*SPARC_INS_XMULX*:*ARM_INS_VMULL_p8*
exit /b %ERRORLEVEL%
