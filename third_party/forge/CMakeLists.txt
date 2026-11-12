cmake_minimum_required(VERSION 3.25)
project(FORGE VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# clang < 19 reports __cpp_concepts 201907L, so libstdc++ 13/14 hides std::expected (Ubuntu 24.04 clang-18 + g++-14).
# clang 18 implements what <expected> needs; raise the macro when the plain check fails.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS -std=c++23)
  check_cxx_source_compiles("#include <expected>\nint main() { std::expected<int, int> e{1}; return *e - 1; }" FORGE_STD_EXPECTED_OK)
  if(NOT FORGE_STD_EXPECTED_OK)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wno-builtin-macro-redefined;-U__cpp_concepts;-D__cpp_concepts=202002L>")
  endif()
  unset(CMAKE_REQUIRED_FLAGS)
endif()
# libc++ < 20 (FreeBSD 14 base clang, older Xcode) keeps std::jthread / std::stop_token behind -fexperimental-library.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
  include(CheckCXXSourceCompiles)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)   # compile-only: no -pthread/link needed for the probe
  set(CMAKE_REQUIRED_FLAGS -std=c++23)
  check_cxx_source_compiles("#include <stop_token>\n#include <thread>\nint main() { std::stop_source s; std::jthread t([](std::stop_token) {}); return s.stop_requested(); }" FORGE_STD_STOP_TOKEN_OK)
  if(NOT FORGE_STD_STOP_TOKEN_OK)
    set(CMAKE_REQUIRED_FLAGS "-std=c++23 -fexperimental-library")
    check_cxx_source_compiles("#include <stop_token>\n#include <thread>\nint main() { std::stop_source s; std::jthread t([](std::stop_token) {}); return s.stop_requested(); }" FORGE_STD_STOP_TOKEN_EXPERIMENTAL)
    if(FORGE_STD_STOP_TOKEN_EXPERIMENTAL)
      add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>")
      add_link_options("$<$<LINK_LANGUAGE:CXX>:-fexperimental-library>")
    endif()
  endif()
  unset(CMAKE_REQUIRED_FLAGS)
  unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
endif()
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FORGE_BUILD_TESTS "Build FORGE tests" ON)
option(FORGE_BUILD_APPS  "Build FORGE apps"  ON)
option(FORGE_SANITIZE    "Build with ASan/UBSan" OFF)
option(FORGE_TSAN        "Build with ThreadSanitizer (scripting runtime threads, task pool)" OFF)
option(FORGE_REPRODUCIBLE "Strip build paths and timestamps from artefacts (bit-identical rebuilds)" ON)
if(WIN32)
  link_libraries(ws2_32 bcrypt userenv)   # userenv: AppContainer profiles (os/sandbox.hpp)
  # Windows' default 1 MiB main-thread stack overflows the script interpreter's recursion tests; match Linux's 8 MiB.
  if(MSVC)
    add_link_options(/STACK:8388608)
    # Sources and narrow literals are UTF-8 ("\u2026", collation and charset tables); without this cl.exe
    # re-encodes them to the ANSI code page.
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
    # Do not rely on CMake's default MSVC flags (they were empty on a CMake 4.3 + VS 18 configure): without /EHsc
    # destructors do not run during unwinding (a lock_guard left a mutex locked after a throw), and without /O2 the
    # script interpreter suite ran >15 min. /bigobj for the large generated/script TUs.
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc> $<$<COMPILE_LANGUAGE:C,CXX>:/bigobj>
                        $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Release,RelWithDebInfo,MinSizeRel>>:/O2>)
  else()
    add_link_options(-Wl,--stack,8388608)
    # Win64 GCC cannot align the stack past 16 bytes, so its aligned AVX/AVX-512 spills can fault (seen on a VNNI
    # runner); the assembler then emits unaligned moves, which cost nothing on aligned data.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-Wa,-muse-unaligned-vector-move>)
    endif()
  endif()
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
option(FORGE_JNI          "Build libforge_jni (Android / JVM Kotlin bindings, bindings/android)" OFF)
if(ANDROID OR IOS OR CMAKE_SYSTEM_NAME MATCHES "iOS|tvOS|visionOS")
  set(FORGE_MOBILE ON)
endif()
if(ANDROID)
  # The NDK's libc++ gates parts of C++20/23 that FORGE uses (std::jthread / stop_token, parts of <format>,
  # <chrono> zones) behind -fexperimental-library, which also links libc++experimental.a.
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>)
  add_link_options(-fexperimental-library)
endif()
option(FORGE_LIBFUZZER   "Link fuzz targets with libFuzzer (clang -fsanitize=fuzzer) instead of the built-in driver" OFF)
set(FORGE_FUZZ_ITERS 4000 CACHE STRING "Mutated inputs per fuzz target in ctest")

if(FORGE_REPRODUCIBLE)
  if(NOT MSVC)   # cl.exe has no prefix map (it warns D9002 and ignores the flag)
    add_compile_options(-ffile-prefix-map=${PROJECT_SOURCE_DIR}=. -ffile-prefix-map=${PROJECT_BINARY_DIR}=build)
  endif()
  if(NOT APPLE AND NOT MSVC)   # ld64 and link.exe have no --build-id
    add_link_options(-Wl,--build-id=none)
  endif()
endif()
if(FORGE_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(FORGE_TSAN)
  if(FORGE_SANITIZE)
    message(FATAL_ERROR "FORGE_TSAN and FORGE_SANITIZE cannot be combined")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
# ---- build hardening (FSS-BLD-1, docs/security-standard.md) ----
# Each flag is probed and skipped when the toolchain rejects it; the result is reported by
# forge::build_hardening() (core/include/forge/core/build_info.hpp). Must stay after the sanitizer options.
option(FORGE_HARDENING "Hardening compile/link flags: stack protector, FORTIFY_SOURCE, PIE, RELRO/BIND_NOW, CET/BTI, /guard:cf" ON)
include(cmake/forge_hardening.cmake)
# ---- end build hardening ----

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ---- spec codegen: .fspec -> op table header + docs + oracle test list ----
file(GLOB FORGE_FSPECS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/ops/defs/*.fspec)
set(FORGE_GEN_DIR ${PROJECT_BINARY_DIR}/generated)
add_custom_command(
  OUTPUT ${FORGE_GEN_DIR}/forge/ops/ops.gen.hpp ${FORGE_GEN_DIR}/ops.md
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/ops/gen/fspec_gen.py
          --out ${FORGE_GEN_DIR} ${FORGE_FSPECS}
  DEPENDS ${FORGE_FSPECS} ${PROJECT_SOURCE_DIR}/ops/gen/fspec_gen.py
  COMMENT "Generating op registry from ops/defs/*.fspec")
add_custom_target(forge_ops_gen DEPENDS ${FORGE_GEN_DIR}/forge/ops/ops.gen.hpp)

# ---- Tier R: raw intrinsic wrappers generated from ops/intrinsics ----
add_custom_command(
  OUTPUT ${FORGE_GEN_DIR}/forge/simd/raw/x86.gen.hpp
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/ops/gen/intrin_gen.py
          --db ${PROJECT_SOURCE_DIR}/ops/intrinsics/x86.json
          --out ${FORGE_GEN_DIR}/forge/simd/raw/x86.gen.hpp
  DEPENDS ${PROJECT_SOURCE_DIR}/ops/intrinsics/x86.json ${PROJECT_SOURCE_DIR}/ops/gen/intrin_gen.py
  COMMENT "Generating simd Tier R (x86)")
add_custom_target(forge_raw_gen DEPENDS ${FORGE_GEN_DIR}/forge/simd/raw/x86.gen.hpp)

add_library(forge_core INTERFACE)
target_include_directories(forge_core INTERFACE core/include)
add_library(forge::core ALIAS forge_core)

add_library(forge_os INTERFACE)   # unified OS layer
target_include_directories(forge_os INTERFACE os/include)
find_package(Threads)   # FreeBSD/older glibc need -pthread for std::thread users of the OS layer
if(Threads_FOUND)
  target_link_libraries(forge_os INTERFACE Threads::Threads)
endif()
if(WIN32)
  target_link_libraries(forge_os INTERFACE ws2_32 bcrypt advapi32)   # advapi32: fs.hpp check_private / private DACLs
elseif(UNIX AND NOT APPLE)
  target_link_libraries(forge_os INTERFACE ${CMAKE_DL_LIBS})
endif()
target_link_libraries(forge_os INTERFACE forge_trust)   # keystore.hpp file backend: Monocypher AEAD (forge_trust is defined below)
target_link_libraries(forge_core INTERFACE forge_os)
if(WIN32)
  target_link_libraries(forge_core INTERFACE advapi32)   # log.hpp: RegisterEventSourceW / ReportEventW
endif()
# Compile-time log strip (core/log.hpp): calls below this level compile to nothing in every target linking forge_core.
set(FORGE_LOG_MIN_LEVEL "none" CACHE STRING "Strip log calls below this level at compile time: trace debug info warn error fatal off, or none")
set_property(CACHE FORGE_LOG_MIN_LEVEL PROPERTY STRINGS none trace debug info warn error fatal off)
include(cmake/forge_log_level.cmake)
forge_log_level_value("${FORGE_LOG_MIN_LEVEL}" _forge_log_min)
if(_forge_log_min STREQUAL "INVALID")
  message(FATAL_ERROR "FORGE_LOG_MIN_LEVEL='${FORGE_LOG_MIN_LEVEL}': use none, trace, debug, info, warn, error, fatal or off")
elseif(NOT _forge_log_min STREQUAL "")
  target_compile_definitions(forge_core INTERFACE FORGE_LOG_MIN_LEVEL=${_forge_log_min})
  message(STATUS "FORGE: log calls below '${FORGE_LOG_MIN_LEVEL}' are stripped (FORGE_LOG_MIN_LEVEL=${_forge_log_min})")
endif()

add_library(forge_loadbalancer INTERFACE)   # load balancer + L4/L7 proxy
target_include_directories(forge_loadbalancer INTERFACE loadbalancer/include)
target_link_libraries(forge_loadbalancer INTERFACE forge_os)
find_package(Threads)
if(Threads_FOUND)
  target_link_libraries(forge_loadbalancer INTERFACE Threads::Threads)
endif()

# forge::os::mobile — sensors, touch, haptics, display, cameras, permissions (docs/mobile.md).
set(FORGE_MOBILE_SRCS os/src/mobile/mobile.cpp os/src/mobile/linux.cpp os/src/mobile/android.cpp
  os/src/mobile/services.cpp os/src/mobile/windows.cpp os/src/mobile/freebsd.cpp)
if(APPLE)
  enable_language(OBJCXX)
  list(APPEND FORGE_MOBILE_SRCS os/src/mobile/apple.mm)
endif()
add_library(forge_mobile STATIC ${FORGE_MOBILE_SRCS})
target_include_directories(forge_mobile PUBLIC os/include core/include support/include)
target_link_libraries(forge_mobile PUBLIC forge_core)
if(Threads_FOUND)
  target_link_libraries(forge_mobile PUBLIC Threads::Threads)
endif()
set_target_properties(forge_mobile PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(ANDROID)
  target_link_libraries(forge_mobile PUBLIC android camera2ndk ${CMAKE_DL_LIBS})
elseif(WIN32)
  target_link_libraries(forge_mobile PUBLIC shell32 user32)
elseif(APPLE)
  set_source_files_properties(os/src/mobile/apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_link_libraries(forge_mobile PUBLIC "-framework UIKit" "-framework CoreMotion" "-framework CoreLocation" "-framework AVFoundation"
      "-framework AudioToolbox" "-framework UserNotifications" "-framework LocalAuthentication" "-framework Foundation")
  else()
    # macOS: AVFoundation (camera enumeration/permission), CoreLocation (location), LocalAuthentication (Touch ID)
    target_link_libraries(forge_mobile PUBLIC "-framework Foundation" "-framework AppKit" "-framework IOKit" "-framework UserNotifications"
      "-framework AVFoundation" "-framework CoreLocation" "-framework LocalAuthentication")
  endif()
endif()

# forge::os::text / forge::os::locale: FORGE's own Unicode, encodings and locale library (docs/text-locale.md).
file(GLOB FORGE_TEXT_SRCS CONFIGURE_DEPENDS os/src/text/*.cpp)
if(APPLE)
  enable_language(OBJCXX)
  list(APPEND FORGE_TEXT_SRCS os/src/text/locale_apple.mm)
  set_source_files_properties(os/src/text/locale_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
add_library(forge_text STATIC ${FORGE_TEXT_SRCS})
target_include_directories(forge_text PUBLIC os/include core/include support/include)
target_link_libraries(forge_text PUBLIC forge_core)
set_target_properties(forge_text PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(APPLE)
  target_link_libraries(forge_text PUBLIC "-framework CoreFoundation" "-framework Foundation")
endif()
option(FORGE_FETCH_UCD "Download the Unicode conformance test files for test_text_conformance" OFF)
if(FORGE_FETCH_UCD)
  include(cmake/fetch_ucd.cmake)
endif()

add_library(forge_ops INTERFACE)
target_include_directories(forge_ops INTERFACE ops/include ${FORGE_GEN_DIR})
target_link_libraries(forge_ops INTERFACE forge_core)
add_dependencies(forge_ops forge_ops_gen)

add_library(forge_simd INTERFACE)
target_include_directories(forge_simd INTERFACE simd/include ${FORGE_GEN_DIR})
target_link_libraries(forge_simd INTERFACE forge_core)
add_dependencies(forge_simd forge_raw_gen)

add_library(forge_verify INTERFACE)
target_include_directories(forge_verify INTERFACE verify/include)
target_link_libraries(forge_verify INTERFACE forge_ops)


# ---- GPU kernels generated from spec/ops (FORGE_PLAN B.4): GLSL -> SPIR-V (embedded), CUDA/HIP compile checks ----
set(FORGE_GPU_DIR ${FORGE_GEN_DIR}/gpu)
execute_process(COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/ops/gen/gpu_gen.py --ops ${FORGE_FSPECS} --out ${FORGE_GPU_DIR} --list
                OUTPUT_VARIABLE FORGE_GPU_KERNELS OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _gpu_rc)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/ops/gen/gpu_gen.py)
find_program(FORGE_GLSLANG glslangValidator)
find_program(FORGE_SPIRV_VAL spirv-val)
find_path(FORGE_VULKAN_INCLUDE vulkan/vulkan.h)
set(FORGE_GPU_SOURCES)
foreach(k ${FORGE_GPU_KERNELS})
  list(APPEND FORGE_GPU_SOURCES ${FORGE_GPU_DIR}/${k}.comp)
endforeach()
add_custom_command(OUTPUT ${FORGE_GPU_SOURCES} ${FORGE_GPU_DIR}/kernels.cu ${FORGE_GPU_DIR}/kernels.cl ${FORGE_GPU_DIR}/kernels_cl_mock.inc ${FORGE_GPU_DIR}/kernels.metal
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/ops/gen/gpu_gen.py --ops ${FORGE_FSPECS} --out ${FORGE_GPU_DIR}
  DEPENDS ${FORGE_FSPECS} ${PROJECT_SOURCE_DIR}/ops/gen/gpu_gen.py COMMENT "Generating GPU kernels from spec/ops")
if(_gpu_rc EQUAL 0 AND FORGE_GLSLANG AND FORGE_VULKAN_INCLUDE)
  set(FORGE_SPV)
  foreach(k ${FORGE_GPU_KERNELS})
    set(_val "")
    if(FORGE_SPIRV_VAL)
      set(_val COMMAND ${FORGE_SPIRV_VAL} --target-env vulkan1.1 ${FORGE_GPU_DIR}/${k}.spv)
    endif()
    add_custom_command(OUTPUT ${FORGE_GPU_DIR}/${k}.spv
      COMMAND ${FORGE_GLSLANG} -V --target-env vulkan1.1 -o ${FORGE_GPU_DIR}/${k}.spv ${FORGE_GPU_DIR}/${k}.comp ${_val}
      DEPENDS ${FORGE_GPU_DIR}/${k}.comp COMMENT "SPIR-V ${k}")
    list(APPEND FORGE_SPV ${FORGE_GPU_DIR}/${k}.spv)
  endforeach()
  add_custom_command(OUTPUT ${FORGE_GEN_DIR}/forge/gpu/spirv.gen.hpp
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/ops/gen/gpu_gen.py --ops ${FORGE_FSPECS} --out ${FORGE_GPU_DIR}
            --embed ${FORGE_GEN_DIR}/forge/gpu/spirv.gen.hpp
    DEPENDS ${FORGE_SPV} ${PROJECT_SOURCE_DIR}/ops/gen/gpu_gen.py COMMENT "Embedding SPIR-V kernels")
  add_custom_target(forge_gpu_gen ALL DEPENDS ${FORGE_GEN_DIR}/forge/gpu/spirv.gen.hpp)
  # Copy only the Vulkan headers so the system include dir is not put ahead of the C++ standard library.
  file(COPY ${FORGE_VULKAN_INCLUDE}/vulkan DESTINATION ${FORGE_GEN_DIR}/vkinc)
  if(EXISTS ${FORGE_VULKAN_INCLUDE}/vk_video)
    file(COPY ${FORGE_VULKAN_INCLUDE}/vk_video DESTINATION ${FORGE_GEN_DIR}/vkinc)
  endif()
  set(FORGE_HAVE_VULKAN ON)
  message(STATUS "FORGE: Vulkan backend enabled (${FORGE_GLSLANG})")
else()
  add_custom_target(forge_gpu_gen ALL DEPENDS ${FORGE_GPU_DIR}/kernels.cu)
  message(STATUS "FORGE: Vulkan backend disabled (need glslangValidator + vulkan/vulkan.h)")
endif()
# CUDA / HIP kernel images, built from the generated kernels.cu with clang (no SDK) and embedded for the
# dlopen-based host runtime (gpu/cuda.hpp). HIP code objects need lld; zig's bundled ld.lld works too.
# When the host compiler is clang, the GPU images use that same toolchain (one LLVM for host and device);
# otherwise the first clang on PATH. Override with -DFORGE_GPU_CLANG=... / -DFORGE_LLD=...
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
  get_filename_component(_host_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(FORGE_GPU_CLANG NAMES clang PATHS ${_host_llvm_bin} NO_DEFAULT_PATH)
  find_program(FORGE_LLD NAMES ld.lld lld PATHS ${_host_llvm_bin} NO_DEFAULT_PATH)
endif()
find_program(FORGE_GPU_CLANG NAMES clang-19 clang-18 clang-20 clang)
find_program(FORGE_LLD NAMES ld.lld ld.lld-19 ld.lld-18 lld)
if(NOT FORGE_LLD)
  find_program(FORGE_ZIG NAMES zig)
  if(NOT FORGE_ZIG)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import ziglang, os; print(os.path.join(os.path.dirname(ziglang.__file__), 'zig'))"
                    OUTPUT_VARIABLE _zig OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _zr)
    if(_zr EQUAL 0 AND EXISTS "${_zig}")
      set(FORGE_ZIG ${_zig} CACHE FILEPATH "zig (for ld.lld)" FORCE)
    endif()
  endif()
endif()
set(FORGE_HIP_ARCHS gfx90a gfx942 gfx1100 CACHE STRING "AMDGPU targets embedded for the HIP runtime")
set(FORGE_GPU_IMAGES ${FORGE_GPU_DIR}/kernels.cl ${FORGE_GPU_DIR}/kernels.metal)
if(FORGE_MOBILE)   # no CUDA/HIP/Level Zero on phones; the NDK's lld can't link AMDGPU either
  set(FORGE_GPU_CLANG_IMAGES OFF)
else()
  set(FORGE_GPU_CLANG_IMAGES ${FORGE_GPU_CLANG})
endif()   # OpenCL C, built by the driver at run time
# Distro clangs (e.g. FreeBSD base clang) may be built without the NVPTX/AMDGPU backends.
set(FORGE_GPU_HAS_NVPTX OFF)
set(FORGE_GPU_HAS_AMDGPU OFF)
if(FORGE_GPU_CLANG_IMAGES)
  execute_process(COMMAND ${FORGE_GPU_CLANG} -print-targets OUTPUT_VARIABLE _gpu_targets ERROR_QUIET)
  if(_gpu_targets MATCHES "nvptx64")
    set(FORGE_GPU_HAS_NVPTX ON)
  endif()
  if(_gpu_targets MATCHES "amdgcn")
    set(FORGE_GPU_HAS_AMDGPU ON)
  endif()
  if(NOT FORGE_GPU_HAS_NVPTX OR NOT FORGE_GPU_HAS_AMDGPU)
    message(STATUS "FORGE: ${FORGE_GPU_CLANG} lacks GPU backends (nvptx=${FORGE_GPU_HAS_NVPTX} amdgpu=${FORGE_GPU_HAS_AMDGPU}); skipping those images")
  endif()
endif()
# Level Zero wants SPIR-V: OpenCL C -> LLVM IR (clang, spir64) -> SPIR-V (llvm-spirv), when the translator exists.
find_program(FORGE_LLVM_SPIRV NAMES llvm-spirv llvm-spirv-19 llvm-spirv-18 llvm-spirv-20)
if(FORGE_GPU_CLANG_IMAGES AND FORGE_LLVM_SPIRV)
  add_custom_command(OUTPUT ${FORGE_GPU_DIR}/kernels.spv
    COMMAND ${FORGE_GPU_CLANG} -x cl -cl-std=CL1.2 --target=spir64 -O2 -emit-llvm -c -o ${FORGE_GPU_DIR}/kernels.cl.bc ${FORGE_GPU_DIR}/kernels.cl
    COMMAND ${FORGE_LLVM_SPIRV} ${FORGE_GPU_DIR}/kernels.cl.bc -o ${FORGE_GPU_DIR}/kernels.spv
    DEPENDS ${FORGE_GPU_DIR}/kernels.cl COMMENT "SPIR-V for Level Zero")
  list(APPEND FORGE_GPU_IMAGES ${FORGE_GPU_DIR}/kernels.spv)
endif()
if(FORGE_GPU_CLANG_IMAGES AND FORGE_GPU_HAS_NVPTX)
  add_custom_command(OUTPUT ${FORGE_GPU_DIR}/kernels.ptx
    COMMAND ${FORGE_GPU_CLANG} -x cuda --cuda-device-only -nocudainc -nocudalib -Wno-unknown-cuda-version --cuda-gpu-arch=sm_70 -O3 -Werror
            -S -o ${FORGE_GPU_DIR}/kernels.ptx ${FORGE_GPU_DIR}/kernels.cu
    DEPENDS ${FORGE_GPU_DIR}/kernels.cu COMMENT "PTX (sm_70)")
  list(APPEND FORGE_GPU_IMAGES ${FORGE_GPU_DIR}/kernels.ptx)
  # sm_80+ image: the same kernels plus the int8 tensor-core prefill matmul; the runtime tries it first.
  add_custom_command(OUTPUT ${FORGE_GPU_DIR}/kernels.sm80.ptx
    COMMAND ${FORGE_GPU_CLANG} -x cuda --cuda-device-only -nocudainc -nocudalib -Wno-unknown-cuda-version --cuda-gpu-arch=sm_80 -O3 -Werror
            -S -o ${FORGE_GPU_DIR}/kernels.sm80.ptx ${FORGE_GPU_DIR}/kernels.cu
    DEPENDS ${FORGE_GPU_DIR}/kernels.cu COMMENT "PTX (sm_80)")
  list(APPEND FORGE_GPU_IMAGES ${FORGE_GPU_DIR}/kernels.sm80.ptx)
endif()
if(FORGE_GPU_CLANG_IMAGES AND FORGE_GPU_HAS_AMDGPU)
  set(_ld)
  if(FORGE_LLD)
    get_filename_component(_lldir ${FORGE_LLD} DIRECTORY)
    set(_ld --ld-path=${FORGE_LLD})
    set(_ldpath ${_lldir})
  elseif(FORGE_ZIG)
    # clang only accepts an --ld-path for AMDGPU whose file is named like lld
    file(COPY ${PROJECT_SOURCE_DIR}/cmake/tools/zig/lld DESTINATION ${PROJECT_BINARY_DIR}/tools
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
    set(_ld --ld-path=${PROJECT_BINARY_DIR}/tools/lld)
    set(_ldpath ${PROJECT_BINARY_DIR}/tools)   # clang runs "lld" from PATH for the LTO link at -O3
  endif()
  if(_ld)
    foreach(arch ${FORGE_HIP_ARCHS})
      add_custom_command(OUTPUT ${FORGE_GPU_DIR}/kernels.${arch}.hsaco
        COMMAND ${CMAKE_COMMAND} -E env FORGE_ZIG=${FORGE_ZIG} "PATH=${_ldpath}:$ENV{PATH}" ${FORGE_GPU_CLANG} -x hip --cuda-device-only --no-gpu-bundle-output
                -nogpuinc -nogpulib -cuid=forge_kernels --offload-arch=${arch} -O3 -Werror ${_ld} -o ${FORGE_GPU_DIR}/kernels.${arch}.hsaco ${FORGE_GPU_DIR}/kernels.cu
        DEPENDS ${FORGE_GPU_DIR}/kernels.cu COMMENT "HIP code object (${arch})" VERBATIM)   # PATH may hold "(x86)" (WSL)
      list(APPEND FORGE_GPU_IMAGES ${FORGE_GPU_DIR}/kernels.${arch}.hsaco)
    endforeach()
  endif()
endif()
add_custom_command(OUTPUT ${FORGE_GEN_DIR}/forge/gpu/gpu_images.gen.hpp
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/ops/gen/gpu_gen.py --ops ${FORGE_FSPECS} --out ${FORGE_GPU_DIR}
          --embed-images ${FORGE_GEN_DIR}/forge/gpu/gpu_images.gen.hpp ${FORGE_GPU_IMAGES}
  DEPENDS ${FORGE_GPU_IMAGES} ${PROJECT_SOURCE_DIR}/ops/gen/gpu_gen.py COMMENT "Embedding CUDA/HIP kernel images")
add_custom_target(forge_gpu_images ALL DEPENDS ${FORGE_GEN_DIR}/forge/gpu/gpu_images.gen.hpp)
add_dependencies(forge_gpu_images forge_gpu_gen)
message(STATUS "FORGE: CUDA/HIP images: ${FORGE_GPU_IMAGES}")
add_library(forge_gpu INTERFACE)
target_include_directories(forge_gpu INTERFACE gpu/include support/include third_party)
target_include_directories(forge_gpu INTERFACE ${FORGE_GEN_DIR})
if(FORGE_HAVE_VULKAN)
  target_include_directories(forge_gpu INTERFACE ${FORGE_GEN_DIR}/vkinc)
endif()
add_dependencies(forge_gpu forge_gpu_gen forge_gpu_images)
if(APPLE)   # Metal runtime (Objective-C++); HARDWARE-UNTESTED
  enable_language(OBJCXX)
  add_library(forge_metal STATIC gpu/src/metal.mm)
  target_compile_options(forge_metal PRIVATE -fobjc-arc)
  target_compile_definitions(forge_metal PUBLIC FORGE_GPU_METAL=1)
  target_include_directories(forge_metal PUBLIC gpu/include support/include third_party ${FORGE_GEN_DIR} core/include os/include ops/include simd/include)
  add_dependencies(forge_metal forge_gpu_gen forge_gpu_images)
  target_link_libraries(forge_metal PUBLIC "-framework Metal" "-framework Foundation")
  target_link_libraries(forge_gpu INTERFACE forge_metal)
endif()
target_link_libraries(forge_gpu INTERFACE forge_simd)

add_library(forge_graph INTERFACE)
target_include_directories(forge_graph INTERFACE graph/include)
target_link_libraries(forge_graph INTERFACE forge_ops forge_simd forge_gpu forge_trust forge_security)   # security: rpc key memory (CRY-2), key file checks (FIO-2)
# TLS for rpc-serve (graph/rpc.hpp). OpenSSL is optional; without it rpc stays plain TCP (auth still works).
option(FORGE_TLS "TLS for forge rpc-serve via OpenSSL when available" ON)
if(FORGE_TLS)
  # Pass -DOPENSSL_ROOT_DIR=<prefix> when OpenSSL lives outside the toolchain's sysroot.
  find_package(OpenSSL QUIET)
  if(OpenSSL_FOUND)
    # Opt-in per target (forge, forge_script, test_rpc): rpc.hpp uses an ABI-tagged inline namespace, so
    # TLS and plain builds of it can coexist in one program without ODR clashes.
    add_library(forge_rpc_tls INTERFACE)
    target_link_libraries(forge_rpc_tls INTERFACE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(forge_rpc_tls INTERFACE FORGE_HAVE_TLS=1)
    message(STATUS "FORGE: rpc TLS via OpenSSL ${OPENSSL_VERSION}")
  else()
    message(STATUS "FORGE: OpenSSL not found; rpc TLS disabled")
  endif()
endif()

# Vendored Monocypher 4.0.2 (BSD-2-Clause / CC0): Ed25519 for .fpk signing, XChaCha20-Poly1305 for the os keystore file backend.
add_library(forge_trust STATIC third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c)
target_include_directories(forge_trust PUBLIC third_party/monocypher)
set_target_properties(forge_trust PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)

add_library(forge_modelfile INTERFACE)
target_include_directories(forge_modelfile INTERFACE modelfile/include)
target_link_libraries(forge_modelfile INTERFACE forge_graph forge_trust)

# forge::security: FORGE Security Standard policy, enforce, audit, lockdown, check (docs/security-api.md). Header-only.
add_library(forge_security INTERFACE)
target_include_directories(forge_security INTERFACE security/include support/include)
target_link_libraries(forge_security INTERFACE forge_core forge_trust)   # forge_trust: Ed25519 for detached signatures

add_library(forge_inference INTERFACE)
target_include_directories(forge_inference INTERFACE inference/include)
target_link_libraries(forge_inference INTERFACE forge_modelfile forge_security)   # FSS model integrity / fs scope at load
target_include_directories(forge_inference INTERFACE inference/include)
# forge::script managed scripting runtime (docs/scripting.md). Safe C++23; libc++ hardening when available.
# Shell backends: one file each in script/src/shells/ defining forge_shell_register_<stem>(); the registry TU is generated.
file(GLOB FORGE_SHELL_SRCS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/script/src/shells/*.cpp)
set(_shell_decl "") 
set(_shell_call "")
foreach(f ${FORGE_SHELL_SRCS})
  get_filename_component(_stem ${f} NAME_WE)
  string(APPEND _shell_decl "void forge_shell_register_${_stem}(forge::script::shell_registry&);\n")
  string(APPEND _shell_call "        forge_shell_register_${_stem}(r);\n")
endforeach()
file(CONFIGURE OUTPUT ${PROJECT_BINARY_DIR}/generated/shells_registry.cpp CONTENT
"// generated by CMake from script/src/shells/*.cpp\n#include \"shells/shell.hpp\"\n${_shell_decl}namespace forge::script {\nconst shell_registry& shells() {\n    static const shell_registry reg = [] {\n        shell_registry r;\n${_shell_call}        return r;\n    }();\n    return reg;\n}\n}\n")
add_library(forge_script STATIC ${FORGE_SHELL_SRCS} ${PROJECT_BINARY_DIR}/generated/shells_registry.cpp script/src/heap.cpp script/src/parser.cpp script/src/runtime.cpp script/src/tree.cpp
  script/src/vm.cpp script/src/builtins.cpp script/src/cppstd.cpp script/src/automation.cpp script/src/ai.cpp script/src/remote.cpp script/src/mobile.cpp script/src/text.cpp script/src/reflect.cpp script/src/emit.cpp script/src/debug.cpp script/src/engine.cpp script/src/fsc.cpp script/src/check.cpp)
target_include_directories(forge_script PUBLIC script/include PRIVATE script/src)
target_link_libraries(forge_script PUBLIC forge_modelfile forge_mobile forge_text PRIVATE forge_verify forge_trust forge_security)
if(TARGET forge_rpc_tls)
  target_link_libraries(forge_script PUBLIC forge_rpc_tls)
endif()
find_package(Threads)
if(Threads_FOUND)
  target_link_libraries(forge_script PUBLIC Threads::Threads)
endif()
target_compile_options(forge_script PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra>)
set_target_properties(forge_script PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Example native plugin (C ABI in script/include/forge/script/plugin.h); also used by test_script.
add_library(forge_script_example_plugin MODULE examples/script_plugin/hello_plugin.c)
target_include_directories(forge_script_example_plugin PRIVATE script/include)
set_target_properties(forge_script_example_plugin PROPERTIES OUTPUT_NAME hello_plugin C_VISIBILITY_PRESET hidden)
add_library(forge_c SHARED inference/capi/forge_c.cpp)
target_link_libraries(forge_c PRIVATE forge_inference forge_verify forge_simd_mv)
target_compile_definitions(forge_c PRIVATE FORGE_C_BUILD)
if(MSVC)   # forge.exe and forge.dll would both link to forge.pdb
  set_target_properties(forge_c PROPERTIES PDB_NAME forge_c)
endif()
set_target_properties(forge_c PROPERTIES OUTPUT_NAME forge VERSION ${PROJECT_VERSION} SOVERSION 1 CXX_VISIBILITY_PRESET default)
if(FORGE_JNI)
  find_package(JNI QUIET)
  add_library(forge_jni SHARED bindings/android/jni/forge_jni.c)
  target_include_directories(forge_jni PRIVATE inference/include)
  if(NOT ANDROID AND JNI_FOUND)
    target_include_directories(forge_jni PRIVATE ${JNI_INCLUDE_DIRS})
  endif()
  target_link_libraries(forge_jni PRIVATE forge_c)
  if(ANDROID)
    target_link_libraries(forge_jni PRIVATE log)
  endif()
endif()
target_compile_definitions(forge_inference INTERFACE FORGE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# Multiversioned simd kernels: one TU per target, own flags, own namespace.
include(CheckCXXSourceCompiles)
# First flag set that compiles a fixed-length (256-bit) scalable-vector probe: GCC/Clang spelling, then zig's.
function(forge_probe_flags out header type attr)
  foreach(cand IN LISTS ARGN)
    string(REPLACE "|" ";" _fl "${cand}")
    set(CMAKE_REQUIRED_FLAGS "${cand}")
    string(REPLACE "|" " " CMAKE_REQUIRED_FLAGS "${cand}")
    unset(_ok CACHE)
    check_cxx_source_compiles("#include <${header}>\ntypedef ${type} t __attribute__((${attr}));\nstatic_assert(sizeof(t)*8==${FORGE_PROBE_BITS});\nint main(){return 0;}" _ok)
    if(_ok)
      set(${out} "${_fl}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out} "" PARENT_SCOPE)
endfunction()

# Scalable vectors: one fixed-length kernel table per VL (128..1024 bits); dispatch picks the hardware's.
set(FORGE_SCALABLE_BITS 128 256 512 1024)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64" OR (APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "arm64"))
  set(FORGE_ARCH aarch64)
  add_library(forge_simd_mv STATIC simd/src/kernels_scalar.cpp simd/src/kernels_neon.cpp simd/src/dispatch.cpp)
  set(_defs)
  foreach(b ${FORGE_SCALABLE_BITS})
    set(FORGE_PROBE_BITS ${b})
    forge_probe_flags(FORGE_SVE${b}_FLAGS arm_sve.h svfloat32_t "arm_sve_vector_bits(${b})"
      "-march=armv8.2-a+sve|-msve-vector-bits=${b}" "-mcpu=generic+sve|-msve-vector-bits=${b}")
    if(FORGE_SVE${b}_FLAGS)
      add_library(forge_simd_sve${b} OBJECT simd/src/kernels_sve.cpp)
      target_compile_options(forge_simd_sve${b} PRIVATE ${FORGE_SVE${b}_FLAGS})
      set_target_properties(forge_simd_sve${b} PROPERTIES POSITION_INDEPENDENT_CODE ON)   # linked into libforge.so
      target_link_libraries(forge_simd_sve${b} PRIVATE forge_simd)
      target_sources(forge_simd_mv PRIVATE $<TARGET_OBJECTS:forge_simd_sve${b}>)
      list(APPEND _defs FORGE_SIMD_SVE${b}=1)
    endif()
  endforeach()
  set_source_files_properties(simd/src/dispatch.cpp PROPERTIES COMPILE_DEFINITIONS "${_defs}")
  message(STATUS "FORGE: SVE kernel tables: ${_defs}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
  set(FORGE_ARCH riscv64)
  add_library(forge_simd_mv STATIC simd/src/kernels_scalar.cpp simd/src/dispatch.cpp)
  set(_defs)
  foreach(b ${FORGE_SCALABLE_BITS})
    set(FORGE_PROBE_BITS ${b})
    forge_probe_flags(FORGE_RVV${b}_FLAGS riscv_vector.h vfloat32m1_t "riscv_rvv_vector_bits(${b})"
      "-march=rv64gcv_zvl${b}b|-mrvv-vector-bits=${b}" "-mcpu=baseline_rv64+v+zvl${b}b|-mrvv-vector-bits=${b}")
    if(FORGE_RVV${b}_FLAGS)
      add_library(forge_simd_rvv${b} OBJECT simd/src/kernels_rvv.cpp)
      target_compile_options(forge_simd_rvv${b} PRIVATE ${FORGE_RVV${b}_FLAGS})
      set_target_properties(forge_simd_rvv${b} PROPERTIES POSITION_INDEPENDENT_CODE ON)   # linked into libforge.so
      target_link_libraries(forge_simd_rvv${b} PRIVATE forge_simd)
      target_sources(forge_simd_mv PRIVATE $<TARGET_OBJECTS:forge_simd_rvv${b}>)
      list(APPEND _defs FORGE_SIMD_RVV${b}=1)
    endif()
  endforeach()
  set_source_files_properties(simd/src/dispatch.cpp PROPERTIES COMPILE_DEFINITIONS "${_defs}")
  message(STATUS "FORGE: RVV kernel tables: ${_defs}")
else()
  set(FORGE_ARCH x86_64)
  add_library(forge_simd_mv STATIC
    simd/src/kernels_scalar.cpp
    simd/src/kernels_avx2.cpp
    simd/src/kernels_avx512.cpp
    simd/src/dispatch.cpp)
  if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(simd/src/kernels_avx2.cpp   PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    set_source_files_properties(simd/src/kernels_avx512.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
  else()
    set_source_files_properties(simd/src/kernels_avx2.cpp   PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma;-mf16c")
    set_source_files_properties(simd/src/kernels_avx512.cpp PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512bw;-mavx512dq;-mavx512vl;-mfma")
  endif()
  # Integer matvec kernels (simd/qdot_i8.hpp): AVX2 always, AVX-VNNI / AVX-512 VNNI when the compiler has them.
  target_sources(forge_simd_mv PRIVATE simd/src/qi8_avx2.cpp simd/src/qi8_avxvnni.cpp simd/src/qi8_avx512vnni.cpp)
  set(_qi8_defs)
  if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(simd/src/qi8_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    set_source_files_properties(simd/src/qi8_avxvnni.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2" COMPILE_DEFINITIONS FORGE_QI8_HAVE_AVXVNNI=1)
    set_source_files_properties(simd/src/qi8_avx512vnni.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX512" COMPILE_DEFINITIONS FORGE_QI8_HAVE_AVX512VNNI=1)
    list(APPEND _qi8_defs FORGE_QI8_HAVE_AVXVNNI=1 FORGE_QI8_HAVE_AVX512VNNI=1)
  else()
    include(CheckCXXSourceCompiles)
    set(_qi8_v3 "-mavx2;-mfma;-mf16c;-mbmi2;-ffp-contract=off")   # no fused multiply-adds: every path gives the scalar reference's bits
    set_source_files_properties(simd/src/qi8_avx2.cpp PROPERTIES COMPILE_OPTIONS "${_qi8_v3}")
    set(CMAKE_REQUIRED_FLAGS "-mavx2 -mfma -mf16c -mavxvnni")
    check_cxx_source_compiles("#include <immintrin.h>\nint main(){__m256i a=_mm256_setzero_si256();a=_mm256_dpbusd_avx_epi32(a,a,a);return _mm256_extract_epi32(a,0);}" FORGE_CAN_AVXVNNI)
    set(CMAKE_REQUIRED_FLAGS "-mavx2 -mfma -mf16c -mavx512f -mavx512bw -mavx512vl -mavx512vnni")
    check_cxx_source_compiles("#include <immintrin.h>\nint main(){__m256i a=_mm256_setzero_si256();a=_mm256_dpbusd_epi32(a,a,a);return _mm256_extract_epi32(a,0);}" FORGE_CAN_AVX512VNNI)
    unset(CMAKE_REQUIRED_FLAGS)
    if(FORGE_CAN_AVXVNNI)
      set_source_files_properties(simd/src/qi8_avxvnni.cpp PROPERTIES COMPILE_OPTIONS "${_qi8_v3};-mavxvnni" COMPILE_DEFINITIONS FORGE_QI8_HAVE_AVXVNNI=1)
      list(APPEND _qi8_defs FORGE_QI8_HAVE_AVXVNNI=1)
    endif()
    if(FORGE_CAN_AVX512VNNI)
      set_source_files_properties(simd/src/qi8_avx512vnni.cpp PROPERTIES COMPILE_OPTIONS "${_qi8_v3};-mavx512f;-mavx512bw;-mavx512vl;-mavx512vnni" COMPILE_DEFINITIONS FORGE_QI8_HAVE_AVX512VNNI=1)
      list(APPEND _qi8_defs FORGE_QI8_HAVE_AVX512VNNI=1)
    endif()
  endif()
  set_source_files_properties(simd/src/qi8_dispatch.cpp PROPERTIES COMPILE_DEFINITIONS "${_qi8_defs}")
  message(STATUS "FORGE: integer matvec kernels: avx2 ${_qi8_defs}")
endif()
target_sources(forge_simd_mv PRIVATE simd/src/qi8_dispatch.cpp)
target_link_libraries(forge_simd_mv PUBLIC forge_simd)
set_target_properties(forge_simd_mv PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Matrix tile units (simd/tile.hpp): AMX on x86_64, SME on aarch64. Each file stubs out when its unit can't be compiled.
include(CheckCXXSourceCompiles)
target_sources(forge_simd_mv PRIVATE simd/src/tile_amx.cpp simd/src/tile_sme.cpp)
if(FORGE_ARCH STREQUAL "x86_64")
  set(CMAKE_REQUIRED_FLAGS "-mamx-tile -mamx-bf16 -mamx-int8")
  check_cxx_source_compiles("#include <immintrin.h>\nint main(){_tile_zero(0);_tile_release();return 0;}" FORGE_CAN_AMX)
  unset(CMAKE_REQUIRED_FLAGS)
  if(FORGE_CAN_AMX)
    set_source_files_properties(simd/src/tile_amx.cpp PROPERTIES COMPILE_OPTIONS "-mamx-tile;-mamx-bf16;-mamx-int8" COMPILE_DEFINITIONS FORGE_HAVE_AMX=1)
  endif()
elseif(FORGE_ARCH STREQUAL "aarch64")
  foreach(_f "-march=armv9-a+sme" "-mcpu=generic+sme")
    set(CMAKE_REQUIRED_FLAGS "${_f}")
    unset(FORGE_CAN_SME CACHE)
    # Compile-only: some runtimes (zig/musl, older libgcc) lack the SME ABI support routines; tile_sme.cpp
    # carries weak fallbacks for those.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    check_cxx_source_compiles("#include <arm_sme.h>\n__arm_new(\"za\") void f() __arm_streaming { svzero_za(); }\nint main(){return 0;}" FORGE_CAN_SME)
    unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
    unset(CMAKE_REQUIRED_FLAGS)
    if(FORGE_CAN_SME)
      set_source_files_properties(simd/src/tile_sme.cpp PROPERTIES COMPILE_OPTIONS "${_f}" COMPILE_DEFINITIONS FORGE_HAVE_SME=1)
      break()
    endif()
  endforeach()
endif()
message(STATUS "FORGE: tile units AMX=${FORGE_CAN_AMX} SME=${FORGE_CAN_SME}")
# Sequence-mixing kernels (simd/seq.hpp: chunked attention, gated delta rule): scalar everywhere, AVX2 and
# AVX-512 tables on x86_64. No contraction of a*b+c anywhere, so every table returns identical bits.
target_sources(forge_simd_mv PRIVATE simd/src/seq_scalar.cpp simd/src/seq_dispatch.cpp)
if(FORGE_ARCH STREQUAL "x86_64")
  target_sources(forge_simd_mv PRIVATE simd/src/seq_avx2.cpp simd/src/seq_avx512.cpp)
endif()
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(simd/src/seq_scalar.cpp PROPERTIES COMPILE_OPTIONS "/fp:precise")
  set_source_files_properties(simd/src/seq_avx2.cpp   PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/fp:precise")
  set_source_files_properties(simd/src/seq_avx512.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX512;/fp:precise")
else()
  set_source_files_properties(simd/src/seq_scalar.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
  set_source_files_properties(simd/src/seq_avx2.cpp   PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma;-mf16c;-ffp-contract=off")
  set_source_files_properties(simd/src/seq_avx512.cpp PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512bw;-mavx512dq;-mavx512vl;-mfma;-ffp-contract=off")
endif()

# Hardware video decode (vision/video.hpp): NVDEC when nvcuvid.h is available, VideoToolbox on Apple, MediaCodec on Android.
add_library(forge_video STATIC support/src/video.cpp)
target_link_libraries(forge_video PUBLIC forge_inference)
set(FORGE_NVCUVID_INCLUDE "" CACHE PATH "Directory with nvcuvid.h/cuviddec.h (NVIDIA Video Codec SDK) and cuda.h")
find_path(FORGE_NVCUVID_H nvcuvid.h HINTS ${FORGE_NVCUVID_INCLUDE} /usr/local/cuda/include)
if(FORGE_NVCUVID_H)
  target_include_directories(forge_video PRIVATE ${FORGE_NVCUVID_H})
  target_compile_definitions(forge_video PRIVATE FORGE_HAVE_NVCUVID=1)
endif()
if(APPLE)
  target_sources(forge_video PRIVATE support/src/video_vt.mm)
  set_source_files_properties(support/src/video_vt.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(forge_video PUBLIC "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo" "-framework Foundation")
elseif(ANDROID)
  target_link_libraries(forge_video PUBLIC mediandk)
elseif(UNIX)
  target_link_libraries(forge_video PUBLIC ${CMAKE_DL_LIBS})
endif()

if(FORGE_BUILD_APPS)
  add_executable(forge apps/forge.cpp)
  target_link_libraries(forge PRIVATE forge_inference forge_verify forge_simd_mv forge_script)
  target_link_libraries(forge PRIVATE forge_security)   # forge security check
  add_executable(forge-serve apps/forge_serve.cpp)
  target_link_libraries(forge-serve PRIVATE forge_inference forge_verify forge_simd_mv forge_loadbalancer)
endif()

if(FORGE_BUILD_TESTS)
  enable_testing()
  file(GLOB FORGE_TESTS CONFIGURE_DEPENDS tests/test_*.cpp)
  # Extra directories searched for shells by the shell/emit golden tests (user-local installs, never apt).
  set(_shells_default "")
  if(EXISTS "${PROJECT_SOURCE_DIR}/../opt/shells/bin")
    get_filename_component(_shells_default "${PROJECT_SOURCE_DIR}/../opt/shells/bin" ABSOLUTE)
  endif()
  set(FORGE_TEST_SHELL_PATH "${_shells_default}" CACHE STRING "Extra PATH entries for shell backend tests")
  foreach(t ${FORGE_TESTS})
    get_filename_component(n ${t} NAME_WE)
    if(n STREQUAL "test_simd_raw" AND NOT FORGE_ARCH STREQUAL "x86_64")
      continue()
    endif()
    if(n MATCHES "^test_(text|locale)")   # only needs forge_text
      add_executable(${n} ${t})
      target_link_libraries(${n} PRIVATE forge_text)
      add_test(NAME ${n} COMMAND ${n} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
      set_tests_properties(${n} PROPERTIES SKIP_RETURN_CODE 77)
      continue()
    endif()
    if(n STREQUAL "test_os_dynlib")   # os::dynlib: a fixture library to load, plus foreign-format copies to parse
      add_library(forge_test_fixture SHARED tests/dynlib_fixture.c)
      set_target_properties(forge_test_fixture PROPERTIES OUTPUT_NAME forge_fixture PREFIX "lib" C_VISIBILITY_PRESET hidden
                            LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/dynlib_fixture RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/dynlib_fixture)
      if(WIN32)
        set_target_properties(forge_test_fixture PROPERTIES PREFIX "")   # os::library::decorate("forge_fixture") == forge_fixture.dll
      endif()
      add_executable(${n} ${t})
      target_link_libraries(${n} PRIVATE forge_os)
      target_include_directories(${n} PRIVATE tests)
      add_dependencies(${n} forge_test_fixture)
      target_compile_definitions(${n} PRIVATE FORGE_TEST_FIXTURE_DIR="${PROJECT_BINARY_DIR}/dynlib_fixture")
      find_program(FORGE_FIXTURE_CLANG NAMES clang clang-18)
      if(FORGE_FIXTURE_CLANG AND NOT CMAKE_CROSSCOMPILING)
        find_program(FORGE_FIXTURE_LLD_LINK NAMES lld-link lld-link-18)
        find_program(FORGE_FIXTURE_LD64 NAMES ld64.lld ld64.lld-18)
        find_program(FORGE_FIXTURE_LIPO NAMES llvm-lipo llvm-lipo-18)
        find_program(FORGE_FIXTURE_OBJCOPY NAMES llvm-objcopy llvm-objcopy-18)
        set(_xf ${PROJECT_BINARY_DIR}/dynlib_fixture/xfmt)
        add_custom_command(OUTPUT ${_xf}/stamp
          COMMAND ${CMAKE_COMMAND} -DCLANG=${FORGE_FIXTURE_CLANG} -DLLD_LINK=${FORGE_FIXTURE_LLD_LINK} -DLD64=${FORGE_FIXTURE_LD64}
                  -DLIPO=${FORGE_FIXTURE_LIPO} -DOBJCOPY=${FORGE_FIXTURE_OBJCOPY} -DSRC=${PROJECT_SOURCE_DIR}/tests/dynlib_fixture.c
                  -DOUT=${_xf} -P ${PROJECT_SOURCE_DIR}/cmake/dynlib_fixtures.cmake
          DEPENDS ${PROJECT_SOURCE_DIR}/tests/dynlib_fixture.c ${PROJECT_SOURCE_DIR}/cmake/dynlib_fixtures.cmake
          COMMENT "Cross-linking os::dynlib parser fixtures (PE, Mach-O, ELF variants)" VERBATIM)
        add_custom_target(forge_test_fixture_xfmt DEPENDS ${_xf}/stamp)
        add_dependencies(${n} forge_test_fixture_xfmt)
        target_compile_definitions(${n} PRIVATE FORGE_TEST_XFMT_DIR="${_xf}")
      endif()
      add_test(NAME ${n} COMMAND ${n} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
      continue()
    endif()
    if(n STREQUAL "test_mobile")   # only needs the device layer
      add_executable(${n} ${t})
      target_link_libraries(${n} PRIVATE forge_mobile)
      add_test(NAME ${n} COMMAND ${n} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
      continue()
    endif()
    add_executable(${n} ${t})
    target_link_libraries(${n} PRIVATE forge_inference forge_verify forge_simd_mv forge_loadbalancer)
    if(n STREQUAL "test_video")
      target_link_libraries(${n} PRIVATE forge_video)
    endif()
    if(n STREQUAL "test_rpc" AND TARGET forge_rpc_tls)
      target_link_libraries(${n} PRIVATE forge_rpc_tls)
    endif()
    if(n MATCHES "^test_script" OR n STREQUAL "test_signing_detached")   # test_signing_detached: scripts + the example plugin
      target_link_libraries(${n} PRIVATE forge_script)
      if(TARGET forge_script_example_plugin)
        add_dependencies(${n} forge_script_example_plugin)
        target_compile_definitions(${n} PRIVATE FORGE_TEST_PLUGIN="$<TARGET_FILE:forge_script_example_plugin>")
      endif()
    endif()
    if(n STREQUAL "test_script_fsc")   # the .fsc encoder/decoder is internal (script/src/fsc.hpp)
      target_include_directories(${n} PRIVATE script/src)
    endif()
    target_include_directories(${n} PRIVATE tests)
    if(n STREQUAL "test_simd_raw")
      if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${n} PRIVATE /arch:AVX2)
      else()
        target_compile_options(${n} PRIVATE -mavx2 -mfma -mf16c)
      endif()
    endif()
    add_test(NAME ${n} COMMAND ${n} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    if(n MATCHES "^test_script" AND FORGE_TEST_SHELL_PATH)
      set_tests_properties(${n} PROPERTIES ENVIRONMENT "FORGE_TEST_SHELL_PATH=${FORGE_TEST_SHELL_PATH}")
    endif()
    if(n STREQUAL "test_rpc")   # the TLS / mTLS / channel-binding cases alone; exits 77 without OpenSSL
      add_test(NAME test_rpc_tls COMMAND ${n} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
      set_tests_properties(test_rpc_tls PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "FORGE_TEST_ONLY=rpc_tls")
    endif()
  endforeach()
  if(TARGET test_security)   # forge::security (re-runs itself for the lockdown child cases)
    target_link_libraries(test_security PRIVATE forge_security)
    set_tests_properties(test_security PROPERTIES LABELS "security")
  endif()
  if(TARGET test_signing_detached)   # FSS INT-2 / INT-3: detached signatures, plugin and script integrity
    set_tests_properties(test_signing_detached PROPERTIES LABELS "security")
  endif()
  set_tests_properties(test_log PROPERTIES SKIP_RETURN_CODE 77)   # skips when built with FORGE_LOG_MIN_LEVEL
  add_test(NAME log_min_level_cmake COMMAND ${CMAKE_COMMAND} -P ${PROJECT_SOURCE_DIR}/tests/log_min_level.cmake)
  # Scalable vectors at many lengths under qemu: the sve/rvv lane must be active at every VL, including the
  # ones without a power-of-two table (lowered at load time) and ones above 1024 bits.
  if(CMAKE_CROSSCOMPILING AND CMAKE_CROSSCOMPILING_EMULATOR)
    list(GET CMAKE_CROSSCOMPILING_EMULATOR 0 _emu)
    set(_vls)
    if(_emu MATCHES "qemu-aarch64" AND FORGE_SVE256_FLAGS)
      set(_lane sve)
      foreach(b 128 256 384 512 640 1024 1536 2048)
        math(EXPR _bytes "${b} / 8")
        list(APPEND _vls "${b}|max,sve${b}=on,sve-default-vector-length=${_bytes}")
      endforeach()
    elseif(_emu MATCHES "qemu-riscv64" AND FORGE_RVV256_FLAGS)
      set(_lane rvv)
      foreach(b 128 256 512 1024)
        list(APPEND _vls "${b}|rv64,v=true,vlen=${b}")
      endforeach()
    endif()
    foreach(v ${_vls})
      string(REPLACE "|" ";" v "${v}")
      list(GET v 0 b)
      list(GET v 1 cpu)
      add_test(NAME lane_${_lane}_vl${b} COMMAND ${_emu} -cpu ${cpu} $<TARGET_FILE:test_simd> WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
      set_tests_properties(lane_${_lane}_vl${b} PROPERTIES PASS_REGULAR_EXPRESSION "active: ${_lane}" FAIL_REGULAR_EXPRESSION "FAIL")
    endforeach()
    if(_emu MATCHES "qemu-aarch64" AND FORGE_CAN_SME)   # SME outer-product tiles at several streaming VLs
      foreach(b 128 256 512 2048)
        add_test(NAME tile_sme_svl${b} COMMAND ${_emu} -cpu max,sme=on,sme${b}=on $<TARGET_FILE:test_tile> WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
        set_tests_properties(tile_sme_svl${b} PROPERTIES PASS_REGULAR_EXPRESSION "SME SVL" FAIL_REGULAR_EXPRESSION "FAIL|failed")
      endforeach()
    endif()
  endif()
  # forge::os / forge::loadbalancer cross-OS compile+link checks (zig ships FreeBSD, macOS and MinGW libcs).
  find_program(FORGE_ZIG NAMES zig HINTS $ENV{ZIG_PATH})
  if(FORGE_ZIG)
    foreach(tg x86_64-freebsd aarch64-macos x86_64-macos x86_64-windows-gnu aarch64-linux-musl)
      foreach(src test_os test_loadbalancer test_os_ext test_os_process test_os_sys test_os_net test_os_dev test_os_dynlib)
        set(_lib)
        if(tg MATCHES windows)
          set(_lib -lws2_32 -lbcrypt)
        endif()
        add_test(NAME xos_${src}_${tg} COMMAND ${FORGE_ZIG} c++ -target ${tg} -std=c++20 -O1 -I${PROJECT_SOURCE_DIR}/os/include
                 -I${PROJECT_SOURCE_DIR}/loadbalancer/include -I${PROJECT_SOURCE_DIR}/tests -I${PROJECT_SOURCE_DIR}/third_party/monocypher
                 ${PROJECT_SOURCE_DIR}/tests/${src}.cpp ${PROJECT_SOURCE_DIR}/third_party/monocypher/monocypher.c ${_lib}
                 -o ${PROJECT_BINARY_DIR}/xos/${src}.${tg})
        set_tests_properties(xos_${src}_${tg} PROPERTIES LABELS "cross;slow")
      endforeach()
    endforeach()
    # The script emitter + shell backends (host_shell() has per-OS branches): syntax-checked for every target.
    file(GLOB _emit_srcs ${PROJECT_SOURCE_DIR}/script/src/emit.cpp ${PROJECT_SOURCE_DIR}/script/src/shells/*.cpp)
    foreach(tg x86_64-freebsd aarch64-macos x86_64-windows-gnu aarch64-linux-musl)
      string(REPLACE ";" "|" _emit_list "${_emit_srcs}")
      add_test(NAME xos_emit_${tg} COMMAND ${CMAKE_COMMAND} -DZIG=${FORGE_ZIG} -DTARGET=${tg} -DOUT=${PROJECT_BINARY_DIR}/xos/emit_${tg}
               "-DINCS=-I${PROJECT_SOURCE_DIR}/script/include|-I${PROJECT_SOURCE_DIR}/script/src|-I${PROJECT_SOURCE_DIR}/os/include|-I${PROJECT_BINARY_DIR}"
               "-DSRCS=${_emit_list}" -P ${PROJECT_SOURCE_DIR}/cmake/xos_compile_each.cmake)
      set_tests_properties(xos_emit_${tg} PROPERTIES LABELS "cross;slow")
    endforeach()
    # forge::os::mobile backends (every non-Apple-ObjC source; apple.mm needs an Apple SDK -> CI on macOS).
    set(_mob_list "${PROJECT_SOURCE_DIR}/os/src/mobile/mobile.cpp|${PROJECT_SOURCE_DIR}/os/src/mobile/services.cpp|${PROJECT_SOURCE_DIR}/os/src/mobile/linux.cpp|${PROJECT_SOURCE_DIR}/os/src/mobile/windows.cpp|${PROJECT_SOURCE_DIR}/os/src/mobile/freebsd.cpp|${PROJECT_SOURCE_DIR}/os/src/mobile/android.cpp")
    foreach(tg x86_64-freebsd aarch64-macos x86_64-windows-gnu aarch64-linux-musl)
      add_test(NAME xos_mobile_${tg} COMMAND ${CMAKE_COMMAND} -DZIG=${FORGE_ZIG} -DTARGET=${tg} -DOUT=${PROJECT_BINARY_DIR}/xos/mobile_${tg}
               "-DINCS=-I${PROJECT_SOURCE_DIR}/os/include|-I${PROJECT_SOURCE_DIR}/core/include|-I${PROJECT_SOURCE_DIR}/support/include"
               "-DSRCS=${_mob_list}" -P ${PROJECT_SOURCE_DIR}/cmake/xos_compile_each.cmake)
      set_tests_properties(xos_mobile_${tg} PROPERTIES LABELS "cross;slow")
    endforeach()
    # forge::os::text / forge::os::locale (every C++ source; locale_apple.mm needs an Apple SDK -> CI on macOS).
    file(GLOB _text_srcs ${PROJECT_SOURCE_DIR}/os/src/text/*.cpp)
    string(REPLACE ";" "|" _text_list "${_text_srcs}")
    foreach(tg x86_64-freebsd aarch64-macos x86_64-windows-gnu aarch64-linux-musl)
      add_test(NAME xos_text_${tg} COMMAND ${CMAKE_COMMAND} -DZIG=${FORGE_ZIG} -DTARGET=${tg} -DOUT=${PROJECT_BINARY_DIR}/xos/text_${tg}
               "-DINCS=-I${PROJECT_SOURCE_DIR}/os/include|-I${PROJECT_SOURCE_DIR}/core/include|-I${PROJECT_SOURCE_DIR}/support/include"
               "-DSRCS=${_text_list}" -P ${PROJECT_SOURCE_DIR}/cmake/xos_compile_each.cmake)
      set_tests_properties(xos_text_${tg} PROPERTIES LABELS "cross;slow")
    endforeach()
    file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/xos)
  endif()
  # Intel SDE: AMX tiles on an emulated Sapphire Rapids / Granite Rapids, and the skip path on Ice Lake.
  find_program(FORGE_SDE NAMES sde64 HINTS $ENV{SDE_PATH})
  if(FORGE_SDE AND FORGE_CAN_AMX)
    foreach(c spr gnr icl)
      add_test(NAME tile_amx_sde_${c} COMMAND ${FORGE_SDE} -${c} -- $<TARGET_FILE:test_tile> WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
      set_tests_properties(tile_amx_sde_${c} PROPERTIES LABELS emu FAIL_REGULAR_EXPRESSION "FAIL")
    endforeach()
  endif()
  set_tests_properties(test_vulkan PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
  add_test(NAME test_vulkan_align256 COMMAND test_vulkan WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(test_vulkan_align256 PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu ENVIRONMENT FORGE_VK_SSBO_ALIGN=256)
  set_tests_properties(test_cuda_hip PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
  add_test(NAME test_gpu_cuda COMMAND test_cuda_hip cuda)
  add_test(NAME test_gpu_hip COMMAND test_cuda_hip hip)
  add_test(NAME test_gpu_features_cuda COMMAND test_cuda_hip features cuda)
  add_test(NAME test_gpu_features_hip COMMAND test_cuda_hip features hip)
  add_test(NAME test_gpu_metal COMMAND test_cuda_hip metal)
  set_tests_properties(test_gpu_metal PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
  foreach(m opencl ze)   # real runtimes: skip (77) without a driver/device
    add_test(NAME test_gpu_${m} COMMAND test_cuda_hip ${m})
    add_test(NAME test_gpu_features_${m} COMMAND test_cuda_hip features ${m})
    set_tests_properties(test_gpu_${m} test_gpu_features_${m} PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
  endforeach()
  set_tests_properties(test_gpu_features_cuda test_gpu_features_hip PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
  set_tests_properties(test_gpu_cuda test_gpu_hip PROPERTIES SKIP_RETURN_CODE 77 LABELS gpu)
  if(NOT CMAKE_CROSSCOMPILING)
    # The CUDA host runtime against a CPU mock of libcuda.so.1 (argument packing, memcpy, launch sizing).
    add_library(mock_cuda SHARED tests/mock_cuda.c)
    set_target_properties(mock_cuda PROPERTIES OUTPUT_NAME cuda SOVERSION 1 LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/mockcuda)
    target_link_libraries(mock_cuda PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:m>)
    add_test(NAME test_gpu_cuda_mock COMMAND test_cuda_hip cuda)
    set_tests_properties(test_gpu_cuda_mock PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${PROJECT_BINARY_DIR}/mockcuda" LABELS gpu SKIP_RETURN_CODE 77)
    add_dependencies(test_cuda_hip mock_cuda)
    add_library(mock_hip SHARED tests/mock_cuda.c)
    target_compile_definitions(mock_hip PRIVATE MOCK_HIP)
    set_target_properties(mock_hip PROPERTIES OUTPUT_NAME amdhip64 SOVERSION 6 LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/mockhip)
    target_link_libraries(mock_hip PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:m>)
    add_dependencies(test_cuda_hip mock_hip)
    foreach(m opencl ze)
      if(m STREQUAL "opencl")
        add_library(mock_${m} SHARED tests/mock_opencl.c)
        set_target_properties(mock_${m} PROPERTIES OUTPUT_NAME OpenCL SOVERSION 1)
      else()
        add_library(mock_${m} SHARED tests/mock_ze.c)
        set_target_properties(mock_${m} PROPERTIES OUTPUT_NAME ze_loader SOVERSION 1)
      endif()
      set_target_properties(mock_${m} PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/mock${m})
      target_include_directories(mock_${m} PRIVATE ${FORGE_GPU_DIR} ${PROJECT_SOURCE_DIR}/third_party)
      target_link_libraries(mock_${m} PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:m>)
      add_dependencies(mock_${m} forge_gpu_gen)
      add_dependencies(test_cuda_hip mock_${m})
      add_test(NAME test_gpu_${m}_mock COMMAND test_cuda_hip ${m})
      add_test(NAME test_gpu_features_${m}_mock COMMAND test_cuda_hip features ${m})
      set_tests_properties(test_gpu_${m}_mock test_gpu_features_${m}_mock PROPERTIES LABELS gpu SKIP_RETURN_CODE 77
        ENVIRONMENT "LD_LIBRARY_PATH=${PROJECT_BINARY_DIR}/mock${m};MOCK_DEVICES=2;FORGE_ZE_TEST_IMAGE=opencl-c")
    endforeach()
    add_test(NAME test_gpu_hip_mock COMMAND test_cuda_hip hip)
    set_tests_properties(test_gpu_hip_mock PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${PROJECT_BINARY_DIR}/mockhip" LABELS gpu SKIP_RETURN_CODE 77)
    add_test(NAME test_gpu_features_cuda_mock COMMAND test_cuda_hip features cuda)
    set_tests_properties(test_gpu_features_cuda_mock PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${PROJECT_BINARY_DIR}/mockcuda;MOCK_DEVICES=2" LABELS gpu SKIP_RETURN_CODE 77)
    add_test(NAME test_gpu_features_hip_mock COMMAND test_cuda_hip features hip)
    set_tests_properties(test_gpu_features_hip_mock PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${PROJECT_BINARY_DIR}/mockhip;MOCK_DEVICES=2" LABELS gpu SKIP_RETURN_CODE 77)
    # Device memory pools: allocation accounting, LRU eviction under a small budget, driver out-of-memory retry.
    foreach(m cuda hip opencl ze)
      add_test(NAME test_gpu_memory_${m}_mock COMMAND test_cuda_hip memory ${m})
      set_tests_properties(test_gpu_memory_${m}_mock PROPERTIES LABELS gpu SKIP_RETURN_CODE 77
        ENVIRONMENT "LD_LIBRARY_PATH=${PROJECT_BINARY_DIR}/mock${m};FORGE_ZE_TEST_IMAGE=opencl-c")
    endforeach()
    if(FORGE_BUILD_APPS)
      add_test(NAME golden_tiny_llama_cuda_mock COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/verify/golden/tiny_llama.py
               --forge $<TARGET_FILE:forge> --work ${PROJECT_BINARY_DIR}/golden_cumock)
      set_tests_properties(golden_tiny_llama_cuda_mock PROPERTIES LABELS gpu
        ENVIRONMENT "FORGE_DEVICE=cuda;LD_LIBRARY_PATH=${PROJECT_BINARY_DIR}/mockcuda")
      add_test(NAME golden_tiny_llama_opencl_mock COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/verify/golden/tiny_llama.py
               --forge $<TARGET_FILE:forge> --work ${PROJECT_BINARY_DIR}/golden_clmock)
      set_tests_properties(golden_tiny_llama_opencl_mock PROPERTIES LABELS gpu
        ENVIRONMENT "FORGE_DEVICE=opencl;LD_LIBRARY_PATH=${PROJECT_BINARY_DIR}/mockopencl")
      add_test(NAME golden_tiny_llama_ze_mock COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/verify/golden/tiny_llama.py
               --forge $<TARGET_FILE:forge> --work ${PROJECT_BINARY_DIR}/golden_zemock)
      set_tests_properties(golden_tiny_llama_ze_mock PROPERTIES LABELS gpu
        ENVIRONMENT "FORGE_DEVICE=level_zero;FORGE_ZE_TEST_IMAGE=opencl-c;LD_LIBRARY_PATH=${PROJECT_BINARY_DIR}/mockze")
      add_test(NAME golden_tiny_llama_hip_mock COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/verify/golden/tiny_llama.py
               --forge $<TARGET_FILE:forge> --work ${PROJECT_BINARY_DIR}/golden_hipmock)
      set_tests_properties(golden_tiny_llama_hip_mock PROPERTIES LABELS gpu
        ENVIRONMENT "FORGE_DEVICE=hip;LD_LIBRARY_PATH=${PROJECT_BINARY_DIR}/mockhip")
    endif()
  endif()
  if(FORGE_HAVE_VULKAN)
    target_include_directories(test_vulkan PRIVATE ${FORGE_GEN_DIR}/vkinc)
  endif()
  # CUDA / HIP: compile the generated kernels to PTX / AMDGCN with clang (no SDK needed); hardware runs need a GPU.
  if(FORGE_GPU_CLANG)
    # Apple clang has no NVPTX backend and some Windows clang builds no AMDGPU one: a missing backend is a
    # skip (77) with the reason printed, not a failure. The skip script asks the clang that would run the test.
    execute_process(COMMAND ${FORGE_GPU_CLANG} -print-targets OUTPUT_VARIABLE _forge_gpu_targets ERROR_QUIET)
    foreach(_t cuda:nvptx64 hip:amdgcn)
      string(REPLACE ":" ";" _tp ${_t})
      list(GET _tp 0 _lang)
      list(GET _tp 1 _be)
      string(FIND "${_forge_gpu_targets}" " ${_be} " _has)
      set(FORGE_GPU_HAS_${_lang} ON)
      if(_has LESS 0)
        set(FORGE_GPU_HAS_${_lang} OFF)
      endif()
    endforeach()
    if(FORGE_GPU_HAS_cuda)
      add_test(NAME gpu_compile_cuda COMMAND ${FORGE_GPU_CLANG} -x cuda --cuda-device-only -nocudainc -nocudalib -Wno-unknown-cuda-version
               --cuda-gpu-arch=sm_80 -Werror -S -o ${FORGE_GPU_DIR}/kernels.check_sm80.ptx ${FORGE_GPU_DIR}/kernels.cu)
    else()
      add_test(NAME gpu_compile_cuda COMMAND ${CMAKE_COMMAND} -E echo "skip: ${FORGE_GPU_CLANG} has no nvptx64 backend")
      set_tests_properties(gpu_compile_cuda PROPERTIES SKIP_REGULAR_EXPRESSION "^skip: ")
    endif()
    if(FORGE_GPU_HAS_hip)
      add_test(NAME gpu_compile_hip COMMAND ${FORGE_GPU_CLANG} -x hip --cuda-device-only -nogpuinc -nogpulib
               --offload-arch=gfx90a -Werror -S -o ${FORGE_GPU_DIR}/kernels.check_gfx90a.s ${FORGE_GPU_DIR}/kernels.cu)
    else()
      add_test(NAME gpu_compile_hip COMMAND ${CMAKE_COMMAND} -E echo "skip: ${FORGE_GPU_CLANG} has no amdgcn backend")
      set_tests_properties(gpu_compile_hip PROPERTIES SKIP_REGULAR_EXPRESSION "^skip: ")
    endif()
    add_test(NAME gpu_compile_opencl COMMAND ${FORGE_GPU_CLANG} -x cl -cl-std=CL1.2 --target=spir64 -Werror -fsyntax-only ${FORGE_GPU_DIR}/kernels.cl)
    add_test(NAME gpu_compile_opencl_nofp64 COMMAND ${FORGE_GPU_CLANG} -x cl -cl-std=CL3.0 --target=spir64 -Xclang -cl-ext=-cl_khr_fp64,-__opencl_c_fp64
             -Werror -fsyntax-only ${FORGE_GPU_DIR}/kernels.cl)
    # Mobile: NDK cross-builds (skip without ANDROID_NDK_HOME), device + iOS runs (skip without adb/Xcode).
    foreach(abi arm64-v8a x86_64)
      add_test(NAME mobile_android_${abi} COMMAND sh ${PROJECT_SOURCE_DIR}/tests/mobile_build.sh android ${abi})
      set_tests_properties(mobile_android_${abi} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "mobile;slow"
        ENVIRONMENT "FORGE_MOBILE_BUILD_DIR=${PROJECT_BINARY_DIR}/mobile")
    endforeach()
    add_test(NAME mobile_android_device COMMAND sh ${PROJECT_SOURCE_DIR}/tests/mobile_build.sh android-device)
    add_test(NAME mobile_ios COMMAND sh ${PROJECT_SOURCE_DIR}/tests/mobile_build.sh ios)
    set_tests_properties(mobile_android_device mobile_ios PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 3600 LABELS "mobile;slow")
    find_path(FORGE_JNI_H jni.h HINTS $ENV{JAVA_HOME}/include NO_CMAKE_SYSTEM_PATH)  # JDK only; the NDK path is covered by mobile_android_*
    if(FORGE_JNI_H)  # host syntax check of the JNI bridge
      file(GLOB _jni_md "${FORGE_JNI_H}/linux" "${FORGE_JNI_H}/darwin" "${FORGE_JNI_H}/win32")
      set(_jni_inc -I${FORGE_JNI_H})
      foreach(d ${_jni_md})
        list(APPEND _jni_inc -I${d})
      endforeach()
      if(MSVC)   # cl / clang-cl spelling of the same check
        add_test(NAME jni_syntax COMMAND ${CMAKE_C_COMPILER} /nologo /c /Fo${PROJECT_BINARY_DIR}/jni_syntax.obj /WX /W3 ${_jni_inc}
          -I${PROJECT_SOURCE_DIR}/inference/include ${PROJECT_SOURCE_DIR}/bindings/android/jni/forge_jni.c)
      else()
        add_test(NAME jni_syntax COMMAND ${CMAKE_C_COMPILER} -c -o ${PROJECT_BINARY_DIR}/jni_syntax.o -Werror -Wall -Wno-unused-command-line-argument ${_jni_inc}
          -I${PROJECT_SOURCE_DIR}/inference/include ${PROJECT_SOURCE_DIR}/bindings/android/jni/forge_jni.c)
      endif()
    endif()
    add_test(NAME gpu_compile_metal_syntax COMMAND ${FORGE_GPU_CLANG} --driver-mode=g++ -std=c++17 -fsyntax-only -Wno-unknown-attributes -Werror
             -include ${PROJECT_SOURCE_DIR}/tests/shims/msl_host_shim.hpp -x c++ ${FORGE_GPU_DIR}/kernels.metal)
    find_program(FORGE_XCRUN xcrun)
    if(FORGE_XCRUN)   # real Metal compiler (macOS with Xcode)
      add_test(NAME gpu_compile_metal COMMAND ${FORGE_XCRUN} -sdk macosx metal -c ${FORGE_GPU_DIR}/kernels.metal -o ${PROJECT_BINARY_DIR}/kernels.air)
      set_tests_properties(gpu_compile_metal PROPERTIES LABELS gpu)
    endif()
    set_tests_properties(gpu_compile_metal_syntax gpu_compile_cuda gpu_compile_hip gpu_compile_opencl gpu_compile_opencl_nofp64 PROPERTIES LABELS gpu)
  endif()
  # Cross builds (e.g. aarch64 under qemu) are typically static: skip the shared-library smoke test there.
  if(NOT CMAKE_CROSSCOMPILING)
    add_executable(capi_smoke tests/capi_smoke.c)
    target_include_directories(capi_smoke PRIVATE inference/include)
    target_link_libraries(capi_smoke PRIVATE forge_c)
    add_executable(doc_example_capi_generate docs/examples/capi_generate.c)
    target_include_directories(doc_example_capi_generate PRIVATE inference/include)
    target_link_libraries(doc_example_capi_generate PRIVATE forge_c)
    # libforge.so needs libstdc++; link the C consumers with the C++ driver so toolchains with a private
    # sysroot (conda clang) can resolve it.
    set_target_properties(capi_smoke doc_example_capi_generate PROPERTIES LINKER_LANGUAGE CXX)
    if(FORGE_BUILD_APPS)
      add_test(NAME doc_example_capi_generate COMMAND doc_example_capi_generate ${PROJECT_BINARY_DIR}/golden/tiny_f32.fpk)
      set_tests_properties(doc_example_capi_generate PROPERTIES DEPENDS golden_tiny_llama)
    endif()
  endif()
  # Documentation examples (docs/examples): compiled and run so the guides cannot drift from the API.
  add_executable(doc_example_simd_dispatch docs/examples/simd_dispatch.cpp)
  target_link_libraries(doc_example_simd_dispatch PRIVATE forge_simd_mv)
  add_test(NAME doc_example_simd_dispatch COMMAND doc_example_simd_dispatch)
  add_executable(doc_example_gpu_graph docs/examples/gpu_graph.cpp)
  target_link_libraries(doc_example_gpu_graph PRIVATE forge_gpu forge_simd_mv)
  add_test(NAME doc_example_gpu_graph COMMAND doc_example_gpu_graph)
  add_executable(doc_example_modelfile_roundtrip docs/examples/modelfile_roundtrip.cpp)
  target_link_libraries(doc_example_modelfile_roundtrip PRIVATE forge_modelfile forge_simd_mv)
  add_test(NAME doc_example_modelfile_roundtrip COMMAND doc_example_modelfile_roundtrip)
  add_executable(doc_example_graph_build docs/examples/graph_build.cpp)
  target_link_libraries(doc_example_graph_build PRIVATE forge_graph forge_verify forge_simd_mv)
  add_test(NAME doc_example_graph_build COMMAND doc_example_graph_build)
  add_executable(doc_example_os_features docs/examples/os_features.cpp)
  target_link_libraries(doc_example_os_features PRIVATE forge_os)
  add_test(NAME doc_example_os_features COMMAND doc_example_os_features)
  add_executable(doc_example_loadbalancer_pool docs/examples/loadbalancer_pool.cpp)
  target_link_libraries(doc_example_loadbalancer_pool PRIVATE forge_loadbalancer)
  add_test(NAME doc_example_loadbalancer_pool COMMAND doc_example_loadbalancer_pool)
  add_executable(doc_example_inference_generate docs/examples/inference_generate.cpp)
  target_link_libraries(doc_example_inference_generate PRIVATE forge_inference forge_verify forge_simd_mv)
  if(FORGE_BUILD_APPS)
    add_test(NAME doc_example_inference_generate COMMAND doc_example_inference_generate ${PROJECT_BINARY_DIR}/golden/tiny_f32.fpk "Hi")
    set_tests_properties(doc_example_inference_generate PROPERTIES DEPENDS golden_tiny_llama)
  endif()
  if(FORGE_BUILD_APPS)
    add_test(NAME modelfile_signing COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/sign_test.py
             $<TARGET_FILE:forge> ${PROJECT_BINARY_DIR}/golden/tiny_f32.fpk)
    set_tests_properties(modelfile_signing PROPERTIES DEPENDS golden_tiny_llama)
    # detached signatures (forge sign / verify on any file), forge script and rpc-serve under integrity.scripts
    add_test(NAME signing_cli COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/signing_cli_test.py $<TARGET_FILE:forge>)
    set_tests_properties(signing_cli PROPERTIES LABELS security)
    add_test(NAME onnx_import COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/onnx_test.py $<TARGET_FILE:forge>)
    add_test(NAME golden_moe_mla
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/verify/golden/tiny_moe.py
              --forge $<TARGET_FILE:forge> --work ${PROJECT_BINARY_DIR}/golden_moe)
    # Python with torch/transformers/gguf (verify/golden/requirements.txt) for the golden tests below.
    set(FORGE_GOLDEN_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH "Python with torch/transformers/gguf for golden tests")
    add_test(NAME golden_quant_inplace
      COMMAND ${FORGE_GOLDEN_PYTHON} ${PROJECT_SOURCE_DIR}/verify/golden/quant_inplace.py
              --forge $<TARGET_FILE:forge> --work ${PROJECT_BINARY_DIR}/golden_quant_inplace)
    # Qwen3.5 (Gated DeltaNet) against transformers; skips without torch + transformers >= 5. Set
    # FORGE_GOLDEN_PYTHON to an interpreter that has them (e.g. a venv) to run it.
    add_test(NAME golden_tiny_qwen3_5
      COMMAND ${FORGE_GOLDEN_PYTHON} ${PROJECT_SOURCE_DIR}/verify/golden/tiny_qwen3_5.py
              --forge $<TARGET_FILE:forge> --work ${PROJECT_BINARY_DIR}/golden_tiny_qwen3_5)
    add_test(NAME golden_tiny_qwen3
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/verify/golden/tiny_qwen3.py
              --forge $<TARGET_FILE:forge> --work ${PROJECT_BINARY_DIR}/golden_qwen3)
    add_test(NAME golden_tiny_llama
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/verify/golden/tiny_llama.py
              --forge $<TARGET_FILE:forge> --work ${PROJECT_BINARY_DIR}/golden)
    if(FORGE_HAVE_VULKAN AND NOT CMAKE_CROSSCOMPILING)
      # Same goldens with the Vulkan lowerings (ops without a GPU kernel run on the CPU).
      add_test(NAME golden_tiny_llama_vulkan COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/verify/golden/tiny_llama.py
               --forge $<TARGET_FILE:forge> --work ${PROJECT_BINARY_DIR}/golden_vk)
      add_test(NAME golden_moe_mla_vulkan COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/verify/golden/tiny_moe.py
               --forge $<TARGET_FILE:forge> --work ${PROJECT_BINARY_DIR}/golden_moe_vk)
      set_tests_properties(golden_tiny_llama_vulkan golden_moe_mla_vulkan PROPERTIES ENVIRONMENT "FORGE_DEVICE=vulkan" LABELS gpu)
    endif()
    if(CMAKE_CROSSCOMPILING_EMULATOR)
      list(JOIN CMAKE_CROSSCOMPILING_EMULATOR " " _forge_runner)
      set_tests_properties(golden_tiny_llama golden_tiny_qwen3 golden_tiny_qwen3_5 golden_quant_inplace golden_moe_mla modelfile_signing signing_cli onnx_import PROPERTIES ENVIRONMENT "FORGE_RUNNER=${_forge_runner}")
    endif()
    if(TARGET capi_smoke)
      add_test(NAME capi_smoke COMMAND capi_smoke ${PROJECT_BINARY_DIR}/golden/tiny_f32.fpk)
      set_tests_properties(capi_smoke PROPERTIES DEPENDS golden_tiny_llama)
      add_test(NAME python_bindings COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/python_bindings.py
               ${PROJECT_BINARY_DIR}/golden/tiny_f32.fpk $<TARGET_FILE:forge> ${PROJECT_BINARY_DIR}/golden/tiny_q8_0.fpk)
      set_tests_properties(python_bindings PROPERTIES DEPENDS golden_tiny_llama ENVIRONMENT "FORGE_LIB=$<TARGET_FILE:forge_c>")
      add_test(NAME serve_http COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/serve_test.py
               $<TARGET_FILE:forge-serve> $<TARGET_FILE:forge> ${PROJECT_BINARY_DIR}/golden/tiny_f32.fpk)
      set_tests_properties(serve_http PROPERTIES DEPENDS golden_tiny_llama)
      add_test(NAME security_cli COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/security_cli_test.py
               $<TARGET_FILE:forge> $<TARGET_FILE:forge-serve> ${PROJECT_BINARY_DIR}/golden/tiny_f32.fpk)
      set_tests_properties(security_cli PROPERTIES DEPENDS golden_tiny_llama LABELS security)
    endif()
    # Python-module prerequisites, probed once at configure time: a test whose modules are missing is
    # reported by ctest as "Not Run (Disabled)" instead of failing (install them to run it; see docs/building.md).
    function(forge_py_needs mods)
      string(REPLACE ";" "," _imp "${mods}")
      execute_process(COMMAND ${Python3_EXECUTABLE} -c "import ${_imp}" RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
      if(NOT _rc EQUAL 0)
        message(STATUS "FORGE: python lacks one of {${_imp}}; disabled tests: ${ARGN}")
        foreach(t IN LISTS ARGN)
          if(TEST ${t})
            set_tests_properties(${t} PROPERTIES DISABLED TRUE)
          endif()
        endforeach()
      endif()
    endfunction()
    forge_py_needs("numpy;safetensors" golden_tiny_llama golden_tiny_qwen3 golden_tiny_qwen3_5 golden_moe_mla golden_quant_inplace
      golden_tiny_llama_vulkan golden_moe_mla_vulkan golden_tiny_llama_cuda_mock golden_tiny_llama_opencl_mock
      golden_tiny_llama_ze_mock golden_tiny_llama_hip_mock doc_example_inference_generate doc_example_capi_generate
      capi_smoke python_bindings serve_http security_cli modelfile_signing)
    forge_py_needs("cryptography" modelfile_signing)
    forge_py_needs("numpy;onnx" onnx_import)
    if(Python3_EXECUTABLE)
      add_test(NAME docs_up_to_date COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/gen_docs.py --check)
      add_test(NAME sbom_spdx COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/sbom_test.py ${PROJECT_SOURCE_DIR})
    endif()
  endif()
endif()

# ---- fuzz targets (fuzz/): libFuzzer entry points + a built-in mutational driver for CI ----
if(FORGE_BUILD_TESTS)
  foreach(ft json blueprint fpk safetensors gguf tokenizer grammar onnx)
    if(FORGE_LIBFUZZER)
      add_executable(fuzz_${ft} fuzz/fuzz_${ft}.cpp)
      target_compile_options(fuzz_${ft} PRIVATE -fsanitize=fuzzer)
      target_link_options(fuzz_${ft} PRIVATE -fsanitize=fuzzer)
    else()
      add_executable(fuzz_${ft} fuzz/fuzz_${ft}.cpp fuzz/driver.cpp)
    endif()
    target_link_libraries(fuzz_${ft} PRIVATE forge_inference forge_verify forge_simd_mv)
    if(NOT FORGE_LIBFUZZER)
      add_test(NAME fuzz_${ft} COMMAND fuzz_${ft} --iters ${FORGE_FUZZ_ITERS} ${PROJECT_SOURCE_DIR}/fuzz/corpus/${ft}
               WORKING_DIRECTORY ${PROJECT_BINARY_DIR})
      set_tests_properties(fuzz_${ft} PROPERTIES TIMEOUT 600 LABELS fuzz)
    endif()
  endforeach()
endif()
if(FORGE_BUILD_TESTS AND TARGET forge_script)   # script front end (parser + compiler + verifier)
  if(FORGE_LIBFUZZER)
    add_executable(fuzz_script fuzz/fuzz_script.cpp)
    target_compile_options(fuzz_script PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_script PRIVATE -fsanitize=fuzzer)
  else()
    add_executable(fuzz_script fuzz/fuzz_script.cpp fuzz/driver.cpp)
    add_test(NAME fuzz_script COMMAND fuzz_script --iters ${FORGE_FUZZ_ITERS} ${PROJECT_SOURCE_DIR}/fuzz/corpus/script
             WORKING_DIRECTORY ${PROJECT_BINARY_DIR})
    set_tests_properties(fuzz_script PROPERTIES TIMEOUT 600 LABELS fuzz)
  endif()
  target_link_libraries(fuzz_script PRIVATE forge_script)
  target_include_directories(fuzz_script PRIVATE script/src)
  # .fsc cache loader (script/src/fsc.cpp): header/checksum/signature checks and the payload decoder
  if(FORGE_LIBFUZZER)
    add_executable(fuzz_fsc fuzz/fuzz_fsc.cpp)
    target_compile_options(fuzz_fsc PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_fsc PRIVATE -fsanitize=fuzzer)
  else()
    add_executable(fuzz_fsc fuzz/fuzz_fsc.cpp fuzz/driver.cpp)
    add_test(NAME fuzz_fsc COMMAND fuzz_fsc --iters ${FORGE_FUZZ_ITERS} ${PROJECT_SOURCE_DIR}/fuzz/corpus/fsc
             WORKING_DIRECTORY ${PROJECT_BINARY_DIR})
    set_tests_properties(fuzz_fsc PROPERTIES TIMEOUT 600 LABELS fuzz)
  endif()
  target_link_libraries(fuzz_fsc PRIVATE forge_script)
  target_include_directories(fuzz_fsc PRIVATE script/src)
endif()

# App integrations (Part F.2): runnable embedding examples for FUSE, Cypha, CameraDetector, seatbelt, NeuralVFX.
option(FORGE_BUILD_EXAMPLES "Build examples/integrations" ON)
if(FORGE_BUILD_EXAMPLES)
  file(GLOB FORGE_EXAMPLES CONFIGURE_DEPENDS examples/integrations/*.cpp)
  foreach(e ${FORGE_EXAMPLES})
    get_filename_component(n ${e} NAME_WE)
    add_executable(example_${n} ${e})
    target_link_libraries(example_${n} PRIVATE forge_inference forge_verify forge_simd_mv)
    if(FORGE_BUILD_TESTS)
      add_test(NAME example_${n} COMMAND example_${n})
      set_tests_properties(example_${n} PROPERTIES LABELS integration)
    endif()
  endforeach()
endif()

# ---- Install + packaging (v1.0 release tooling; tools/release.py drives this) -------------------------
include(GNUInstallDirs)
install(DIRECTORY core/include/ simd/include/ ops/include/ gpu/include/ graph/include/ modelfile/include/ inference/include/ support/include/ verify/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev)
install(TARGETS forge_c LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
install(TARGETS forge_simd_mv forge_trust ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev)
foreach(t forge forge-serve)
  if(TARGET ${t})
    install(TARGETS ${t} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
  endif()
endforeach()
install(FILES LICENSE NOTICE COMMERCIAL-LICENCE.md SECURITY.md README.md DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT runtime)

set(CPACK_PACKAGE_NAME forge)
set(CPACK_PACKAGE_VENDOR "Odin Loch")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "FORGE: one C++23 stack for SIMD, GPU and model inference")
set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE)
set(CPACK_PACKAGE_FILE_NAME "forge-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "forge-${PROJECT_VERSION}-src")
set(CPACK_SOURCE_IGNORE_FILES "/build/" "/\\\\.git/" "/\\\\.venv/" "__pycache__")
if(WIN32)
  set(CPACK_GENERATOR ZIP)
  set(CPACK_SOURCE_GENERATOR ZIP)
else()
  set(CPACK_GENERATOR TGZ)
  set(CPACK_SOURCE_GENERATOR TGZ)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND CPACK_GENERATOR DEB)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Odin Loch")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  endif()
endif()
include(CPack)
