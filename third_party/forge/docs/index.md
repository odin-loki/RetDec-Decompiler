# FORGE documentation

Each guide follows the same shape: **concepts**, **API or command reference**, **examples**, and **platform notes and limits**. Anything that has not been run is labelled as such.

## Getting started
- [README](../README.md): what FORGE is, quick start, layout
- [building.md](building.md): requirements, presets, CMake options, targets, native and cross builds
- [testing.md](testing.md): kinds of test, ctest names and labels, how to run them
- [cli.md](cli.md): every `forge` command and flag
- [configuration.md](configuration.md): every environment variable in one place
- [memory.md](memory.md): memory budgets, eviction, admission control and OOM avoidance

## Library layers, bottom up
- [os.md](os.md): `forge::os`, the portability layer and its extension modules
- [os-wrapper.md](os-wrapper.md): the `forge::os` headers beyond the core layer: processes, pipes, signals, file-system extras, users, time, system information, console, networking, file watching, service control, Windows registry, devices, clipboard, dynamic libraries, process pools. Built; the missing parts are listed in the page
- [text-locale.md](text-locale.md): `forge::os::text` and `forge::os::locale` (Unicode, encodings, locales; `forge_text`)
- [mobile.md](mobile.md): `forge::os::mobile`, sensors, touch and device services (`forge_mobile`)
- [support.md](support.md): logger, process wait and leases, thread pool, cache, deadlines, settings registry
- [simd.md](simd.md): CPU feature detection and SIMD kernel dispatch
- [gpu.md](gpu.md): devices, graphs, parallel primitives, GPU runtimes
- [graph.md](graph.md): graph IR, passes, plans, codecs, autodiff, RPC devices
- [modelfile.md](modelfile.md): the `.fpk` container, signing, encryption, importing
- [format.md](format.md): `.fpk` byte layout
- [inference.md](inference.md): models, sessions, samplers, generation

## Serving, remote devices and embedding
- [serve.md](serve.md): `forge-serve` HTTP server
- [loadbalancer.md](loadbalancer.md): load-balancing pool and `forge-serve proxy`
- [rpc.md](rpc.md): `forge rpc-serve`, authentication, TLS and remote scripts
- [capi.md](capi.md): the C ABI (`forge.h`)
- [bindings.md](bindings.md): Python, Kotlin and Swift

## Scripting
- [scripting.md](scripting.md): `forge script`, the REPL, static checks, shell blocks and emitting shell scripts
- [shells.md](shells.md): shell backends and emit targets

## Security
- [security.md](security.md): what is enforced today: script capabilities, OS sandbox, plugins, AI confirmation, RPC auth, signing, known gaps
- [security-standard.md](security-standard.md): draft security standard and the state of each control
- [security-api.md](security-api.md): `forge::security`: policy file and profiles, `enforce`, `safe_join`, audit, `lockdown`, `check`, `forge security check`
- [../SECURITY.md](../SECURITY.md): how to report a vulnerability, threat model

## Reference and project
- [reference.md](reference.md): op table, GPU kernel list and C ABI, **generated** by `tools/gen_docs.py` (do not edit by hand; the `docs_up_to_date` test checks it)
- [../ops/README.md](../ops/README.md): op specs and code generators
- [STATUS.md](STATUS.md): what has been tested where
- [RELEASING.md](RELEASING.md): release procedure
- [../CONTRIBUTING.md](../CONTRIBUTING.md): how to change code and docs, and how to merge
- [../TODO.md](../TODO.md): open work items
- [../FORGE_PLAN.md](../FORGE_PLAN.md): the original design plan (a roadmap, not a description of current behaviour)
- [claims-audit.md](claims-audit.md): historical record of audits of doc claims against the code; not a description of current behaviour
- `wip/dn-scratch.patch`: a saved work-in-progress diff from a scratch directory. It is not applied and uses the old module names (`flow`, `grid`)

## Compiled examples
Each file in `docs/examples/` below is built by CMake and run by ctest as `doc_example_<name>`, so the snippets in the guides cannot silently drift from the API:

```sh
ctest --test-dir build/dev -R doc_example
```

| Example | Guide |
|---------|-------|
| `simd_dispatch.cpp` | simd.md |
| `gpu_graph.cpp` | gpu.md |
| `graph_build.cpp` | graph.md |
| `modelfile_roundtrip.cpp` | modelfile.md |
| `inference_generate.cpp` | inference.md (needs the golden model) |
| `os_features.cpp` | os.md |
| `loadbalancer_pool.cpp` | loadbalancer.md |
| `capi_generate.c` | capi.md (needs the golden model; not built when cross-compiling) |

`docs/examples/script/reflection.cpp` is a `forge script` example for scripting.md; it is not a ctest target.
