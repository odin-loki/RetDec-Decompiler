# cuda_accel — experimental, unintegrated

This directory is a **parked research** GPU acceleration layer for RetDec
Imortek. It is **not wired** into the decompiler pipeline, is **not linked**
from `src/retdec`, and is **not a product feature** at v2.0.22.
`C-CUDA-PIPE` is withdrawn ([docs/CLAIMS.md](../../docs/CLAIMS.md)).

Linux, macOS, and Windows installers do **not** require an NVIDIA card.

- CMake option `RETDEC_ENABLE_CUDA_ACCEL` defaults **OFF**. Full presets
  (`full-linux-*`, `full-windows-*`) keep it OFF.
- `src/CMakeLists.txt` still calls `add_subdirectory(cuda_accel)` so the
  option can be consumed in place; with the option OFF the target builds
  CPU-only stubs and does not enable the CUDA language.
- Do not advertise CUDA acceleration as a shipped capability.

Research-only enable:

```bash
cmake --preset full-linux-debug -DRETDEC_ENABLE_CUDA_ACCEL=ON
```

Nothing in the specification-extraction pipeline (buildable C, semantic
detections, optional llama.cpp) calls this library.

Include paths remain `retdec/cuda_accel/*`. Public write-up:
[docs/CUDA_CAPABILITIES.md](../../docs/CUDA_CAPABILITIES.md).
[docs/RESEARCH_FRONTIERS.md](../../docs/RESEARCH_FRONTIERS.md) § GPU
acceleration (parked).
