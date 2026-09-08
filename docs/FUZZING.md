# Fuzzing the untrusted-input parsers

Everything a decompiler reads is hostile by assumption. `scripts/standalone_fuzz.sh`
builds and runs libFuzzer harnesses for the parsers that read attacker-controlled
bytes, using nothing but `clang++`.

```bash
./scripts/standalone_fuzz.sh --replay        # deterministic regression, ~1 min
./scripts/standalone_fuzz.sh                 # replay, then fuzz each target
./scripts/standalone_fuzz.sh pyc wasm        # only these
./scripts/standalone_fuzz.sh --time 600      # seconds per target
./scripts/standalone_fuzz.sh --list
```

Requires `clang++` **and its sanitizer runtime** — on Debian/Ubuntu that is the
matching `libclang-rt-<N>-dev` package, plus `zlib1g-dev` for the archive
readers. The script says so if the runtime is missing rather than failing at
link time.

## Targets

| Target | Parser | Reads |
|---|---|---|
| `pyc` | `pyc_parser` | CPython bytecode and its marshal stream |
| `lua` | `lua_parser` | Lua bytecode chunks |
| `wasm` | `wasm_parser` | WebAssembly modules |
| `dex` | `dex_parser` | Android DEX |
| `apk` | `dex_parser` | APK archives |
| `jvm` | `jvm_parser` | Java `.class` |
| `jar` | `jvm_parser` | JAR archives |
| `pdb` | `pdbparser` | Microsoft PDB |
| `cil` | `cli_parser` | .NET CIL metadata |
| `pelib` | `pelib` | PE headers and every data directory |
| `lattice` | `fileformat/lattice` | The signature-lattice format detector — the first structured read of an untrusted file |

ELF, Mach-O and the unpacker harnesses are **not** here: they link
`retdec::fileformat`, which publicly links LLVM. Those stay on the
`-DRETDEC_FUZZ=ON` path in `.github/workflows/fuzz-pr.yml`. `fuzz_pe.cpp` is on
that path too — but PeLib itself is not, which is why `pelib` above drives
`PeLib::PeFileT` directly. It is 9,791 lines that read attacker-controlled bytes
and had neither a unit suite nor any fuzzing.

`lattice` is here for the mirror-image reason. `src/fileformat/lattice/` is the
one translation unit under `src/fileformat/` that links no LLVM, so `fuzz_pe.cpp`
on the RETDEC_FUZZ path cannot reach it and this driver can. It is the
decompiler's first structured read of a file, before any loader is chosen.

## The two modes, and why they are separate

**`--replay`** runs every seed and every reproducer committed under
`tests/crash_corpus/<target>/` with `-runs=0`. No mutation, so the result is the
same on every machine and every run. That is what gates a pull request: it
cannot be flaky, and it fails if a previously fixed crash comes back.

**Discovery** (the default) mutates. It may find something new, which means it
may fail for a reason unrelated to the change under test — so it runs on a
schedule, not on pull requests. A find is written to
`tests/crash_corpus/<target>/` via `-artifact_prefix`, and the job uploads it.
**Commit the reproducer**: it turns a one-off find into a permanent regression
case that `--replay` will check forever.

### A caveat about out-of-memory artifacts

There are two kinds of OOM and they behave differently on replay.

A **single oversized allocation** — the parser calling `reserve()` on a count it
read out of the file — is caught by `-malloc_limit_mb`, fires on one execution,
and replays deterministically. That is the class that matters most here, and it
is why replay passes `-malloc_limit_mb=512` rather than relying on the RSS limit
alone. Without it, an OOM reproducer replays clean and the whole class goes
ungated.

An **RSS-limit** OOM is cumulative: libFuzzer notices total memory growth and
saves *the last unit it ran*, which is not necessarily the one responsible. Such
an artifact is a useful corpus seed but not a reproducer, and replaying it
proves nothing. Do not treat a saved artifact as a demonstrated bug until you
have watched it fail on its own:

```bash
./build/fuzz/bin/<target> <artifact> -rss_limit_mb=2048 -malloc_limit_mb=512
```

### A hang is a finding, and it needs its own bound

libFuzzer's default `-timeout` is 1200 seconds. That does gate hangs, but at
twenty minutes an input, so a regression run that trips one looks like CI
wedging rather than like a failure — and a hang gates nothing anybody waits for.
Replay and fuzzing both pass `-timeout=25` (override with `FUZZ_TIMEOUT`). A
parser that cannot decode a few kilobytes in that long is not slow, it is
looping.

This is not hypothetical: an infinite loop reached `PDBSymbols::parse_symbols`
on this branch, introduced by a *security* fix. The guard was written as
`if (!record_name_terminated(...)) continue;` inside a walk whose position
advance sits at the bottom of the loop, so `continue` skipped it and the walk
spun on one record forever — a hang where there had been an over-read, which is
not an improvement. The corpus entry caught it; a hand check for sanitizer
output did not, because a hang produces none.

### `-max_len` is not a tuning knob

libFuzzer defaults to 4096-byte inputs, and that default hides whole bug classes
rather than merely slowing their discovery.

A Python marshal container costs two bytes per nesting level, so 4 KB caps
nesting at about 2000 — comfortably survivable. The stack overflow that a 30 KB
file triggers is unreachable at the default no matter how long the fuzzer runs,
and 1.4 million executions duly reported nothing. Both modes therefore pass
`-max_len=65536`, and replay passes it too so a large committed reproducer is
not silently skipped.

Some things stay out of reach even so: the Python 3.11 line-table accumulator
needs roughly a 34 MB input to overflow. That one is covered by a unit test
instead. Fuzzing is not a substitute for reading the code.

### The gate does not skip

A target listed in `TARGETS` whose harness source has gone missing is a failure,
not a skip — the whole reason this script exists is that a presence check did
not notice `fuzz_dex.cpp` had stopped compiling. Retire a target by deleting its
row, deliberately. A target with no seeds and no reproducers is reported as
ungated rather than printed green, since replaying nothing proves nothing.

## Seeds

Each target is seeded from its fixtures under
`tests/managed_integration/fixtures/`, plus everything in
`fixtures/malformed/`. The malformed fixtures matter more than the valid ones —
they already sit on the error paths, which is where the bugs are.

But there has to be at least one *valid* seed, or the fuzzer spends its whole
budget rediscovering the file's magic number. The `pelib` target proved that the
hard way: with no PE anywhere in the tree it ran 423,000 executions and found
nothing, because almost every input was rejected at the `MZ` check. Three
minimal PEs later — built by `fixtures/pe/make_pe_corpus.py`, so a reader can
see what each one is rather than trusting a blob — it found a bug within
minutes, then two more.

If you add a target, add a valid seed with it.

The corpus lives in `build/fuzz/corpus/<target>/` and is cached between CI runs,
so coverage accumulates rather than restarting from the seeds every night.

## What it has already found

Within the first few minutes of its first run:

| Finding | Where |
|---|---|
| Out-of-memory: `reserve()` on a tuple count read straight from the file — five bytes can claim 2³¹ elements | `src/pyc_parser/py_marshal.cpp` |
| Crashes in eight of the nine targets on the first four-minute-per-target run; only the CIL parser survived, at 48 million executions | across the parsers |
| Heap-buffer-overflow: the 3.11 line-table decoder checked for one byte and read two, and for two and read three | `src/pyc_parser/py_code_object.cpp` |
| `fuzz_dex.cpp` did not compile — it called a `DexFile::classDefsSize()` that no longer exists | `tests/managed_integration/fuzz/` |
| Stack exhaustion from 15,000 levels of marshal nesting in a 30 KB file — invisible at the default `-max_len` | `src/pyc_parser/py_marshal.cpp` |
| Misaligned `uint32_t`/`uint16_t` loads at a file-controlled offset | `src/pelib/ImageLoader.cpp` |
| `fileSize()` returning `tellg()`'s `-1` as an unsigned size, so every bounds check downstream passed | `src/pelib/PeLibAux.cpp` |
| A 32-bit `offset + size` that wraps, then allocating the unbounded size | `src/pelib/SecurityDirectory.cpp` |
| Non-zero offset applied to a null pointer on an absent relocation directory | `src/pelib/RelocationsDirectory.cpp` |

That last one is the reason this script exists. The pull-request job could only
check that the harness *files were present*, so a harness that had stopped
compiling looked exactly like a healthy one.

## Writing a new harness

Add `LLVMFuzzerTestOneInput` under `tests/managed_integration/fuzz/`, then a row
in the `TARGETS` table in `scripts/standalone_fuzz.sh`:

```
name : harness stem : src modules : C++ standard : link libs : seed glob
```

The modules must be dependency-free — `scripts/standalone_check.sh --list` shows
which ones are. A harness that needs LLVM belongs in the CMake `RETDEC_FUZZ`
path instead.

The contract is the usual one: **any input at all, no crash.** Rejecting
malformed input is correct behaviour; aborting, over-reading, or allocating what
the file claims is not.
