# Cross-build for AArch64 and run ctest under qemu with 256-bit SVE (exercises the `sve` lane table).
# Compilers default to the GNU cross toolchain; override with -DFORGE_CROSS_CC/-DFORGE_CROSS_CXX
# (e.g. a wrapper running `zig c++ -target aarch64-linux-musl`). Build with -DCMAKE_EXE_LINKER_FLAGS=-static.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(FORGE_CROSS_CC aarch64-linux-gnu-gcc CACHE STRING "")
set(FORGE_CROSS_CXX aarch64-linux-gnu-g++ CACHE STRING "")
set(CMAKE_C_COMPILER ${FORGE_CROSS_CC})
set(CMAKE_CXX_COMPILER ${FORGE_CROSS_CXX})
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64;-cpu;max,sve256=on)
