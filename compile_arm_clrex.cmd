@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d C:\Users\odinl\OneDrive\Desktop\RetDec\build\windows || exit /b 1

set ROOT=C:\Users\odinl\OneDrive\Desktop\RetDec
set SRC=%ROOT%\src\capstone2llvmir
set TST=%ROOT%\tests\capstone2llvmir

set CL_COMMON=/nologo /TP -DCAPSTONE_AARCH64_COMPAT_HEADER -DCAPSTONE_SYSTEMZ_COMPAT_HEADER -DRAPIDJSON_HAS_CXX11_RVALUE_REFS=1 -DRAPIDJSON_HAS_STDSTRING=1 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING -I%ROOT%\include -I%ROOT%\src -external:I%ROOT%\build\windows\external\src\capstone-project\arch -external:I%ROOT%\build\windows\deps\install\capstone\include -external:I%ROOT%\build\windows\external\src\llvm-project\llvm\include -external:I%ROOT%\build\windows\deps\install\llvm\include -external:I%ROOT%\deps\rapidjson\include -external:W0 /W0 /MD /O2 /Ob2 /DNDEBUG -std:c++17

set CL_TEST=/nologo /TP -DCAPSTONE_AARCH64_COMPAT_HEADER -DCAPSTONE_SYSTEMZ_COMPAT_HEADER -DNOMINMAX -DRAPIDJSON_HAS_CXX11_RVALUE_REFS=1 -DRAPIDJSON_HAS_STDSTRING=1 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING -I%ROOT%\tests -I%ROOT%\src -I%ROOT%\include -external:I%ROOT%\build\windows\external\src\capstone-project\arch -external:I%ROOT%\build\windows\deps\install\capstone\include -external:I%ROOT%\build\windows\external\src\llvm-project\llvm\include -external:I%ROOT%\build\windows\deps\install\llvm\include -external:I%ROOT%\deps\rapidjson\include -external:I%ROOT%\build\windows\deps\install\keystone\include -external:I%ROOT%\build\windows\deps\install\googletest\include -external:W0 /W0 /MD /O2 /Ob2 /DNDEBUG -std:c++17 /bigobj

echo === arm.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm\arm.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\arm\arm.cpp
if errorlevel 1 exit /b 1

echo === arm_init.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm\arm_init.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\arm\arm_init.cpp
if errorlevel 1 exit /b 1

echo === arm64.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm64\arm64.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\arm64\arm64.cpp
if errorlevel 1 exit /b 1

echo === arm64_init.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm64\arm64_init.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\arm64\arm64_init.cpp
if errorlevel 1 exit /b 1

echo === lib retdec-capstone2llvmir.lib ===
set STAGING=leftover-parent
if not exist %STAGING% mkdir %STAGING%
copy /Y src\capstone2llvmir\retdec-capstone2llvmir.lib %STAGING%\retdec-capstone2llvmir.lib >nul
if errorlevel 1 exit /b 1
lib /nologo %STAGING%\retdec-capstone2llvmir.lib ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm\arm.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm\arm_init.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm64\arm64.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm64\arm64_init.cpp.obj
if errorlevel 1 exit /b 1
lib /nologo src\capstone2llvmir\retdec-capstone2llvmir.lib ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm\arm.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm\arm_init.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm64\arm64.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm64\arm64_init.cpp.obj
if errorlevel 1 echo LIB_INTREE_LOCKED using staging lib
set LINKLIB=src\capstone2llvmir\retdec-capstone2llvmir.lib
if exist %STAGING%\retdec-capstone2llvmir.lib set LINKRSP=%STAGING%\tests.rsp
if exist %STAGING%\retdec-capstone2llvmir.lib powershell -NoProfile -Command "$c = [IO.File]::ReadAllText('CMakeFiles\tests-capstone2llvmir.rsp'); $c = $c.Replace('src\capstone2llvmir\retdec-capstone2llvmir.lib', 'leftover-parent\retdec-capstone2llvmir.lib'); [IO.File]::WriteAllText('leftover-parent\tests.rsp', $c)"

echo === arm_tests.cpp ===
cl %CL_TEST% /Fotests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\arm_tests.cpp.obj /Fdtests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\ /FS -c %TST%\arm_tests.cpp
if errorlevel 1 exit /b 1

echo === arm64_tests.cpp ===
cl %CL_TEST% /Fotests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\arm64_tests.cpp.obj /Fdtests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\ /FS -c %TST%\arm64_tests.cpp
if errorlevel 1 exit /b 1

echo === link tests-capstone2llvmir ===
if exist leftover-parent\tests.rsp (
  link /nologo /INCREMENTAL:NO /subsystem:console /OUT:leftover-parent\retdec-tests-capstone2llvmir.exe @leftover-parent\tests.rsp
  if errorlevel 1 exit /b 1
  copy /Y leftover-parent\retdec-tests-capstone2llvmir.exe tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe >nul
) else (
  link /nologo /INCREMENTAL:NO /subsystem:console /OUT:tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe @CMakeFiles\tests-capstone2llvmir.rsp
  if errorlevel 1 exit /b 1
)

echo === run new ARM/ARM64 compiler-bar tests then full ARM and ARM64 suites ===
tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe "--gtest_filter=*ARM_INS_CLREX*:*ARM_INS_FLDM*:*ARM_INS_FSTM*:*ARM_INS_MRS_apsr*:*ARM_INS_MSR_apsr*:*ARM_INS_MRS_spsr*:*ARM64_INS_CLREX*"
if errorlevel 1 exit /b 1
tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe "--gtest_filter=InstantiateArmWithAllModes*:InstantiateArm64WithAllModes*"
exit /b %ERRORLEVEL%
