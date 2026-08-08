# LIEF adoption plan (step 29)

**Library:** [LIEF](https://github.com/lief-project/LIEF) (Apache-2.0)  
**Goal:** Incrementally replace aging `fileformat` parsers for PE/ELF/Mach-O.

## Strategy

Adopt behind `retdec::fileformat::LiefAdapter` — do not cut over in one PR.

### Phase A (scaffold) — done in v1.1.0

- `include/retdec/fileformat/lief_adapter.h` interface
- `RETDEC_ENABLE_LIEF=OFF` CMake option
- Differential tests: LIEF vs existing parser on corpus binaries

### Phase B — parse-only (v2.0.13)

- `cmake/lief_optional.cmake` wires `find_package(LIEF)` when `RETDEC_ENABLE_LIEF=ON`
- `LiefAdapter::parseSections` implemented with LIEF C++ API
- Install C++ SDK:
  - **Ubuntu 24.04+ (Noble):** no `liblief-dev` in apt — use `bash scripts/install_lief_sdk.sh`
  - **Ubuntu 22.04 (Jammy):** `sudo apt install liblief-dev` (universe, 0.9.0 — older than upstream)
  - Then: `export LIEF_DIR="$(pwd)/deps/lief-sdk/lib/cmake/LIEF"` and `-DRETDEC_ENABLE_LIEF=ON`
- Python eval (no C++ SDK): `bash scripts/setup_eval_venv.sh` → `pip install lief`
- Shadow validation: set `RETDEC_LIEF_SHADOW=1` when decompiling to log LIEF section counts alongside existing parsers

### Phase C — modification

- LIEF write support for unpacker / patch workflows

## Licence

Apache-2.0 — compatible with commercial distribution (include NOTICE).

## raw_pdb companion

Consider **raw_pdb** (BSD-2) for PDB reads instead of LLVM PDB (step 29 adjunct).
