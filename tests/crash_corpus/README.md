# Crash corpus (Part 10.3)

Fuzzer crashes are ingested here as permanent regression tests.

## Layout

One directory per fuzz target, named after the target, holding the inputs that
target must survive. The name is what seeds the replay, so it has to match:

```
tests/crash_corpus/
  README.md
  apk/  cil/  dex/  jvm/  jar/  lattice/  lua/  pdb/  pelib/  pyc/  wasm/
        # the dependency-free targets in scripts/standalone_fuzz.sh's TARGETS
  fuzz_elf/  fuzz_macho/
        # the libFuzzer targets in .github/workflows/fuzz-pr.yml, which link
        # retdec::fileformat and so cannot run in the standalone path
```

## Ingest a crash

```bash
bash scripts/ingest_fuzz_crash.sh path/to/crash.bin fuzz_elf
```

The second argument is the directory name, which is the target name.

## How these are replayed

Nothing needs a per-file ctest. `scripts/standalone_fuzz.sh --replay` runs
every input under the standalone targets' directories, and the `fuzz-libfuzzer`
job seeds `fuzz_elf/` and `fuzz_macho/` into the corpus it hands libFuzzer --
which runs every corpus file before it starts mutating, so a committed
reproducer is replayed first.

## Hand-written entries

Most files here are libFuzzer's own output, named `crash-`, `oom-` or
`slow-unit-` followed by the input's SHA-1. One is not:

`fuzz_elf/handcrafted-dynamic-sh_size-larger-than-file.elf` is a 224-byte
ELF64 with no program headers and one `SHT_DYNAMIC` section header whose
`sh_size` is `0x6d616e79642e0079` -- the value libFuzzer produced when it found
that `ElfFormat::loadDynamicSegmentSection()` passed `sh_size` to ELFIO's
`section::load()`, which allocates that many bytes. `readelf -S` says "Size of
section 1 is larger than the entire file!". It is hand-written rather than
libFuzzer's blob so that what it tests is legible.

## CI

Weekly `sanitizers.yml` runs ASan over `tests/decompile_samples` corpus.
Promote any ASan findings into this directory.
