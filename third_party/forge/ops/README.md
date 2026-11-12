# ops: one definition per operation

## Concepts
Every tensor operation is declared once, in a `.fspec` file, with a float64 scalar reference implementation. The other layers are generated from that declaration or checked against it:
- The **graph** IR uses the generated `enum class op` and its info table.
- **Lowerings** (CPU SIMD, Vulkan, CUDA, HIP, OpenCL, Level Zero, Metal, RPC) must agree with the reference within the declared tolerance. The oracle in `verify/` checks each lowering before it is used.
- **GPU kernels** are generated from the same specs, and every generated kernel must name a declared op.

The generated op table is [docs/reference.md](../docs/reference.md) (62 ops, 33 with GPU kernels).

## Layout

| Path | Contents |
|------|----------|
| `ops/defs/*.fspec` | Op declarations: `elementwise` (40), `reduction` (11), `movement` (8), `contraction` (3). The file name is only grouping; the class is declared per op |
| `ops/include/forge/ops/op.hpp` | Metadata types: `op_class`, `shape_rule`, `tolerance`, `fusion`, `op_info` |
| `ops/include/forge/ops/reference.hpp` | Scalar references, `forge::ops::ref::<name>` |
| `ops/intrinsics/x86.json` | Curated x86 intrinsic database (name, types, CPUID feature), maintained with `build_x86_db.py` |
| `ops/gen/fspec_gen.py` | Generates `forge/ops/ops.gen.hpp` and `ops.md` in the build tree |
| `ops/gen/gpu_gen.py` | Generates the GPU kernels: one GLSL compute shader per kernel (Vulkan), `kernels.cu` (CUDA/HIP, plus CUDA/HIP-only quantised, attention and state-op kernels), `kernels.cl` (OpenCL, Level Zero), `kernels.metal` (Metal). Also embeds the compiled images into headers |
| `ops/gen/intrin_gen.py` | Generates `forge/simd/raw/x86.gen.hpp`: C++ intrinsic wrappers gated by CPU feature. Using an intrinsic the target lacks is a compile error |
| `ops/gen/fit_math.py` | Fits the polynomials behind `simd` erf, erfc and lgamma. Run by hand: `python ops/gen/fit_math.py > simd/include/forge/simd/math_coeffs.hpp` |

## The .fspec format
One `op` line per operation. After the name come key/value pairs in any order. `#` starts a comment line.

```
op <name> class <elementwise|reduction|contraction|data_movement|stateful>
   inputs <n> shape <rule> tol <exact|ulp:N|rel:X|abs:X> fuse <yes|no|epilogue>
   cost <flops per output element> [attrs "<comma list>"] doc "<text>"
```

All keys except `attrs` are required. `shape` must name a `shape_rule` from `op.hpp`. Duplicate op names fail the generator.

Examples:

```
op add    class elementwise    inputs 2  shape broadcast  tol exact  fuse yes  cost 1  doc "y = a + b (numpy broadcasting)"
op slice  class data_movement  inputs 1  shape slice      tol exact  fuse no   cost 0  attrs "axis,start,end" doc "x[start:end] along axis"
```

## Generated outputs
CMake runs the generators at build time:

| Target | Generator | Output |
|--------|-----------|--------|
| `forge_ops_gen` | `fspec_gen.py` | `forge/ops/ops.gen.hpp`: `enum class op`, `op_count`, the info table (name, class, arity, shape rule, tolerance, fusion, cost, attrs, doc), `op_from_name`, and the binding of each op to `forge::ops::ref::<name>`. A missing reference is a compile error. Also `ops.md` |
| `forge_gpu_gen` | `gpu_gen.py` | Kernel sources in the build tree; SPIR-V, PTX and AMDGPU images when the tools exist ([gpu.md](../docs/gpu.md)) |
| `forge_raw_gen` | `intrin_gen.py` | `forge/simd/raw/x86.gen.hpp` |

`tools/gen_docs.py` builds `docs/reference.md` from the `.fspec` files, `gpu_gen.py`'s kernel list and the C ABI header. ctest `docs_up_to_date` runs it with `--check`.

## Adding an op
1. Add an `op` line to the right `.fspec` file.
2. Add `forge::ops::ref::<name>` to `reference.hpp`.
3. Optionally add a GPU kernel in `gpu_gen.py` and a SIMD lowering in graph.
4. Rebuild, then run `python tools/gen_docs.py` to refresh `docs/reference.md`.
5. `test_graph` checks the new op: `oracle_matrix_all_ops` runs every op through the full compile pipeline against its reference, and `lowerings_pass_oracle` checks every CPU lowering. Device lowerings are checked by the backend tests (`test_vulkan`, `test_gpu_*`).
