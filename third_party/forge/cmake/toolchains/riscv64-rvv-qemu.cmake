# Cross-build for RISC-V 64 and run ctest under qemu with V at VLEN=256 (exercises the `rvv` lane table).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)
set(FORGE_CROSS_CC riscv64-linux-gnu-gcc CACHE STRING "")
set(FORGE_CROSS_CXX riscv64-linux-gnu-g++ CACHE STRING "")
set(CMAKE_C_COMPILER ${FORGE_CROSS_CC})
set(CMAKE_CXX_COMPILER ${FORGE_CROSS_CXX})
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-riscv64;-cpu;rv64,v=true,vlen=256)
