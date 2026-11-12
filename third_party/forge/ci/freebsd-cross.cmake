# FreeBSD amd64 cross toolchain (clang + lld). FREEBSD_SYSROOT = usr/include, usr/lib, lib extracted from
# download.freebsd.org/releases/amd64/<ver>-RELEASE/base.txz. Used by ci/freebsd-vm.sh.
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSROOT $ENV{FREEBSD_SYSROOT})
set(CMAKE_C_COMPILER_TARGET x86_64-unknown-freebsd14.4)
set(CMAKE_CXX_COMPILER_TARGET x86_64-unknown-freebsd14.4)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
