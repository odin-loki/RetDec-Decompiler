# Using PDB debug files with RetDec (Windows)

Imortek **2.0.22**. RetDec does **not** download PDBs, does **not** speak
HTTP symbol servers, and does **not** read `_NT_SYMBOL_PATH`.

The CLI flag is a **regular file**, not a directory:

```
[-p|--pdb FILE] File with PDB debug information.
```

`ProgramOptions::checkFile` requires `std::filesystem::is_regular_file`.
A folder path fails with `[-p|--pdb] bad file: …`.

## What the code actually loads

Live path:

1. CLI stores the path on `config.parameters` (`setInputPdbFile`).
2. `src/debugformat/debugformat.cpp` constructs `pdbparser::PDBFile` and
   `load_pdb_file`.
3. `src/debugformat/pdb.cpp` copies types, globals, and functions
   (module name, return type, register / BP-relative locals) into the
   common debug representation used by bin2llvmir.

That is **`pdbparser` + `debugformat`**. `src/debug_info/pdb_extractor.cpp`
(LLVM `loadDataForPDB`) is a **separate** library (`retdec-debug-info`),
optional `RETDEC_USE_LLVM_PDB`, and is **not** linked from the decompiler.

When a PDB loads, names and types from it can replace `sub_` placeholders
and seed locals. Without a PDB, analysis still runs; names come from
exports, imports, DWARF (if present), and heuristics.

RetDec does not verify that the PDB matches the binary’s build ID beyond
what `pdbparser` can parse. A mismatched file may load partially or fail.

## Option 1 — Local PDB file (supported)

1. Obtain the matching `program.pdb` (build tree, vendor package, or a
   cache you filled with **other** tools).
2. Pass the **file**:

```powershell
retdec-decompiler.exe -o out.c `
  --pdb C:\symbols\MyApp\release\MyApp.pdb `
  C:\samples\MyApp.exe
```

Placing `program.pdb` next to `program.exe` does **not** auto-load it.
You still pass `--pdb` with the file path.

## Option 2 — `_NT_SYMBOL_PATH` / Microsoft symbol server

RetDec does **not** read `_NT_SYMBOL_PATH` and does **not** call
`symsrv`. Prefetch with your own tooling, then pass the resulting **.pdb
file**:

```powershell
$env:_NT_SYMBOL_PATH = "SRV*C:\symbols*https://msdl.microsoft.com/download/symbols"
symchk /v C:\samples\MyApp.exe /s SRV*C:\symbols*https://msdl.microsoft.com/download/symbols

retdec-decompiler.exe -o out.c --pdb C:\symbols\…\MyApp.pdb C:\samples\MyApp.exe
```

A `SRV*` cache directory is not a valid `--pdb` argument.

## Option 3 — Corporate symbol server

Mirror the PDB to disk with an approved fetch tool (do not embed
credentials in RetDec command lines). Pass the mirrored **file** via
`--pdb`.

## GUI

There is no “Analysis → Configure… PDB path directory” control.

**Analysis → Set PDB symbols…** opens a file picker (`PDB (*.pdb)`),
stores `decompilerPdbPath`, and the next F5 decompile adds
`-p <file>` if that path still exists
(`src/gui/mainwindow.cpp`, `src/gui/decompiler_launch.cpp`).

## Verifying symbols were applied

- Function list / decompiled C shows PDB names rather than only `sub_<addr>`.
- `.config.json` sidecar `functions[].name` / `demangledName` populated.
- Decompiler log may mention debug info; wording is not a stable ABI.

If names stay synthetic:

| Issue | Fix |
|-------|------|
| `--pdb` pointed at a directory | Pass the `.pdb` **file** |
| PDB/build mismatch | Use the PDB from the same build |
| Flag omitted | PDB beside the EXE is not auto-discovered |
| Stripped PE and no PDB | Expected — exports/imports only |
| OneDrive/sync locks | Copy binary + PDB to a local folder |

## Security note

PDBs may reveal internal paths and function names. Handle exported
decompilation bundles accordingly.

## See also

- [user_manual.md](user_manual.md) — CLI flags
- [architecture.md](architecture.md) — debugformat vs `src/debug_info`
