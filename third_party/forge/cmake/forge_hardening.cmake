# FSS-BLD-1 build hardening (docs/security-standard.md). Included from the top-level CMakeLists.txt after the
# sanitizer options. Every flag is probed (compile + link of a small program that has a stack buffer and a
# fortifiable libc call) and silently skipped when the toolchain rejects it or warns about it, so turning
# FORGE_HARDENING on never breaks a toolchain. What was enabled is recorded as a bitmask in the compile
# definition FORGE_HARDENING_MASK, which core/include/forge/core/build_info.hpp decodes (forge::build_hardening()).
#
# Bits (keep in sync with build_info.hpp):
#   0 stack_protector      -fstack-protector-strong            (MSVC: /GS, on by default)
#   1 fortify_source       -D_FORTIFY_SOURCE=3                 (optimised configs, glibc Linux, no sanitizers)
#   2 stack_clash          -fstack-clash-protection
#   3 pie                  -fPIE / -pie                        (ELF; Mach-O is PIE by default)
#   4 relro                -Wl,-z,relro
#   5 bind_now             -Wl,-z,now
#   6 noexecstack          -Wl,-z,noexecstack
#   7 cf_protection        -fcf-protection=full                (x86_64 CET: IBT + shadow-stack markers)
#   8 branch_protection    -mbranch-protection=standard        (aarch64 BTI + PAC)
#   9 stl_assertions       -D_GLIBCXX_ASSERTIONS / libc++ hardening mode "fast"
#  10 guard_cf             /guard:cf                           (MSVC compile + link)
#  11 cet_compat           /CETCOMPAT                          (MSVC link, x64)
#  12 sdl                  /sdl                                (MSVC; off, see below)
#  13 dynamicbase          /DYNAMICBASE, -Wl,--dynamicbase     (PE ASLR)
#  14 nxcompat             /NXCOMPAT, -Wl,--nxcompat           (PE DEP)
#  15 high_entropy_va      /HIGHENTROPYVA, -Wl,--high-entropy-va (PE 64-bit ASLR)

include(CheckCXXSourceCompiles)
include(CMakeCheckCompilerFlagCommonPatterns)

set(FORGE_HARDENING_MASK 0)
set(FORGE_HARDENING_ENABLED "")

# Languages the compile flags apply to (not CUDA/HIP device code, which is built by custom commands anyway).
set(_fh_langs C,CXX,OBJC,OBJCXX)

set(_fh_probe_src [=[
#include <cstdio>
#include <cstring>
int main(int argc, char** argv) {
    char buf[64];
    const char* s = argc > 1 ? argv[1] : "forge";
    std::size_t n = std::strlen(s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    std::memcpy(buf, s, n);
    buf[n] = 0;
    std::puts(buf);
    return 0;
}
]=])

# _fh_probe(<var> <compile flags (string)> <link options (list)>): TRUE when the probe compiles and links
# without the toolchain complaining about the flags.
function(_fh_probe var cflags lflags)
  if(DEFINED ${var})
    return()
  endif()
  check_compiler_flag_common_patterns(_fh_fail)
  set(CMAKE_REQUIRED_FLAGS "${cflags}")
  set(CMAKE_REQUIRED_LINK_OPTIONS ${lflags})
  set(CMAKE_REQUIRED_QUIET ON)
  check_cxx_source_compiles("${_fh_probe_src}" ${var} ${_fh_fail})
  message(STATUS "FORGE_HARDENING probe ${var}: ${${var}}")
endfunction()

macro(_fh_on bit name)
  math(EXPR FORGE_HARDENING_MASK "${FORGE_HARDENING_MASK} | (1 << ${bit})")
  list(APPEND FORGE_HARDENING_ENABLED ${name})
endmacro()

if(FORGE_HARDENING)
  set(_fh_x64 OFF)
  set(_fh_arm64 OFF)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$" AND CMAKE_SIZEOF_VOID_P EQUAL 8
     AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    set(_fh_x64 ON)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    set(_fh_arm64 ON)
  endif()

  if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # ---- MSVC (cl.exe + link.exe) ----
    # /GS is cl's default; keep it explicit so a stray /GS- in someone's CXXFLAGS is overridden.
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/GS>)
    _fh_on(0 stack_protector)
    _fh_probe(FORGE_HARDEN_GUARDCF "/guard:cf" "/guard:cf")
    if(FORGE_HARDEN_GUARDCF)
      add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/guard:cf>)
      add_link_options(/guard:cf)
      _fh_on(10 guard_cf)
    endif()
    if(_fh_x64)
      _fh_probe(FORGE_HARDEN_CETCOMPAT "" "/CETCOMPAT")
      if(FORGE_HARDEN_CETCOMPAT)
        add_link_options(/CETCOMPAT)
        _fh_on(11 cet_compat)
      endif()
    endif()
    # /sdl is deliberately not used: it promotes C4996 (CRT "unsafe" deprecations: getenv, sscanf, fopen, ...) and
    # other warnings to errors (MSVC 14.44: 763 errors in 76 files) and adds member zero-initialisation on every
    # `new`, a runtime cost on the graph/script allocators. Its stack checks are /GS, kept below.
    _fh_probe(FORGE_HARDEN_PE_ASLR "" "/DYNAMICBASE;/NXCOMPAT")
    if(FORGE_HARDEN_PE_ASLR)
      add_link_options(/DYNAMICBASE /NXCOMPAT)
      _fh_on(13 dynamicbase)
      _fh_on(14 nxcompat)
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      _fh_probe(FORGE_HARDEN_HEVA "" "/HIGHENTROPYVA")
      if(FORGE_HARDEN_HEVA)
        add_link_options(/HIGHENTROPYVA)
        _fh_on(15 high_entropy_va)
      endif()
    endif()
  elseif(NOT MSVC)
    # ---- GCC / Clang (Linux, Android, FreeBSD, Apple, MinGW) ----
    # Stack protector. MinGW needs __stack_chk_fail from libssp or mingw-w64-crt >= 11: the link probe decides.
    _fh_probe(FORGE_HARDEN_SSP "-fstack-protector-strong" "-fstack-protector-strong")
    if(FORGE_HARDEN_SSP)
      add_compile_options($<$<COMPILE_LANGUAGE:${_fh_langs}>:-fstack-protector-strong>)
      add_link_options(-fstack-protector-strong)
      _fh_on(0 stack_protector)
    endif()

    # _FORTIFY_SOURCE=3 needs optimisation (glibc warns otherwise) and conflicts with ASan/TSan interceptors.
    # Only on glibc/musl Linux: Android's NDK and Apple's SDK already pick their own level, FreeBSD's libc
    # implements up to 2 and MinGW's fortify needs libssp. -U first so a toolchain default (Ubuntu gcc predefines
    # it) does not trigger a redefinition warning.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID AND NOT FORGE_SANITIZE AND NOT FORGE_TSAN)
      _fh_probe(FORGE_HARDEN_FORTIFY3 "-O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3" "")
      if(FORGE_HARDEN_FORTIFY3)
        add_compile_options(
          "$<$<AND:$<COMPILE_LANGUAGE:${_fh_langs}>,$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=3>")
        if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$" OR CMAKE_CONFIGURATION_TYPES)
          _fh_on(1 fortify_source)
        endif()
      endif()
    endif()

    if(NOT APPLE)   # Apple clang accepts the flag but ld64 / Darwin have no stack-clash guard semantics on arm64
      _fh_probe(FORGE_HARDEN_STACK_CLASH "-fstack-clash-protection" "")
      if(FORGE_HARDEN_STACK_CLASH)
        add_compile_options($<$<COMPILE_LANGUAGE:${_fh_langs}>:-fstack-clash-protection>)
        _fh_on(2 stack_clash)
      endif()
    endif()

    if(NOT WIN32 AND NOT APPLE)
      # PIE: -fPIC for libraries, -fPIE + -pie for executables (CMP0083 is NEW at cmake_minimum_required 3.25).
      include(CheckPIESupported)
      check_pie_supported(OUTPUT_VARIABLE _fh_pie_out LANGUAGES C CXX)
      set(CMAKE_POSITION_INDEPENDENT_CODE ON)
      if(CMAKE_CXX_LINK_PIE_SUPPORTED)
        _fh_on(3 pie)
      endif()
      _fh_probe(FORGE_HARDEN_RELRO "" "-Wl,-z,relro")
      if(FORGE_HARDEN_RELRO)
        add_link_options(-Wl,-z,relro)
        _fh_on(4 relro)
      endif()
      _fh_probe(FORGE_HARDEN_NOW "" "-Wl,-z,now")
      if(FORGE_HARDEN_NOW)
        add_link_options(-Wl,-z,now)
        _fh_on(5 bind_now)
      endif()
      _fh_probe(FORGE_HARDEN_NOEXECSTACK "" "-Wl,-z,noexecstack")
      if(FORGE_HARDEN_NOEXECSTACK)
        add_link_options(-Wl,-z,noexecstack)
        _fh_on(6 noexecstack)
      endif()
    elseif(APPLE)
      _fh_on(3 pie)   # ld64 links PIE executables by default (MH_PIE); W^X stacks are the platform default
    endif()

    if(_fh_x64)
      _fh_probe(FORGE_HARDEN_CET "-fcf-protection=full" "")
      if(FORGE_HARDEN_CET)
        add_compile_options($<$<COMPILE_LANGUAGE:${_fh_langs}>:-fcf-protection=full>)
        _fh_on(7 cf_protection)
      endif()
    elseif(_fh_arm64 AND NOT APPLE)
      # Not on Apple: plain arm64 Darwin's unwinder does not follow PAC-signed return addresses, so C++
      # exceptions stop being caught (pointer authentication there is the arm64e ABI instead).
      _fh_probe(FORGE_HARDEN_BTI "-mbranch-protection=standard" "")
      if(FORGE_HARDEN_BTI)
        add_compile_options($<$<COMPILE_LANGUAGE:${_fh_langs}>:-mbranch-protection=standard>)
        _fh_on(8 branch_protection)
      endif()
    endif()

    # Standard-library precondition checks (operator[] bounds, front() on empty, ...): cheap, abort on violation.
    check_cxx_source_compiles("#include <version>\n#ifndef __GLIBCXX__\n#error not libstdc++\n#endif\nint main(){return 0;}"
                              FORGE_HARDEN_IS_LIBSTDCXX)
    if(FORGE_HARDEN_IS_LIBSTDCXX)
      add_compile_options($<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-D_GLIBCXX_ASSERTIONS>)
      _fh_on(9 stl_assertions)
    elseif(NOT APPLE AND NOT ANDROID)
      # libc++ >= 18 hardening modes (FreeBSD 14.3 base, clang -stdlib=libc++). Not forced on Apple / the NDK,
      # whose libc++ builds choose their own default and where availability markup can reject it.
      _fh_probe(FORGE_HARDEN_LIBCXX_MODE "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST" "")
      check_cxx_source_compiles("#include <version>\n#if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION < 180000\n#error old\n#endif\nint main(){return 0;}"
                                FORGE_HARDEN_IS_LIBCXX18)
      if(FORGE_HARDEN_LIBCXX_MODE AND FORGE_HARDEN_IS_LIBCXX18)
        add_compile_options($<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST>)
        _fh_on(9 stl_assertions)
      endif()
    endif()

    if(WIN32)   # MinGW: binutils >= 2.36 already defaults these; be explicit for older ones.
      _fh_probe(FORGE_HARDEN_PE_ASLR "" "-Wl,--dynamicbase;-Wl,--nxcompat")
      if(FORGE_HARDEN_PE_ASLR)
        add_link_options(-Wl,--dynamicbase -Wl,--nxcompat)
        _fh_on(13 dynamicbase)
        _fh_on(14 nxcompat)
      endif()
      if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        _fh_probe(FORGE_HARDEN_HEVA "" "-Wl,--high-entropy-va")
        if(FORGE_HARDEN_HEVA)
          add_link_options(-Wl,--high-entropy-va)
          _fh_on(15 high_entropy_va)
        endif()
      endif()
    endif()
  endif()
  message(STATUS "FORGE_HARDENING: ${FORGE_HARDENING_ENABLED}")
else()
  message(STATUS "FORGE_HARDENING: OFF")
endif()

add_compile_definitions(FORGE_HARDENING_MASK=${FORGE_HARDENING_MASK})
