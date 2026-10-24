@echo off
setlocal EnableExtensions
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
cd /d C:\Users\odinl\OneDrive\Desktop\RetDec\build\windows || exit /b 1

set ROOT=C:\Users\odinl\OneDrive\Desktop\RetDec
set SRC=%ROOT%\src\capstone2llvmir
set TST=%ROOT%\tests\capstone2llvmir
set STAGING=leftover-parent
if not exist %STAGING% mkdir %STAGING%

set CL_COMMON=/nologo /TP -DCAPSTONE_AARCH64_COMPAT_HEADER -DCAPSTONE_SYSTEMZ_COMPAT_HEADER -DRAPIDJSON_HAS_CXX11_RVALUE_REFS=1 -DRAPIDJSON_HAS_STDSTRING=1 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING -I%ROOT%\include -I%ROOT%\src -external:I%ROOT%\build\windows\external\src\capstone-project\arch -external:I%ROOT%\build\windows\deps\install\capstone\include -external:I%ROOT%\build\windows\external\src\llvm-project\llvm\include -external:I%ROOT%\build\windows\deps\install\llvm\include -external:I%ROOT%\deps\rapidjson\include -external:W0 /W0 /MD /O2 /Ob2 /DNDEBUG -std:c++17 /FS
set CL_TEST=/nologo /TP -DCAPSTONE_AARCH64_COMPAT_HEADER -DCAPSTONE_SYSTEMZ_COMPAT_HEADER -DNOMINMAX -DRAPIDJSON_HAS_CXX11_RVALUE_REFS=1 -DRAPIDJSON_HAS_STDSTRING=1 -D_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING -I%ROOT%\tests -I%ROOT%\src -I%ROOT%\include -external:I%ROOT%\build\windows\external\src\capstone-project\arch -external:I%ROOT%\build\windows\deps\install\capstone\include -external:I%ROOT%\build\windows\external\src\llvm-project\llvm\include -external:I%ROOT%\build\windows\deps\install\llvm\include -external:I%ROOT%\deps\rapidjson\include -external:I%ROOT%\build\windows\deps\install\keystone\include -external:I%ROOT%\build\windows\deps\install\googletest\include -external:W0 /W0 /MD /O2 /Ob2 /DNDEBUG -std:c++17 /bigobj /FS

set LIBDIR=src\capstone2llvmir\CMakeFiles\capstone2llvmir.dir
set TSTDIR=tests\capstone2llvmir\CMakeFiles\tests-capstone2llvmir.dir

echo === compile translators ===
cl %CL_COMMON% /Fo%LIBDIR%\riscv\riscv.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\riscv\riscv.cpp
if errorlevel 1 (echo COMPILE_FAIL riscv.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\riscv\riscv_init.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\riscv\riscv_init.cpp
if errorlevel 1 (echo COMPILE_FAIL riscv_init.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\mips\mips.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\mips\mips.cpp
if errorlevel 1 (echo COMPILE_FAIL mips.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\mips\mips_init.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\mips\mips_init.cpp
if errorlevel 1 (echo COMPILE_FAIL mips_init.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\powerpc\powerpc.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\powerpc\powerpc.cpp
if errorlevel 1 (echo COMPILE_FAIL powerpc.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\powerpc\powerpc_init.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\powerpc\powerpc_init.cpp
if errorlevel 1 (echo COMPILE_FAIL powerpc_init.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\arm64\arm64.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\arm64\arm64.cpp
if errorlevel 1 (echo COMPILE_FAIL arm64.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\arm64\arm64_init.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\arm64\arm64_init.cpp
if errorlevel 1 (echo COMPILE_FAIL arm64_init.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\arm\arm.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\arm\arm.cpp
if errorlevel 1 (echo COMPILE_FAIL arm.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\arm\arm_init.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\arm\arm_init.cpp
if errorlevel 1 (echo COMPILE_FAIL arm_init.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\x86\x86.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\x86\x86.cpp
if errorlevel 1 (echo COMPILE_FAIL x86.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\x86\x86_init.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\x86\x86_init.cpp
if errorlevel 1 (echo COMPILE_FAIL x86_init.cpp & exit /b 1)
cl %CL_COMMON% /Fo%LIBDIR%\x86\x86_sse.cpp.obj /Fd%STAGING%\capstone2llvmir.pdb -c %SRC%\x86\x86_sse.cpp
if errorlevel 1 (echo COMPILE_FAIL x86_sse.cpp & exit /b 1)
echo COMPILE_BATCH_OK

echo === lib ===
copy /Y src\capstone2llvmir\retdec-capstone2llvmir.lib %STAGING%\retdec-capstone2llvmir.lib >nul
if errorlevel 1 exit /b 1
lib /nologo %STAGING%\retdec-capstone2llvmir.lib ^
  %LIBDIR%\riscv\riscv.cpp.obj %LIBDIR%\riscv\riscv_init.cpp.obj ^
  %LIBDIR%\mips\mips.cpp.obj %LIBDIR%\mips\mips_init.cpp.obj ^
  %LIBDIR%\powerpc\powerpc.cpp.obj %LIBDIR%\powerpc\powerpc_init.cpp.obj ^
  %LIBDIR%\arm64\arm64.cpp.obj %LIBDIR%\arm64\arm64_init.cpp.obj ^
  %LIBDIR%\arm\arm.cpp.obj %LIBDIR%\arm\arm_init.cpp.obj ^
  %LIBDIR%\x86\x86.cpp.obj %LIBDIR%\x86\x86_init.cpp.obj %LIBDIR%\x86\x86_sse.cpp.obj
if errorlevel 1 (echo LIB_FAIL & exit /b 1)
lib /nologo src\capstone2llvmir\retdec-capstone2llvmir.lib ^
  %LIBDIR%\riscv\riscv.cpp.obj %LIBDIR%\riscv\riscv_init.cpp.obj ^
  %LIBDIR%\mips\mips.cpp.obj %LIBDIR%\mips\mips_init.cpp.obj ^
  %LIBDIR%\powerpc\powerpc.cpp.obj %LIBDIR%\powerpc\powerpc_init.cpp.obj ^
  %LIBDIR%\arm64\arm64.cpp.obj %LIBDIR%\arm64\arm64_init.cpp.obj ^
  %LIBDIR%\arm\arm.cpp.obj %LIBDIR%\arm\arm_init.cpp.obj ^
  %LIBDIR%\x86\x86.cpp.obj %LIBDIR%\x86\x86_init.cpp.obj %LIBDIR%\x86\x86_sse.cpp.obj
if errorlevel 1 echo LIB_INTREE_LOCKED using staging lib
echo LIB_OK

echo === compile tests ===
cl %CL_TEST% /Fo%TSTDIR%\riscv_tests.cpp.obj /Fd%STAGING%\tests.pdb -c %TST%\riscv_tests.cpp
if errorlevel 1 (echo COMPILE_FAIL riscv_tests.cpp & exit /b 1)
cl %CL_TEST% /Fo%TSTDIR%\mips_tests.cpp.obj /Fd%STAGING%\tests.pdb -c %TST%\mips_tests.cpp
if errorlevel 1 (echo COMPILE_FAIL mips_tests.cpp & exit /b 1)
cl %CL_TEST% /Fo%TSTDIR%\powerpc_tests.cpp.obj /Fd%STAGING%\tests.pdb -c %TST%\powerpc_tests.cpp
if errorlevel 1 (echo COMPILE_FAIL powerpc_tests.cpp & exit /b 1)
cl %CL_TEST% /Fo%TSTDIR%\arm64_tests.cpp.obj /Fd%STAGING%\tests.pdb -c %TST%\arm64_tests.cpp
if errorlevel 1 (echo COMPILE_FAIL arm64_tests.cpp & exit /b 1)
cl %CL_TEST% /Fo%TSTDIR%\arm_tests.cpp.obj /Fd%STAGING%\tests.pdb -c %TST%\arm_tests.cpp
if errorlevel 1 (echo COMPILE_FAIL arm_tests.cpp & exit /b 1)
cl %CL_TEST% /Fo%TSTDIR%\x86_tests.cpp.obj /Fd%STAGING%\tests.pdb -c %TST%\x86_tests.cpp
if errorlevel 1 (echo COMPILE_FAIL x86_tests.cpp & exit /b 1)

echo === link ===
powershell -NoProfile -Command "$c = [IO.File]::ReadAllText('CMakeFiles\tests-capstone2llvmir.rsp'); $c = $c.Replace('src\capstone2llvmir\retdec-capstone2llvmir.lib', 'leftover-parent\retdec-capstone2llvmir.lib'); [IO.File]::WriteAllText('leftover-parent\tests.rsp', $c)"
if errorlevel 1 exit /b 1
link /nologo /INCREMENTAL:NO /subsystem:console /OUT:%STAGING%\retdec-tests-capstone2llvmir.exe @%STAGING%\tests.rsp
if errorlevel 1 (echo LINK_FAIL & exit /b 1)
copy /Y %STAGING%\retdec-tests-capstone2llvmir.exe tests\capstone2llvmir\retdec-tests-capstone2llvmir.exe >nul
echo LINK_OK

echo === leftover filters ===
%STAGING%\retdec-tests-capstone2llvmir.exe --gtest_filter=*BLCFILL*:*TZMSK*:*PSHUFW*:*PFADD*:*PSWAPD*:*VPBROADCASTMB2Q*:*VPCONFLICTD*:*VPCOMB*
if errorlevel 1 (echo TEST_FAIL x86 & exit /b 1)
%STAGING%\retdec-tests-capstone2llvmir.exe --gtest_filter=*MIPS_INS_CMP_EQ_S*:*MIPS_INS_POP*:*MIPS_INS_BADDU*:*MIPS_INS_CINS*:*MIPS_INS_EXTS*
if errorlevel 1 (echo TEST_FAIL mips & exit /b 1)
%STAGING%\retdec-tests-capstone2llvmir.exe --gtest_filter=*PPC_INS_MFOCRF*:*PPC_INS_MTOCRF*
echo leftover-parent tests exit %ERRORLEVEL%
exit /b %ERRORLEVEL%
