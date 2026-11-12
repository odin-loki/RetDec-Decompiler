# graph: graph IR, compilation and execution

## Concepts
graph treats a tensor graph as a program that is compiled once and replayed:

1. **Build** a `tensor_graph` (or let [inference](inference.md) build one from a model's blueprint).
2. **Optimise** (`passes.hpp`): verify shapes, then CSE, constant folding, algebraic simplification, dead-code elimination, and fusion. The only fusion today is `silu(a) * b` → `silu_mul(a, b)` when `silu(a)` has no other users.
3. **Plan memory** (`memplan.hpp`): liveness analysis gives static arena offsets, view aliasing (`reshape`) and in-place reuse, so peak memory is known before running.
4. **Lower** each op to an implementation from a lowering registry: CPU by default, or a device registry (Vulkan, CUDA, HIP, OpenCL, Level Zero, Metal, RPC, or a multi-device pipeline).
5. **Replay** the flat plan with an `executor`. inference caches plans per shape bucket (`plan_cache`).

A lowering is used only if it agrees with the float64 scalar reference in `ops/` within the op's declared tolerance (the oracle, `verify/`). The check runs the first time the lowering is selected; a lowering that fails is disabled with a warning. Ops or shapes a device does not support run on the CPU. On a device run, `FORGE_PLAN_TRACE` lists those host ops at exit.

Quantised weights (`tensor_graph::qweight`) stay encoded. On the CPU, `matmul_t` and `gather` read the encoded rows in place, so a q4_k model costs its file size in memory. On a device, see [Device execution](#device-execution).

## API (`graph/include/forge/graph/*.hpp`)
`graph.hpp` includes the IR, passes, codecs, memory planner, kernels, plans and the device registries. `autodiff.hpp`, `kv_codec.hpp` and `rpc.hpp` are included separately.

| Item | Header | Notes |
|------|--------|-------|
| `tensor_graph` | `ir.hpp` | `input(name, shape)`, `weight(name, shape, data)`, `qweight(name, shape, bytes, codec)`, `constant(tensor)`, `scalar(v)`, `apply(op, {inputs}, attrs)` (op enum or name), `state_append(slot, x)`, `state_op(slot, fn, {inputs}, attrs)`, `output(name, value)`; helpers `add`, `mul`, `matmul`. `to_string(g)` prints it |
| `verify_graph(g)` | `passes.hpp` | Infers shapes; errors name the node (`node %2 (matmul)`) |
| `optimise(g, pass_options)` | `passes.hpp` | `pass_options{cse, fold, simplify, fuse}` all default to `true`; `lowerings` is the registry (`nullptr` = CPU) |
| `compile(g, state_store* = nullptr, pass_options = {})` | `plan.hpp` | → `std::unique_ptr<plan>`; `plan::describe()` prints it |
| `executor(plan&, state_store* = nullptr)` | `plan.hpp` | `set_input(name, data)`, `run()`, `output(name)` (tensor copy), `output_view(name)` |
| `state_store` | `plan.hpp` | Per-session slots: KV history (`state_append`) and fixed-size state (`state_op`). `reset()`, `truncate(len)`, paged growth (`page_rows`), KV codec, host offload |
| `state_op` kernels | `state_ops.hpp` | `causal_conv` (depthwise causal conv1d) and `delta_rule` (gated delta rule recurrence), for Gated DeltaNet layers (Qwen3.5) |
| `grad(g, loss, wrt)` | `autodiff.hpp` | Reverse-mode autodiff. Appends the backward graph and returns a map from each `wrt` value to its gradient. Supported ops: add sub mul div neg scale square sqrt rsqrt recip exp log tanh sigmoid silu relu abs, matmul (rank-2 b), matmul_t, transpose, reshape, reduce_sum, reduce_mean, logsumexp, softmax, rms_norm. Others raise `errc::unsupported` |
| `codecs::find(name)`, `codecs::all()` | `codecs.hpp` | Weight codecs: `f32 f16 bf16 fp8_e4m3 fp8_e5m2 q8_0 q4_k q5_k q6_k q4_g32 q8_g32 mxfp8 mxfp6 mxfp4 mxint8 nvfp4 tq2` |
| `kv_codec_from_name`, `kv_codec_pair_from_name` | `kv_codec.hpp` | KV cache codecs, below |
| `device_registry(name, &why)` | `vulkan.hpp` | Lowering registry for a device string, or `nullptr` (CPU) with the reason in `why` |
| `rpc::open(addr, &why)`, `rpc::server`, `rpc::client` | `rpc.hpp` | Remote device over TCP |

### KV cache codecs

| Name (aliases) | Storage | Runs on |
|----------------|---------|---------|
| `f32` (`""`, `f16`, `none`) | f32 | all |
| `int8` (`i8`, `q8`) | symmetric int8, one f32 scale per (row, head) | CPU, Vulkan, CUDA/HIP |
| `fp8` (`fp8_e4m3`, `e4m3`) | E4M3 (saturating), one f32 scale per (row, head) | CPU, Vulkan, CUDA/HIP |
| `tbq4` (`tbq4_0`, `turbo4`), `tbq3` (`tbq3_0`, `turbo3`) | TurboQuant: Walsh-Hadamard rotation, then 4- or 3-bit Lloyd-Max codes and one gain per head | CPU; GPU up to head dim 256 |
| `tbq4p`, `tbq3p` (`*_qjl`, `turbo*p`) | TurboQuant with a 1-bit QJL sketch of the residual | CPU only |
| `k8v4` | int8 keys, tbq4 values (`kv_codec_pair_from_name` only) | as its parts |

The attention kernels decode quantised slots on the fly. inference selects the codec with `session_options.kv_codec` or `FORGE_KV_CODEC` ([inference.md](inference.md)).

### Device strings

| String | Registry |
|--------|----------|
| `cpu` or empty | `nullptr`: CPU lowerings |
| `vulkan` | `vk.<op>` lowerings |
| `cuda[:N]`, `hip[:N]` | `cuda.<op>` / `hip.<op>`, plus device quantised matmul and gather |
| `opencl[:N]`, `ze[:N]` / `level_zero[:N]`, `metal[:N]` | `<backend>.<op>` |
| `rpc:host:port` | ops run by a remote `forge rpc-serve` |
| `pipe:<dev>[*w],<dev>[*w],...` | Split contiguous layer ranges across devices, balanced by cost × weight `w` (e.g. `pipe:vulkan*3,cpu`, `pipe:cuda:0,cuda:1`) |

Device lowerings have priority 20, above the CPU ones. The backends themselves are described in [gpu.md](gpu.md).

## Device execution
A resident device registry carries `device_hooks` (`kernels.hpp`) that the executor calls: `begin_run`/`end_run`, `pin` (weights and constants, once), `to_host`, `host_wrote`, `release`, device-side `copy`/`copy2d` (reshape and concat stay on the device), and optionally `arena`, `keep`, `end_run_discard`, `state`, `ran`/`replay`, `argmax` and `fused`. With `fused`, `compile` marks adjacent step pairs the backend may run as one kernel (`fuse_steps`: a residual `add` followed by the `rms_norm` of its result; an `rms_norm` whose only consumer is the next step, a `rope`); the executor offers each pair to the hook and runs the two steps as usual when it declines. Which hooks a backend provides, and the environment variables that turn them off, are in [gpu.md](gpu.md#environment).

Quantised weights on a device:
- CUDA and HIP keep q8_0, q4_k, q5_k and q6_k weights encoded on the device and run them with their own kernels.
- Other resident backends get an f32 copy, decoded on the host and mirrored on the device, up to `FORGE_GPU_WEIGHT_BUDGET` bytes (default `8G`; K/M/G suffixes). Weights beyond the budget stay encoded and their `matmul_t`/`gather` run on the CPU.

Sparse MoE (`moe_ffn(h, router_logits, gate[e,f,d], up[e,f,d], down[e,d,f], k, norm, scale, gating)`): each token is routed to its top-k experts; the (token, slot) pairs are grouped by expert and only the selected experts' slices of the stacked (encoded) weights are read, with the same SIMD quantised dot / row-decode kernels as `matmul_t`; tokens are combined in slot order (deterministic). Expert weights always stay encoded. There is no device kernel yet: on a device run `moe_ffn` runs on the host (`registry::device_moe` is the hook for a grouped device kernel).

On the CPU, `FORGE_QDOT=0` replaces the SIMD quantised-row kernels with plain decode + dot (for benchmarking and bisecting).

## Example
`docs/examples/graph_build.cpp` (ctest `doc_example_graph_build`) checks `silu(1)·exp(1)` and that the gradient at x = 0 is 0.5:

```cpp
tensor_graph g;
auto x = g.input("x", {2, 4});
auto y = g.mul(g.apply(ops::op::silu, {x}), g.apply(ops::op::exp, {x}));   // fused to silu_mul
g.output("y", y);
auto flat = g.apply(ops::op::reshape, {y}, {{"shape", std::vector<i64>{8}}});
auto loss = g.apply(ops::op::reduce_sum, {flat}, {{"axis", 0.0}});
auto grads = grad(g, loss, {x});
g.output("dx", grads[x]);
auto plan = compile(g);
executor ex(*plan);
ex.set_input("x", in);
ex.run();
tensor out = ex.output("y");
```

## RPC device
`rpc:host:port` sends each op to a server started with `forge rpc-serve [--port 7000] [--bind 127.0.0.1] [--device cpu]`. The server runs it with its own device registry. Every op is still oracle-checked on the client. Weights travel with every op (no remote residency).

Optional TLS (mTLS too) and token or Ed25519 key authentication are available. The client reads its credentials from `FORGE_RPC_KEY`, `FORGE_RPC_TOKEN`, `FORGE_RPC_TLS=1`, `FORGE_RPC_CA`, `FORGE_RPC_FINGERPRINT`, `FORGE_RPC_SERVER_NAME`, `FORGE_RPC_TLS_CERT` and `FORGE_RPC_TLS_KEY`. A server with no tokens or keys accepts anyone, so bind it to loopback. Details: [rpc.md](rpc.md).

## Platform notes and limits
- Link `forge_graph`, `forge_verify` and `forge_simd_mv`.
- Device lowerings have run on an NVIDIA RTX 3090 (CUDA), Mesa lavapipe and SwiftShader (Vulkan), and mock drivers (HIP, OpenCL, Level Zero). Metal has not been compiled. See [gpu.md](gpu.md).
- Pipeline splits have only been run with CPU and lavapipe stages, not across real GPUs.
