# Linux/WSL (or other Unix host) -> Windows PE via MinGW-w64. Not an MSVC toolchain.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

# Keep try-compile from attempting to run target binaries.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Optional user-space OpenSSL prefix for cross builds (override: cmake -DRETDEC_MINGW_OPENSSL_ROOT=...).
set(RETDEC_MINGW_OPENSSL_ROOT "$ENV{HOME}/toolchains/openssl-mingw" CACHE PATH "Cross OpenSSL root path")

set(CMAKE_FIND_ROOT_PATH "/usr/x86_64-w64-mingw32")
if(EXISTS "${RETDEC_MINGW_OPENSSL_ROOT}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${RETDEC_MINGW_OPENSSL_ROOT}")
    set(OPENSSL_ROOT_DIR "${RETDEC_MINGW_OPENSSL_ROOT}" CACHE PATH "OpenSSL root for cross build" FORCE)
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
