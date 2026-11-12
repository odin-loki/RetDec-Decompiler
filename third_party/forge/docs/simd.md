# simd: CPU features and kernel dispatch

## Concepts
`forge::simd` detects what the CPU supports and what the OS has enabled (XGETBV state for AVX/AVX-512, the Linux AMX tile permission, the SVE vector length, …). It then picks **one table of kernel function pointers** for the process. The choice is made once, on the first call to `active()`; every later call goes through that table.

Kernels are written once and compiled per target:

| Layer | Header | Contents |
|-------|--------|----------|
| Tier P | `vec.hpp` | Portable fixed-width vectors and masks over native registers |
| Tier A | `algo.hpp`, `qdot.hpp` | Algorithms written against Tier P (dot, softmax, GEMM, quantised-row decode and dot, …) |
| Tier R | `forge/simd/raw/x86.gen.hpp` (generated) | Raw x86 intrinsic wrappers from `ops/intrinsics/x86.json` (`ops/gen/intrin_gen.py`). Each wrapper requires its CPU feature, so using one the target lacks is a compile error |
| Capabilities | `caps.hpp` | Feature tags; a target is a set of features |
| Maths | `math.hpp`, `math_coeffs.hpp` | Vectorised libm. The erf/erfc/lgamma coefficients come from `ops/gen/fit_math.py` (run by hand; the output is checked in) |

`simd/src/` builds one translation unit per target, each with its own flags and namespace, into the static library `forge_simd_mv`.

Targets per architecture:

| Arch | Targets (preferred first) | Requirements |
|------|---------------------------|--------------|
| x86_64 | `avx512`, `avx2`, `scalar` | `avx2`: the x86-64-v3 set (SSE2, SSE4.2, POPCNT, AVX, AVX2, FMA, F16C, BMI2). `avx512`: v3 plus AVX-512 F, BW, DQ, VL, CD |
| aarch64 | `sve`, `neon`, `scalar` | `sve`: a table built for exactly the hardware vector length (128, 256, 512 or 1024 bits) |
| riscv64 | `rvv`, `scalar` | `rvv`: a table built for exactly the hardware VLEN (128, 256, 512 or 1024 bits) |
| other | `scalar` | |

SVE and RVV tables are built only for the lengths the compiler accepts (CMake prints `SVE kernel tables:` / `RVV kernel tables:`).

Matrix tile units are separate (`tile.hpp`, namespace `forge::simd::tile`). They compute C = A·Bᵀ like `gemm_nt` and return `false` when the unit is absent, the OS refuses tile state, or the compiler could not build them. Callers then fall back to the vector GEMM.

| Function | Unit | Accuracy |
|----------|------|----------|
| `amx_gemm_bf16` | Intel AMX | Inputs rounded to bf16; f32 accumulate (relative error about 2⁻⁸·√k) |
| `amx_gemm_s8` | Intel AMX | int8 × int8 → int32, exact |
| `sme_gemm_f32` | Arm SME | f32 outer products, same class as `gemm_nt` |

`amx_available()`, `sme_available()` and `sme_vl_bytes()` report the units.

## API (`simd/include/forge/simd/dispatch.hpp`)

| Function | Returns |
|----------|---------|
| `const cpu_features& detect()` | Cached feature flags (`avx2`, `avx512f`, …, `amx_permitted`, `sve_bytes`, `sve_hw_bytes`, `rvv_bytes`, `hybrid`, `vendor`, `brand`). `.summary()` is the one-line description `forge info` prints |
| `const kernels& active()` | The table for this process. `FORGE_SIMD_TARGET` selects a table by name if it is usable here; otherwise the best usable target wins |
| `const kernels* by_name(const char*)` | A specific table, or `nullptr` if it is not built or not usable here. Names: `scalar`; `avx2`, `avx512` (x86); `neon`, `sve` (aarch64); `rvv` (riscv64) |

`kernels` fields:

| Field | Operation |
|-------|-----------|
| `target` | Table name |
| `saxpy`, `dot`, `sum` | y = a·x + y; dot product; sum |
| `softmax_row`, `rms_norm_row`, `logsumexp` | Row operations |
| `exp`, `log`, `tanh`, `sigmoid`, `silu`, `gelu` | Element-wise maths over arrays |
| `silu_mul`, `add`, `sub`, `mul` | Binary element-wise |
| `gemm_nt`, `gemm_nn` | C = A·Bᵀ and C = A·B, row-major |
| `math1(fn, …)`, `math2(fn, …)` | Extended maths by id: `fn1` = exp2, expm1, log2, log10, log1p, sqrt, rsqrt, cbrt, sin, cos, tan, asin, acos, atan, sinh, cosh, erf, erfc, gelu_erf, softplus, lgamma; `fn2` = pow, atan2, hypot |
| `qdecode`, `qdot` | Decode a quantised row (q8_0, q4_k, q6_k, q5_k; `simd::qfmt`) or dot it against f32 activations without materialising it. Other codecs use the scalar decoder passed in. Results are bit-identical to `dot(x, decode(row))` |

## Integer matvec (`qdot_i8.hpp`)
The CPU quantised matmul (`graph::qweights::gemm_nt`) quantises each activation row once to int8 (blocks of 256 with int16 sums of 16 and 32 for q4_k/q5_k/q6_k; blocks of 32 for q8_0) and forms exact int32 sums per weight block, the K-quant sub-scales folded in. Only one or two `fma`s per block are floating point, in the same order on every ISA, so the `scalar` reference, `avx2` (maddubs + madd), `avxvnni` and `avx512vnni` (vpdpwssd / vpdpbusd on ymm) tables give identical bits, and a row's result does not depend on how many rows ran with it (1..8 vectors share one pass over the weights). `qi8_active()` picks the table once (`FORGE_QI8_TARGET` overrides; `FORGE_SIMD_TARGET=scalar|avx2` also caps it); `FORGE_CPU_INT8=0` returns to the f32 path. Tests: `qdot_i8_matches_reference_on_every_isa` (memcmp of every table against the reference, batch vs single vector, error vs f32 below 1% of sum |x·w|). `avxvnni` is compiled but not run here (no hardware). `forge bench --kernels <MB>` prints GB/s per format.

## Example
`docs/examples/simd_dispatch.cpp` (ctest `doc_example_simd_dispatch`):

```cpp
std::printf("cpu: %s\n", forge::simd::detect().summary().c_str());
const forge::simd::kernels& k = forge::simd::active();   // FORGE_SIMD_TARGET overrides
std::vector<float> x(1000, 1.f), y(1000, 2.f);
k.saxpy(3.f, x.data(), y.data(), x.size());               // y = 3*x + y
float d = k.dot(x.data(), y.data(), x.size());            // 5000
```

## Environment

| Variable | Effect |
|----------|--------|
| `FORGE_SIMD_TARGET` | Force a table (`scalar`, `avx2`, …). Ignored if that table is not usable on this CPU |
| `FORGE_LANE_TARGET` | Old name of `FORGE_SIMD_TARGET`, still read (after it) |
| `FORGE_SVE_KEEP_VL=1` | aarch64 Linux: do not lower the SVE vector length at start-up (see below) |

## Platform notes and limits
- Link `forge_simd_mv` whenever you use simd, directly or through gpu, graph or inference.
- aarch64 Linux: if the hardware SVE length has no table (not a power of two, or above 1024 bits), a start-up constructor lowers the process VL with `prctl(PR_SVE_SET_VL)` to the largest length that has one. With `FORGE_SVE_KEEP_VL=1` the length is kept, `sve` is unusable, and NEON is used.
- Run so far: x86_64 scalar/AVX2/AVX-512 natively. SVE (128–512 bits) and RVV (128–1024 bits) only under qemu-user. AMX natively on a Xeon and under Intel SDE. SME only under qemu. See [STATUS.md](STATUS.md).
- AMX on the Xeon VM failed intermittently. The cause was the VM losing tile registers across context switches, not FORGE (`tools/amx_state_probe.cpp` shows it with no FORGE code). `test_tile` retries before failing.
