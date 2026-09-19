# CUDA / OpenCL — parked acceleration vs host recovery

Imortek **2.0.22**. Three different subsystems share the word “CUDA”. They
are not interchangeable.

| Concern | What it is | In `decompile()`? |
|---------|------------|-------------------|
| **GPU acceleration** | `src/cuda_accel/`, `src/opencl/`, optional `GpuScanner` | **No** |
| **CUDA host API recovery** | `ptx_decompile::CudaHostRecovery` (`cudaLaunchKernel`, `cudaMalloc`, …) | **No** (tests only) |
| **OpenCL host API recovery** | `ptx_decompile::OclHostRecovery` (`clCreate*`, `clEnqueue*`, …) | **Yes** (log summary) |
| **PTX text → CUDA-C** | `PtxParser` / `InstrLifter` | **No** (no CLI `.ptx`) |
| **SASS machine code** | NVIDIA GPU ISA | **No** (library only; not Production) |

`src/cuda_accel/` and `src/opencl/` are parked research trees. They are
**not linked** from `src/retdec`, default **OFF**, and must not be advertised
as shipped GPU acceleration of the decompiler. OpenCL sources are **not**
added from `src/CMakeLists.txt`. See [RESEARCH_FRONTIERS.md](RESEARCH_FRONTIERS.md)
§ GPU acceleration (parked).

Enable CUDA accel only with `-DRETDEC_ENABLE_CUDA_ACCEL=ON` for research
builds.

## Build flags

| CMake option | Default | Purpose |
|--------------|---------|---------|
| `RETDEC_ENABLE_CUDA` | **OFF** (`src/utils/CMakeLists.txt`) | `retdec-gpu-scanner` — GPU signature matching and file entropy when nvcc is present; else CPU twin |
| `RETDEC_ENABLE_CUDA_ACCEL` | **OFF** (`cmake/options.cmake`) | Experimental `src/cuda_accel/` (`CUDAContext`, kernel cache). Unintegrated |

Disable GPU builds when no toolkit is installed:

```bash
cmake -S . -B build -DRETDEC_ENABLE_CUDA=OFF -DRETDEC_ENABLE_CUDA_ACCEL=OFF
```

Windows helper: `scripts/windows_native_configure.ps1 -NoCuda`

## GPU-accelerated scanner (`RETDEC_ENABLE_CUDA=ON`)

Implemented in [`include/retdec/utils/gpu_scanner.h`](../include/retdec/utils/gpu_scanner.h):

- **Signature batch matching** — upload file bytes once, run nibble patterns
- **Whole-file entropy** — parallel Shannon entropy

When `RETDEC_ENABLE_CUDA=OFF` or no device, [`gpu_scanner_cpu.cpp`](../src/utils/gpu_scanner_cpu.cpp)
provides the same API on CPU.

**Entry point:** `retdec::utils::GpuScanner`. `src/retdec` **links**
`retdec::gpu-scanner` and **does not construct** one in `decompile()`.
Callers in-tree are `tests/utils/gpu_scanner_tests.cpp`.

## CUDA acceleration layer (`RETDEC_ENABLE_CUDA_ACCEL=ON`)

Separate from `gpu_scanner`. Nothing in `src/retdec` includes
`retdec/cuda_accel/*`.

| Component | Role |
|-----------|------|
| [`src/cuda_accel/`](../src/cuda_accel/) | Experimental `CUDAContext`, kernel cache, optional profiling |
| [`src/opencl/`](../src/opencl/) | Experimental OpenCL backend — **not** added from `src/CMakeLists.txt` |
| [`src/neural/`](../src/neural/) | Opt-in llama.cpp refine. GPU offload is `n_gpu_layers` / `RETDEC_NEURAL_GPU_OFFLOAD`, not this tree |
| GUI **CUDA** settings tab | Device index, block size, kernel cache — persisted GUI state, not a pipeline hook |

These do **not** run in the default LLVM → C pipeline.

## Host-side CUDA / OpenCL recovery (`src/ptx_decompile/`)

This is **decompilation of host binaries that call GPU APIs**, not running
the decompiler on a GPU.

- **`CudaHostRecovery`** — `KernelLaunchDetector` (`cudaLaunchKernel`,
  `cuLaunchKernel`, `cudaConfigureCall`), memory / device / stream / NVCC
  stub detectors. Unit tests in `tests/ptx_decompile/`. **Not** instantiated
  in `src/retdec/retdec.cpp`.
- **`OclHostRecovery`** — **is** instantiated after semantic export. Skips
  when `RETDEC_OCL_HOST=0` or the module has no `cl*` API names. On a hit,
  `OclHostEmitter` prints a log summary; it does not rewrite `.c`.
- **PTX parser/lifter** — CUDA-C strings from PTX text. No
  `retdec-decompiler` input path for `.ptx`.
- **SASS / cubin / fatbin** — `src/sass_decode/` loads ELF `EM_CUDA` (190)
  cubin and fatbin (`0xBA55ED50`), decodes a **documented SM_70/SM_80
  subset**, and emits CUDA-C comments + a few operators. **Not** in
  `decompile()`. **Not Production.** SASS is not in Capstone; `nvdisasm`
  is not used. Integrator patches: [internal/wire-sass.md](internal/wire-sass.md).
  CUDA Binary Utilities 12.8 / 13.x print encoding words for EXIT, NOP,
  BRA, IMAD, IMAD.WIDE, MOV, LDG, STG, IADD3, FADD, S2R, LDC, SHFL, S2UR,
  LDCU. They **do not** print 16-byte words for FMUL, FFMA, ISETP, SHL,
  SHR, LOP3, FSETP, MUFU, or LEA (mnemonics / ISA tables only). Those
  stay `Unknown`. Do not invent opcodes.

## CPU-only (main decompile pipeline)

Always CPU, regardless of CUDA flags:

- LLVM bitcode lifting (`bin2llvmir`)
- llvmir2hll C emission
- Managed-language routes (`managed_decompiler`)
- Post-pipeline semantic detectors
- Unpacker, fileformat, YARA static-code detection

## Quick reference

```
Binary input
    │
    ├─► GpuScanner                    linked, never called from decompile()
    │
    ├─► Managed dispatcher            CPU
    │
    └─► LLVM pipeline + llvmir2hll    CPU
            │
            └─► Post-pipeline analysis CPU
                    ├─► OclHostRecovery (log)
                    ├─► CudaHostRecovery (not called)
                    └─► llama.cpp refine (opt-in) / parked cuda_accel
```

## Related docs

- [architecture.md](architecture.md)
- [ARCHITECTURE_TARGETS.md](ARCHITECTURE_TARGETS.md) — SASS library, not Production
- [internal/wire-sass.md](internal/wire-sass.md)
- [BUILD_REFERENCE.md](BUILD_REFERENCE.md)
- [NEURAL_REFINEMENT.md](NEURAL_REFINEMENT.md) — `n_gpu_layers` offload
