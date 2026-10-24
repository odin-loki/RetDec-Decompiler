@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d C:\Users\odinl\OneDrive\Desktop\RetDec\build\windows || exit /b 1

set ROOT=C:\Users\odinl\OneDrive\Desktop\RetDec
set SRC=%ROOT%\src\capstone2llvmir
set TST=%ROOT%\tests\capstone2llvmir

set CL_COMMON=/nologo /TP -DCAPSTONE_AARCH64_COMPAT_HEADER -DCAPSTONE_SYSTEMZ_COMPAT_HEADER -DRAPIDJSON_HAS_CXX11_RVALUE_REFS=1 -DRAPIDJSON_HAS_STDSTRING=1 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING -I%ROOT%\include -I%ROOT%\src -external:I%ROOT%\build\windows\external\src\capstone-project\arch -external:I%ROOT%\build\windows\deps\install\capstone\include -external:I%ROOT%\build\windows\external\src\llvm-project\llvm\include -external:I%ROOT%\build\windows\deps\install\llvm\include -external:I%ROOT%\deps\rapidjson\include -external:W0 /W0 /MD /O2 /Ob2 /DNDEBUG -std:c++17

set CL_TEST=/nologo /TP -DCAPSTONE_AARCH64_COMPAT_HEADER -DCAPSTONE_SYSTEMZ_COMPAT_HEADER -DNOMINMAX -DRAPIDJSON_HAS_CXX11_RVALUE_REFS=1 -DRAPIDJSON_HAS_STDSTRING=1 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING -I%ROOT%\tests -I%ROOT%\src -I%ROOT%\include -external:I%ROOT%\build\windows\external\src\capstone-project\arch -external:I%ROOT%\build\windows\deps\install\capstone\include -external:I%ROOT%\build\windows\external\src\llvm-project\llvm\include -external:I%ROOT%\build\windows\deps\install\llvm\include -external:I%ROOT%\deps\rapidjson\include -external:I%ROOT%\build\windows\deps\install\keystone\include -external:I%ROOT%\build\windows\deps\install\googletest\include -external:W0 /W0 /MD /O2 /Ob2 /DNDEBUG -std:c++17 /bigobj

echo === arm64.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm64\arm64.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\arm64\arm64.cpp
if errorlevel 1 exit /b 1

echo === arm64_init.cpp ===
cl %CL_COMMON% /Fosrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm64\arm64_init.cpp.obj /Fdsrc\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\capstone2llvmir.pdb /FS -c %SRC%\arm64\arm64_init.cpp
if errorlevel 1 exit /b 1

echo === lib retdec-capstone2llvmir.lib ===
lib /nologo src\capstone2llvmir\retdec-capstone2llvmir.lib ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm64\arm64.cpp.obj ^
  src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\arm64\arm64_init.cpp.obj
if errorlevel 1 exit /b 1

echo === arm64_tests.cpp ===
cl %CL_TEST% /Fotests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\arm64_tests.cpp.obj /Fdtests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\ /FS -c %TST%\arm64_tests.cpp
if errorlevel 1 exit /b 1

echo === link tests-capstone2llvmir ===
link /nologo /INCREMENTAL:NO /subsystem:console /OUT:tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe @CMakeFiles\tests-capstone2llvmir.rsp
if errorlevel 1 exit /b 1

echo === run leftover SVE mem/pred + regression ===
tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe --gtest_filter=*ARM64_INS_LD1SB*:*ARM64_INS_LD1SH*:*ARM64_INS_LD1SW*:*ARM64_INS_LDNT1SB*:*ARM64_INS_LDNT1SH*:*ARM64_INS_LDFF1W*:*ARM64_INS_LD1W_sve_gather*:*ARM64_INS_ST1W_sve_scatter*:*ARM64_INS_SETFFR*:*ARM64_INS_RDFFR*:*ARM64_INS_WRFFR*:*ARM64_INS_PRFB*:*ARM64_INS_CLASTA*:*ARM64_INS_CLASTB*:*ARM64_INS_SPLICE*:*ARM64_INS_REV_sve*:*ARM64_INS_REVB*:*ARM64_INS_SADDV*:*ARM64_INS_UADDV*:*ARM64_INS_UMAXV_sve*:*ARM64_INS_FCMEQ_sve*:*ARM64_INS_LDNT1W*:*ARM64_INS_FADD_sve*:*ARM64_INS_FMOPA*:*ARM64_INS_COMPACT*
exit /b %ERRORLEVEL%
