# Using PDB Symbol Servers with RetDec (Windows)

RetDec does not download PDBs itself. On Windows you point the toolchain at
symbols that **symstore**, **WinDbg**, or your corporate symbol server already
cached, or at a local directory produced by `symchk` / Visual Studio.

## What RetDec uses symbols for

When debug information is available, RetDec can:

- Recover real function and global names instead of `sub_` placeholders
- Improve type recovery and calling-convention hints
- Align decompiled output with source-level structure (especially with MSVC `/Zi`)

Without PDBs, analysis still runs on stripped PE files; names come from exports,
imports, and heuristics only.

## Option 1 — Local PDB directory (recommended)

1. Obtain matching PDBs (build tree, vendor package, or symbol server dump).
2. Place `program.pdb` next to `program.exe`, **or** in a folder that mirrors
   the original build path.
3. Decompile with an explicit PDB search path:

```powershell
retdec-decompiler.exe -o out.c `
  --pdb-path "C:\symbols\MyApp\release" `
  C:\samples\MyApp.exe
```

The GUI **Analysis → Configure…** dialog exposes the same `--pdb-path` when
present in your RetDec build.

## Option 2 — `_NT_SYMBOL_PATH` (system symbol cache)

Many Windows analysis VMs already set:

```powershell
$env:_NT_SYMBOL_PATH = "SRV*C:\symbols*https://msdl.microsoft.com/download/symbols"
```

RetDec does **not** read `_NT_SYMBOL_PATH` directly. Prefetch PDBs into a local
folder, then pass `--pdb-path`:

```powershell
# Example: copy PDBs with symstore/symchk tooling you already use
symchk /v C:\samples\MyApp.exe /s SRV*C:\symbols*https://msdl.microsoft.com/download/symbols

retdec-decompiler.exe -o out.c --pdb-path C:\symbols C:\samples\MyApp.exe
```

## Option 3 — Corporate symbol server

If your org hosts symbols at `https://symbols.corp.example/v2/symbols`:

1. Mirror needed PDBs to disk with your approved fetch tool (do not embed
   credentials in RetDec command lines).
2. Pass the mirror directory via `--pdb-path`.

Air-gapped labs should import PDBs on removable media and reference the mount
point only.

## GUI workflow

1. **File → Open Binary…** and load the PE.
2. **Analysis → Configure…** — set **PDB path** to the directory containing
   the PDB (or the symstore cache subtree for that module).
3. **Analysis → Run Full Analysis** (F5).
4. Confirm improved names in the **Functions** list and decompiled C.

Optional: run **Inspect → Refresh fileinfo** first to verify compiler/linker
metadata matches the PDB you expect.

## Verifying symbols were applied

- Function list shows meaningful names (not only `sub_<addr>`).
- `.config.json` sidecar `functions[].name` / `demangledName` fields are populated.
- Decompiler log mentions loading debug info (exact wording varies by version).

If names stay synthetic, check:

| Issue | Fix |
|-------|-----|
| PDB/build ID mismatch | Use the PDB from the same build as the binary |
| Wrong `--pdb-path` | Directory must contain the PDB or a matching subtree |
| Stripped + no PDB | Expected — only exports/imports remain |
| OneDrive/sync locks | Copy binary + PDB to a local non-synced folder |

## Security note

Symbol servers and PDBs may reveal internal project paths and function names.
Handle exported decompilation bundles accordingly in threat-intel workflows
(see `retdec_export_intel.py`).

## See also

- [user_manual.md](user_manual.md) — decompiler CLI flags
- [ENGINEERING_ROADMAP.md](ENGINEERING_ROADMAP.md) — Tier 4 security workflows
