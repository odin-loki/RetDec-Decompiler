# Decompile pass profiles

Registry of named LLVM pass lists for `retdec-decompiler`. Profiles control **bin2llvmir → LLVM opts → llvmir2hll** only; backend HLL optimizers are configured separately in `decompiler-config.json` (`backendEnabledOpts`, `--backend-no-opts`, etc.).

## Index

See [index.json](index.json) for machine-readable metadata. Validate any profile file with:

```bash
python3 scripts/validate_pipeline_json.py path/to/profile.json
```

## Profiles

| ID | File | When to use |
|----|------|-------------|
| **fast** | [fast.json](fast.json) (canonical GUI copy: [llvm_passes_fast.json](../../gui/resources/llvm_passes_fast.json)) | Interactive GUI work, smoke tests, parity `-Fast` benches. Skips some verify/loop passes. Use with `--backend-no-opts` for speed. |
| **balanced** | [balanced.json](balanced.json) | Default stock pipeline — same pass order as install `decompiler-config.json`. Good baseline for CI and regression diffs. |
| **quality** | [quality.json](quality.json) | Same LLVM passes as balanced; intended for jobs where backend readability optimizers stay **enabled** (no `--backend-no-opts`). |

## CLI usage

Named profile:

```bash
retdec-decompiler --profile fast|balanced|quality -o out.c input.exe
```

Resolution order for `--profile`:

1. `$RETDEC_PROFILES_DIR/<profile>.json` if set
2. `<bin>/../share/retdec/profiles/<profile>.json` (install layout)
3. For `fast` only: `<bin>/../share/retdec/llvm_passes_fast.json`

Explicit pass list file (`--llvm-passes-json FILE`). The file may be:

- a **JSON array** of pass name strings (legacy GUI fast preset), or
- a **pipeline document** matching [pipeline_builder_schema.json](../../docs/pipeline_builder_schema.json).

Examples:

```bash
# Fast preset via named profile
retdec-decompiler --profile fast --backend-no-opts --disable-static-code-detection \
  -o out.c input.exe

# Balanced document form
retdec-decompiler -o out.c \
  --llvm-passes-json src/retdec-decompiler/profiles/balanced.json input.exe
```

## Custom profiles

Copy `balanced.json`, edit `passes`, set a unique `name`, and validate:

```bash
python3 scripts/validate_pipeline_json.py my_team.json
retdec-decompiler --llvm-passes-json my_team.json -o out.c binary.exe
```

Pass names must match LLVM/RetDec registered passes (see `RegisterPass` in `src/bin2llvmir/` and `src/llvmir2hll/`).

## Related

- [docs/pipeline_builder_schema.json](../../docs/pipeline_builder_schema.json)
- [docs/PIPELINE_REDESIGN_TODO.md](../../docs/PIPELINE_REDESIGN_TODO.md)
- [examples/decompiler_plugin/](../../examples/decompiler_plugin/) — post-decompile hooks
