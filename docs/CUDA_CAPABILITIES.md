# CUDA capabilities in RetDec

RetDec uses two independent CUDA-related build switches. They control different
subsystems and should not be conflated.

## Build flags

| CMake option | Default (full preset) | Purpose |
|--------------|----------------------|---------|
| `RETDEC_ENABLE_CUDA` | OFF | [`retdec-gpu-scanner`](../src/utils/gpu_scanner.cu) — GPU signature matching and file entropy |
| `RETDEC_ENABLE_CUDA_ACCEL` | ON (when toolkit found) | CUDA acceleration layer (`cuda_accel`), Qwen3 GPU inference, GUI OpenCL/CUDA recovery hooks |

Disable GPU builds when no toolkit is installed:

```bash
cmake -S . -B build -DRETDEC_ENABLE_CUDA=OFF -DRETDEC_ENABLE_CUDA_ACCEL=OFF
```

Windows helper: `scripts/windows_native_configure.ps1 -NoCuda`

## GPU-accelerated (`RETDEC_ENABLE_CUDA=ON`)

Implemented in [`include/retdec/utils/gpu_scanner.h`](../include/retdec/utils/gpu_scanner.h):

- **Signature batch matching** — upload file bytes once, run many YARA-like nibble patterns on GPU
- **Whole-file entropy** — parallel Shannon entropy for quick triage

When `RETDEC_ENABLE_CUDA=OFF` or no capable device is present, [`gpu_scanner_cpu.cpp`](../src/utils/gpu_scanner_cpu.cpp) provides the same API on CPU.

**Entry point:** `retdec::utils::GpuScanner` (used from [`src/retdec/retdec.cpp`](../src/retdec/retdec.cpp) during binary loading / pattern phases).

## CUDA acceleration layer (`RETDEC_ENABLE_CUDA_ACCEL=ON`)

Separate from `gpu_scanner`:

| Component | Role |
|-----------|------|
| [`src/cuda_accel/`](../src/cuda_accel/) | `CUDAContext`, kernel cache, optional profiling |
| [`src/qwen3/`](../src/qwen3/) | GGUF model GPU inference (FlashAttention path when CUDA available) |
| GUI **CUDA** settings tab | Device index, block size, kernel cache directory |

These accelerate **analysis / ML**, not the core LLVM → C decompilation pipeline.

## CPU-only (main decompile pipeline)

The following always run on CPU regardless of CUDA flags:

- LLVM bitcode lifting (`bin2llvmir`)
- SSA / alias / type inference passes
- `llvmir2hll` C emission
- Managed-language routes (`managed_decompiler` — Java, DEX, Python, Lua, WASM, CLI)
- Post-pipeline semantic detectors (container, sort, algorithm, concurrency)
- Unpacker, fileformat parsers, YARA static-code detection (except optional `GpuScanner` fast path)

## Quick reference

```
Binary input
    │
    ├─► GpuScanner (optional GPU)     RETDEC_ENABLE_CUDA
    │
    ├─► Managed dispatcher            CPU
    │
    └─► LLVM pipeline + llvmir2hll    CPU
            │
            └─► Post-pipeline analysis CPU
                    │
                    └─► Qwen3 / cuda_accel (optional GPU)  RETDEC_ENABLE_CUDA_ACCEL
```

## Related docs

- [BUILD_REFERENCE.md](BUILD_REFERENCE.md) — preset and `-NoCuda` notes
- [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) — Windows CUDA defaults
- [ENGINEERING_ROADMAP.md](ENGINEERING_ROADMAP.md) — product tiers
