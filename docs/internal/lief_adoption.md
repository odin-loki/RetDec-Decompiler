# LIEF adoption plan (step 29)

**Library:** [LIEF](https://github.com/lief-project/LIEF) (Apache-2.0)  
**Goal:** Incrementally replace aging `fileformat` parsers for PE/ELF/Mach-O.

## Strategy

Adopt behind `retdec::fileformat::LiefAdapter` — do not cut over in one PR.

### Phase A (scaffold) — done in v1.1.0

- `include/retdec/fileformat/lief_adapter.h` interface
- `RETDEC_ENABLE_LIEF=OFF` CMake option
- Differential tests: LIEF vs existing parser on corpus binaries

### Phase B — parse-only

- PE section/import table via LIEF for new code paths
- Keep `FormatFactory` as facade

### Phase C — modification

- LIEF write support for unpacker / patch workflows

## Licence

Apache-2.0 — compatible with commercial distribution (include NOTICE).

## raw_pdb companion

Consider **raw_pdb** (BSD-2) for PDB reads instead of LLVM PDB (step 29 adjunct).
