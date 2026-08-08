# Crash corpus (Part 10.3)

Fuzzer crashes are ingested here as permanent regression tests.

## Layout

```
tests/crash_corpus/
  README.md
  ingest_fuzz_crash.sh
  pe/           # PE crash inputs (not committed until found)
  elf/
  macho/
```

## Ingest a crash

```bash
bash scripts/ingest_fuzz_crash.sh path/to/crash.bin pe
```

Each ingested file should be referenced from a ctest that runs the decompiler
with a timeout and expects clean exit (no ASan fault).

## CI

Weekly `sanitizers.yml` runs ASan over `tests/decompile_samples` corpus.
Promote any ASan findings into this directory.
