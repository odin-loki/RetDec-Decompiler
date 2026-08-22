# cuda_accel — experimental, unintegrated

This directory is an **experimental** GPU acceleration layer. It is **not
wired** into the decompiler pipeline, is **not linked** from `src/retdec`,
and is **not a product feature**.

- CMake option `RETDEC_ENABLE_CUDA_ACCEL` defaults **OFF**. Full presets do
  not require an NVIDIA card.
- `src/CMakeLists.txt` still calls `add_subdirectory(cuda_accel)` so the
  option can be consumed in place; with the option OFF the target builds
  CPU-only stubs and does not enable the CUDA language.
- Do not advertise CUDA acceleration as a shipped capability.

Include paths remain `retdec/cuda_accel/*`.
