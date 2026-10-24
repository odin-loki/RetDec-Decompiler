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

echo === lib ===
lib /nologo src\capstone2llvmir\retdec-capstone2llvmir.lib src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\mips\mips.cpp.obj src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir\mips\mips_init.cpp.obj
if errorlevel 1 exit /b 1

echo === mips_tests.cpp ===
cl %CL_TEST% /Fotests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\mips_tests.cpp.obj /Fdtests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir\ /FS -c %TST%\mips_tests.cpp
if errorlevel 1 exit /b 1

echo === link ===
link /nologo /INCREMENTAL:NO /subsystem:console /OUT:tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe @CMakeFiles\tests-capstone2llvmir.rsp
if errorlevel 1 exit /b 1

echo === tests ===
tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe --gtest_filter=*MIPS_INS_SRA_W*:*MIPS_INS_SLLI_W*:*MIPS_INS_SRAI_W*:*MIPS_INS_SRLI_W*:*MIPS_INS_SRAR_W*:*MIPS_INS_SRARI_W*:*MIPS_INS_SRLR_W*:*MIPS_INS_SRLRI_W*:*MIPS_INS_SUBSUS_U_W*:*MIPS_INS_SUBSUU_S_W*:*MIPS_INS_SLL_B*:*MIPS_INS_SRL_W*:*MIPS_INS_SLL/CS*:*MIPS_INS_SRA_no_sign*
exit /b %ERRORLEVEL%
