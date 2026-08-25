# Provenance

This document records the chain of title for the Imortek RetDec fork.

## Upstream

Upstream is [Avast RetDec](https://github.com/avast/retdec) **v5.0**, licensed under the **MIT License**.

The MIT copyright and permission notice are in `LICENSE-MIT`:

```
Copyright (c) 2017 Avast Software
```

`LICENSE-MIT` must ship with every distribution (source tarball, installer, and `cmake --install` package). The root `CMakeLists.txt` install list includes it. MIT clause 2 requires that the copyright notice and permission notice be retained in all copies and substantial portions of the Software.

## This fork

Modifications in this repository are dual-licensed:

- **AGPL-3.0 or later** — full text in `LICENSE-AGPL`
- **Commercial** — terms in `LICENSE-COMMERCIAL`

See the root `LICENSE` for how to choose a path. `NOTICE` keeps the Avast attribution and points at `LICENSE-MIT`.

Relicensing MIT-origin code under AGPL-3.0+ / commercial is permitted. Stripping the MIT notice is not. This tree restores that notice.

This fork's git history (all refs) contains a single author identity:
`odin-loki <odin-loki@users.noreply.github.com>` (274 commits as of the
Phase 1 `LEG-07` check; no `Co-authored-by` trailers). **Sole author of
fork commits to date; no third-party GitHub contributions.** Upstream Avast
files remain Avast's MIT-licensed work.

## How to classify a file

Look at the file header, not the directory name alone.

| Header | Meaning |
|---|---|
| `@copyright (c) 2017 Avast Software, licensed under the MIT license` plus `@copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)` | Upstream Avast file (or based on one). The 2017 Avast line is the MIT notice. The second line records fork modifications. |
| `Copyright (c) 2004 - 2005 Sebastian Porst` | Third-party PeLib (zlib/libpng). Do not rewrite. |
| `2025-2026` / `2025` / `2026` Imortek only, and **no** 2017 Avast line | Genuinely new Imortek module. Leave as-is. |
| `@copyright (c) 2017 Odin Loch` | Mechanical rewrite tell. **Must not remain.** Restored by `scripts/ci/restore_avast_headers.py`. Guarded by `scripts/ci/check_avast_mit_notice.py`. |

The 2017 date on an Imortek line is the tell: Imortek did not exist in 2017. Those lines were a find-and-replace of the Avast header.

L1 restored every `@copyright (c) 2017 Odin Loch` line under `src/`, `include/`, and `tests/` (never `deps/`, never `build/`). Files that already correctly named Avast were left alone. `src/pelib/` files that retain Sebastian Porst copyright were left alone. New-module files that already said 2025–2026 Imortek only were left alone (L3).

A residual class exists: some known-upstream files use **2019–2020** Odin Loch lines (Avast-era years, same rewrite pattern). L1 restored 2017; Phase 1 restored **2018**. 2019–2020 remain. `scripts/ci/restore_avast_headers.py --year 2019` (then `--year 2020`) is the next slice. `check_avast_mit_notice.py` currently fails on leftover **2017 and 2018** rewrite lines.

## Module origin (`src/`)

Headers were restored by the 2017-rewrite heuristic, not by a per-file upstream diff. A module is **upstream** if it is an Avast RetDec v5.0 component, or if the heuristic restored Avast lines in it. A module is **Imortek-new** if it has no restored 2017 Avast line and is not a known v5.0 directory. Individual files inside an upstream module may still be Imortek additions.

| Module | Origin | Notes |
|---|---|---|
| `llvmir2hll` | upstream | Largest Avast component; 2017 headers restored |
| `bin2llvmir` | upstream | 2017 headers restored |
| `fileformat` | upstream | 2017 headers restored |
| `fileinfo` | upstream | 2017 headers restored |
| `capstone2llvmir` | upstream | 2017 headers restored |
| `capstone2llvmirtool` | upstream | 2017 headers restored |
| `loader` | upstream | 2017 headers restored |
| `ctypes` | upstream | 2017 headers restored |
| `ctypesparser` | upstream | 2017 headers restored |
| `cpdetect` | upstream | 2017 headers restored |
| `unpacker` | upstream | 2017 headers restored |
| `unpackertool` | upstream | 2017 headers restored |
| `utils` | upstream | 2017 headers restored |
| `common` | upstream | 2017 headers restored |
| `config` | upstream | 2017 headers restored |
| `debugformat` | upstream | 2017 headers restored |
| `yaracpp` | upstream | 2017 headers restored |
| `pdbparser` | upstream | 2017 headers restored |
| `patterngen` | upstream | 2017 headers restored |
| `pat2yara` | upstream | 2017 headers restored |
| `rtti-finder` | upstream | 2017 headers restored |
| `stacofin` | upstream | 2017 headers restored |
| `stacofintool` | upstream | 2017 headers restored |
| `llvmir-emul` | upstream | 2017 headers restored |
| `ar-extractor` | upstream | 2017 headers restored |
| `ar-extractortool` | upstream | 2017 headers restored |
| `macho-extractor` | upstream | 2017 headers restored |
| `macho-extractortool` | upstream | 2017 headers restored |
| `bin2pat` | upstream | 2017 headers restored |
| `getsig` | upstream | 2017 headers restored |
| `idr2pat` | upstream | 2017 headers restored |
| `pelib` | mixed | Sebastian Porst PeLib retained; Avast-era 2017 additions restored |
| `demangler` | upstream | Avast v5.0; headers use 2018–2019 rewrite years, not restored by L1 |
| `demanglertool` | upstream | Companion tool; no 2017 tell |
| `retdec` | upstream | Pipeline library; `retdec.cpp` is 2019 rewrite (`.orig` still says Avast 2019) plus newer Imortek files |
| `retdectool` | upstream | 2019 rewrite year |
| `retdec-decompiler` | mixed | Upstream CLI (`2020`) plus Imortek-new managed/output files (`2025`–`2026`) |
| `gui` | Imortek-new | Analyst workbench |
| `neural` | Imortek-new | Optional refinement tier |
| `ssa` | Imortek-new | Flag-bundle / SSA layer |
| `algo_recover` | Imortek-new | Algorithm recovery |
| `crypto_detect` | Imortek-new | Crypto constant / structure detection |
| `concurrency_detect` | Imortek-new | |
| `serdes` | upstream | JSON (de)serialisers; 2019 rewrite year restored |
| `sem_decoder` | Imortek-new | |
| `module_cluster` | Imortek-new | |
| `cuda_accel` | Imortek-new | |
| `opencl` | Imortek-new | |
| `ptx_decompile` | Imortek-new | |
| `bc_module` | Imortek-new | Shared bytecode CFG |
| `wasm_parser` | Imortek-new | |
| `lua_parser` | Imortek-new | |
| `pyc_parser` | Imortek-new | |
| `jvm_parser` | Imortek-new | |
| `dex_parser` | Imortek-new | |
| `cil_reconstruct` | Imortek-new | |
| `py_reconstruct` | Imortek-new | |
| `jvm_reconstruct` | Imortek-new | |
| `py_emitter` | Imortek-new | |
| `java_emitter` | Imortek-new | |
| `csharp_emitter` | Imortek-new | |
| `fsharp_emitter` | Imortek-new | |
| `kotlin_emitter` | Imortek-new | |
| `vbnet_emitter` | Imortek-new | |
| `cxx_backend` | Imortek-new | |
| `codegen` | Imortek-new | |
| `alias_analysis` | Imortek-new | |
| `call_conv` | Imortek-new | |
| `cfg` | Imortek-new | |
| `cfg_structure` | Imortek-new | |
| `cli_parser` | Imortek-new | |
| `code_data` | Imortek-new | |
| `compiler_abi` | Imortek-new | |
| `compiler_detect` | Imortek-new | |
| `container_detect` | Imortek-new | |
| `dce` | Imortek-new | |
| `debug_info` | Imortek-new | |
| `eh_reconstruct` | Imortek-new | |
| `experimental` | Imortek-new | Opt-in scaffold (`RETDEC_ENABLE_EXPERIMENTAL_SCAFFOLD`). Empty `task_*_scaffold` stubs; not a product pipeline. |
| `func_boundary` | Imortek-new | |
| `idiom_reconstruct` | Imortek-new | |
| `ipa` | Imortek-new | |
| `loader_sim` | Imortek-new | |
| `mini_emu` | Imortek-new | |
| `packer` | Imortek-new | |
| `pattern_detect` | Imortek-new | |
| `profiling` | Imortek-new | |
| `rtti` | Imortek-new | Distinct from upstream `rtti-finder` |
| `serial_detect` | Imortek-new | |
| `sort_detect` | Imortek-new | |
| `string_detect` | Imortek-new | |
| `testing` | Imortek-new | |
| `type_inference` | Imortek-new | |
| `type_seed` | Imortek-new | |
| `var_recovery` | Imortek-new | |
| `qwen3` / `qwen3_runner` | Imortek-new | Support dirs; no C/C++ of their own |

Matching `include/retdec/<module>/` and `tests/<module>/` trees follow the same origin as `src/<module>/`.

This is a module-level map, not a 5,000-file inventory. File-level truth is the header.

## CI

- `scripts/ci/restore_avast_headers.py` — restore Avast-era rewrite years (`--year 2017|2018|2019|2020`). L1 did 2017; Phase 1 did 2018.
- `scripts/ci/check_avast_mit_notice.py` — fails if `LICENSE-MIT` is missing or if `@copyright (c) 2017|2018 Odin Loch` still exists under `src/`, `include/`, or `tests/`. Wired in `.github/workflows/doc-integrity.yml`.

## Related files

| File | Role |
|---|---|
| `LICENSE` | Dual-licence summary; points at MIT / AGPL / commercial |
| `LICENSE-MIT` | Avast RetDec v5.0 MIT permission notice (**must ship**) |
| `LICENSE-AGPL` | AGPL-3.0 full text |
| `LICENSE-COMMERCIAL` | Commercial terms |
| `NOTICE` | Avast attribution and third-party list |
