# Confirmed audit findings not fixed here

A 14-dimension subsystem audit of this fork produced 217 findings, 129 of them
high or critical. The ones that could be built and tested without LLVM were
fixed on the branch that produced this file; these could not, so they are
written down rather than attempted.

**Why they are not fixed.** A full build needs the LLVM pin from
`cmake/deps.cmake` -- upstream `llvm-project` 23.1.0, not the Avast LLVM-8 fork
this document used to say -- and the environment this audit ran in could not
build it.
Changing `llvmir2hll` or `src/retdec/retdec.cpp` there would mean shipping an
edit to the C emitter that had never been compiled, let alone run against the
216-binary corpus. Everything below is therefore *read and verified against the
source*, but not tested.

Each entry names the file and line, states what is wrong, and says what the fix
is. Verify before acting: this tree moves, and a finding may already be fixed.

**Later sections carry their own reasons.** The paragraph above explains the
original audit's findings, which were behind a build that environment could not
run. Everything under "Found while fixing the reproducibility defects" and after
was found with CI available and is unfixed for a reason stated in the entry --
an output change too large to make without a reproducer, a platform this
environment cannot compile, a failure whose log does not yet say enough.

---

## 1. The 0/216 recompile failure has one cause, and it is three lines

**Closed by 86970d0, by the narrow route.** The pass is no longer run:
`optimizer_manager.cpp` now carries a comment in place of the `run<>` call
naming the three facts in this tree that break the safety argument the old
comment made. The pass itself is untouched and still deletes every
initialiser-less `VarDefStmt` unconditionally, so it must not be re-enabled
without the use check described below; it is correct only for a back end that
emits no declarations, and this tree has only `c_hll_writer`.

**The 0/216 number is now unmeasured, not fixed.** Nothing had ever handed the
emitted C to a compiler -- DET-01 compares two runs, CACHE-05 compares cache on
against cache off, the recovery gate reads detections, and all three treat the
output as text. `scripts/ci/check_emitted_c_compiles.sh` (CC-01) is that
missing measurement and runs in ctest-linux; until its first number is in, the
README's 0/216 is a figure that predates this branch and nobody has re-taken.
Sections 2 and 3 below are the other whole-file failures and are still open, so
a jump to 216/216 was never expected from this alone.

The original entry follows.

---

`src/llvmir2hll/optimizer/optimizers/no_init_var_def_optimizer.cpp:28`

This was the finding worth acting on first. `README.md` reports default `.c`
recompiling **0/216** while the `--buildable` sidecar reports 216/216, and the
whole sidecar exists to work around it.

```cpp
void NoInitVarDefOptimizer::visit(ShPtr<VarDefStmt> stmt) {
    if (stmt->hasInitializer()) {
        visitStmt(stmt->getSuccessor());
        return;
    }
    ShPtr<Statement> stmtSucc(stmt->getSuccessor());
    Statement::removeStatement(stmt);   // <- unconditional
    visitStmt(stmtSucc);
}
```

Every `VarDefStmt` without an initializer is deleted. There is no liveness check
and no use check. The call site in
`src/llvmir2hll/optimizer/optimizer_manager.cpp:263` described one that did not
exist (this comment has since been replaced by the one that removes the call):

```cpp
// NoInitVarDefOptimizer removes VarDefStmt nodes that have no initializer
// and whose variable is not used after the declaration (dead declarations
// left behind after VarDefStmtOptimizer moved the real defs to their uses).
run<NoInitVarDefOptimizer>(m);
```

The second clause of that comment is not implemented. So a local that is
assigned later but declared without an initializer loses its declaration, and
the emitted C reads `v7 = ...;` with no `int64_t v7;` anywhere — `error: 'v7'
undeclared`. That is a whole-translation-unit failure on essentially every
function, which is exactly what `tu_valid_rate: 0.000` in
`results/decompilebench-full.json` records.

The pass is correct for Python, which emits no declarations at all. It is only
wrong for C.

**Two ways to fix it.** The narrow one is to skip the pass for the C writer at
the call site, which is a one-line change and immediately testable against the
corpus. The better one is to implement the use check the comment already claims,
via `ValueAnalysis` / `VarUsesVisitor` — both are already available to the
manager — so dead declarations still get removed and live ones do not.

Either way the regression test is the same: a function whose local is assigned
but never initialised must still emit its declaration.

**What to expect.** This is not a guaranteed jump to 216/216. Sections 2 and 3
below are also whole-file compile failures, and there may be more behind them
that only become visible once this one stops masking everything. But it is the
first blocker, and until it is fixed no measurement of the default `.c` output
is measuring anything else.

---

## 2. Two more whole-file compile failures in the same backend

Both produce output that no C compiler accepts, so each one alone is enough to
keep a binary at `tu_valid = 0`.

**`goto_cfg_optimizer.cpp:361` — gotos with no label.** Pattern A builds a
replacement `IfStmt`, retargets every inbound goto to it, moves the label onto
it, and then discards it. The result is `goto lab_XXXX;` with no `lab_XXXX:`
anywhere: *use of undeclared label*, from gcc and clang alike. Triggered
whenever a goto target is itself an `if (c) goto L;`.

There is a second, independent source of the same symptom at line 354: the pass
clones the moved statements, and `Statement::clone()` does not copy labels.

**`c_hll_writer.cpp:1194` — a label at the end of a block.** `CHLLWriter` emits
nothing at all for `UnreachableStmt`. A labelled unreachable terminator
therefore emits a label with no statement after it: *label at end of compound
statement*, an error in every C standard before C23. Any block reached only by a
goto and ending in `unreachable` — the standard lowering for a noreturn call —
hits this. Emitting `;` or an explicit trap would do.

**Both of the above are fixed** — `a4a1e92` and the commits around it — along
with a third route to the same `goto` symptom: `HLLWriter::getRawGotoLabel()`
invents `generated_N` for a statement with neither a label nor LLVM
basic-block metadata and is called once for the goto and once for the label, so
without memoisation the two got different names. `tests/llvmir2hll` now runs
(`scripts/ci/check_llvmir2hll_tests.sh`), and all three have regression tests
there.

### Still open: the emitted C contradicts the header it asks for

CC-01 (`scripts/ci/check_emitted_c_compiles.sh`) measures 21/24 on its slice.
One of the three failures is the goto symptom above by a route the fixes do not
cover; the other two are this:

```
ring_buffer-gcc-O3.c:78:12: error: too many arguments to function 'putc'
    | return putc(c, stream, a3);
    ? 7:#include <stdio.h>
```

`HeadersForDeclaredFuncs::getHeaders()` adds `<stdio.h>` because the semantics
recognise `putc` as a library function, and `emitFunctionPrototypesForNonLibraryFuncs()`
therefore emits no prototype of retdec's own — the header is taken as the
truth. The call is then emitted against a signature the ABI recovery invented,
with an extra argument. The file asserts two incompatible things about the same
function.

This is not fixed here, and the reason is worth recording rather than guessing
at a patch:

- The extra argument comes from parameter recovery in `bin2llvmir`, which needs
  the pinned LLVM and cannot be built or tested in this environment. Every fix
  landed on this branch was reproduced before it was written; a change here
  could only be validated by a thirty-minute CI round trip, which is not the
  same thing.
- The obvious in-backend repairs are each wrong in a different way. Truncating
  the call to the arity `semantics::getNameOfParam` knows would break `printf`,
  which is variadic and has exactly one named parameter. Emitting retdec's own
  prototype instead of taking the header replaces *too many arguments* with
  *conflicting types*. Suppressing the header for that one function does not
  help when another function pulls the same header in.
- What would settle it is an arity (and variadic flag) in the semantics layer
  next to `getCHeaderFileForFunc`, which is where this knowledge already lives
  for headers and parameter names. That is a table, and a table is only worth
  adding if it is right.

Until then CC-01's floor holds the line at 0.8750: the rate cannot fall without
turning CI red, and each fix that raises it should raise the floor with it.

---

## 3. Silent miscompilation, which is worse than a compile failure

These produce C that compiles cleanly and does the wrong thing. Nothing in the
pipeline catches them, because `tu_valid` and `recompile` both pass.

**`if_to_switch_optimizer.cpp:429` — range comparisons become single cases.**
An unbounded `v >= k` / `v > k` else-if chain is converted into a `switch` whose
cases are single values. `v = 7` takes branch A in the binary and branch C in
the emitted source. This is the most common dispatch shape there is.

**`if_to_switch_optimizer.cpp:310` — signedness ignored.** The `Lt`/`Le`
upper-bound patterns do not distinguish the signed from the unsigned compare, so
`if (v < 1) A; else if (v < 2) B; else C;` on a signed control expression becomes
`switch (v) { case 0: A; case 1: B; default: C; }`. Every negative input takes
`C` instead of `A`.

**`copy_propagation_optimizer_ext.cpp:114` — propagation across an if.**
`canPropagateThroughStmts` invokes `ValueAnalysis` with `visitNestedStmts=false`,
so an intervening `if`/`while`/`for` is treated as opaque rather than as
unknown. `t = f(); if (c) { t = 0; } g(t);` becomes
`t = f(); if (c) { t = 0; } g(f());` — the call is duplicated, moved past a
reassignment, and past whatever the body did.

**`while_true_to_for_loop_optimizer_ext.cpp:105` — loop direction by name.**
Whether a loop counts up or down is decided by a substring match on the
variable's *name*. A descending loop whose induction variable is not named
suggestively is emitted with an ascending `i < N` exit test, so the emitted loop
terminates differently from the binary.

---

## 4. The public API cannot report failure

`src/retdec/retdec.cpp:1083` — `decompile()` is declared `bool` and returns
`EXIT_SUCCESS`, which is `0`, which is `false`. There is no failure return path
at all. So the idiomatic `if (!retdec::decompile(cfg)) { /* error */ }` reports
an error on every successful run, and `if (retdec::decompile(cfg))` never fires.

`src/retdec/retdec.cpp:1346` — `parallelBatchDecompile()` consequently reports
every job as failed. A harness batching 216 binaries and counting successes gets
0/216 whatever happens.

`src/llvmir2hll/llvmir2hll.cpp:514` — failing to open the output file exits 0
having written nothing. `retdec-decompiler -o /nonexistent/dir/out.c foo.elf`
succeeds, silently.

`src/retdec-decompiler/retdec-decompiler.cpp:1028` — there is no unknown-option
diagnostic anywhere in the CLI parser. A malformed command line prints help to
stdout and exits 0, and an unrecognised option is taken as the input filename.

These four together mean no script, Makefile or CI job can currently tell
whether a decompilation worked. They are small, independent fixes; they need
the build only because `src/retdec/` and `src/llvmir2hll/` link LLVM.

---

## 5. The SSA the detectors run on is not the SSA they were written against

`src/retdec/llvm_to_ssa.cpp`

This is the widest finding in the file, and it is one defect, not a family of
them. `src/retdec/retdec.cpp:684` builds the module every structural detector
consumes:

```cpp
ssaMod = buildSsaModule(*module);
```

`buildSsaModule` calls `translateInstr` per instruction, and `translateInstr`
populates `IrInstr::uses` in exactly one place:

```cpp
if (instr
    && (llvm::isa<llvm::BinaryOperator>(li) || llvm::isa<llvm::PHINode>(li)
        || llvm::isa<llvm::AtomicRMWInst>(li) || llvm::isa<llvm::AtomicCmpXchgInst>(li)))
{
    for (unsigned i = 0, n = li.getNumOperands(); i < n; ++i)
    {
        const auto* c = llvm::dyn_cast<llvm::ConstantInt>(li.getOperand(i));
        if (!c || c->getBitWidth() > 64) continue;      // <- everything else dropped
        ...
        instr->uses.push_back(u);
    }
}
```

Three things follow, and each was checked by reading the whole file:

1. **`defValue` is never assigned.** The string `defValue` does not appear in
   `llvm_to_ssa.cpp` at all (a grep for it matches only `UndefValue`). Every
   instruction in a production module therefore defines nothing.
2. **Only immediate operands ever become uses**, and only on four instruction
   classes. A register operand — the entire point of a def-use graph — is
   skipped by the `continue`.
3. **`Load` and `Store` are not in that list**, so they arrive with empty use
   lists. Not "partially populated": empty.

No `SSAPass` runs on the result either; `retdec.cpp` never calls one. And it
could not help if it did, because renaming needs value operands to rename and
there are none.

**What this costs.** Any predicate that reads `instr->defValue`, follows a
non-immediate use, or relates two instructions by value is dead on the
production path — it cannot return true for any binary, however well the code
matches. Confirmed dead by this route, at least:

| File | Predicate | Needs |
|---|---|---|
| `algo_recover/binary_search_detect.cpp:379` | the whole detector | `defValue` |
| `algo_recover/find_detect.cpp:88` | `hasImmediateComparand` | `defValue` |
| `container_detect/list_detect.cpp` | `hasSentinelInit` | `Store::uses[0..1]` |
| `container_detect/map_detect.cpp` | `hasRotation` | `Load::defValue`, `Store::uses[0..1]` |

Their unit tests pass because the fixtures hand-build IR with `defValue` set and
real operand lists — which is what the detectors were designed against, and what
the adapter does not deliver.

Two consequences worth stating plainly, because they change how other findings
should be read:

- The *precision* fixes made to these detectors on this branch are real and
  worth keeping, but their corpus impact is nil until the adapter is fixed:
  a predicate that cannot fire cannot fire wrongly either.
- Conversely, any claim that a detector's false positives explain part of the
  0.056 name-blind F1 is unearned for the detectors above. They contribute
  nothing on real input, in either direction. That is its own kind of bad.

**The fix.** Give `translateInstr` a `DenseMap<const llvm::Value*, ssa::ValueId>`
memo, allocate an `IrValue` per LLVM `Value` on first sight, set
`instr->defValue` for every instruction that produces one, and push a `Use` for
every operand rather than only `ConstantInt` ones — with `Load` and `Store` in
the set, and the `Store` operand order (value, address) that the detectors
already document. It is a self-contained change to one file, but it needs LLVM
to compile and the 216-binary corpus to show it helped, so it is not attempted
here.

Worth doing before any further detector tuning: the tuning is currently being
measured against fixtures rather than against anything the pipeline produces.

---

## 6. Other confirmed items

~~`retdec-decompiler` aborts on `generated_quicksort-gcc-O0`~~ — diagnosed and
fixed, and the guess recorded here was wrong.

This entry read: "The four candidates are in
`src/bin2llvmir/analyses/reaching_definitions.cpp` (lines 248, 277, 343 and
347), all of the shape 'we should have this basic block in bbMap'." None of
them. The assertion is in LLVM, and the gate had never printed it: the
prediction extractor kept the last three lines of the crash, and an LLVM
assertion prints its message FIRST. With `summarise_failure_output` keeping
both ends, `ctest-linux` run 34271470725 said:

```
llvm/lib/IR/Value.cpp:523: void llvm::Value::doRAUW(llvm::Value*, ReplaceMetadataUses):
Assertion `New->getType() == getType() &&
          "replaceAllUses of value with new value of different type!"' failed.
1.  Running pass 'LLVM instruction optimization using RDA' on module ''
```

`ReachingDefinitionsAnalysis` pairs a definition with a use by their POINTER
operand — `initializeBasicBlocks` records `Definition(s, s->getPointerOperand())`
and `Use(l, l->getPointerOperand())` — and says nothing about the value types at
either end. Under opaque pointers a store of an `i32` and a load of an `i64`
through the same register global are one definition and one use of it, and
`inst_opt_rda.cpp` called `replaceAllUsesWith` across them. The three patterns
in `inst_opt_rda_ext.cpp` each check the types already; the two in
`inst_opt_rda.cpp` predate opaque pointers, where a pointer's element type tied
the two together, and did not. Both check now.

Not reproducible in this environment either way — it needs the pinned LLVM
build — so the fix rides on the next `ctest-linux` run rather than on a local
test.

Eleven of the thirteen test sources in `tests/fileformat/` and one in
`tests/common/` are named by no `CMakeLists.txt`, so `ctest` does not build or
run them: `ar_archive_format_probe_tests.cpp`, `coff_format_tests.cpp`,
`elf_format_tests.cpp`, `format_detection_tests.cpp`,
`format_factory_tests.cpp`, `intel_hex_format_20bit_tests.cpp`,
`intel_hex_format_tests.cpp`, `intel_hex_token_test.cpp`,
`macho_format_tests.cpp`, `pe_format_tests.cpp`, `raw_data_format_tests.cpp`
and `tests/common/calling_convention_tests.cpp`. `tests/fileformat` builds two
files. Whether they still compile against the LLVM 23.1.0 migration is not
known here, because everything they link needs that build; re-enabling them
blind would break `ctest-linux` for hours per attempt, so they are listed
rather than switched on. `scripts/check_cmake_sources.sh` now fails on any
*new* test source that nothing builds, and carries these twelve in an explicit
`UNBUILT` list with that reason attached, so the set cannot grow quietly.

`src/bin2llvmir/providers/` — the ten provider maps are process-wide, and
`ProviderInitialization::runOnModule` starts by calling `clear()` on all of
them. `clear()` drops everyone's entries, not the caller's, so a second pipeline
destroys the `Config` and `FileImage` the first is still holding pointers into.
`parallelBatchDecompile()` ran N pipelines on a thread pool, which made that a
use-after-free on the fast path of an API that advertises parallelism.

Not fixed here, because the fix is ten headers and every accessor in them --
key and clear per `llvm::Module` rather than per process -- and nothing in this
environment can compile bin2llvmir to check it. What *is* done is that
`decompile()`, `decompileToLlvmIr()`, `tryEmulationUnpacking()` and
`disassemble()` now take a process-wide recursive lock, so the pipelines take
turns instead of corrupting each other. `parallelBatchDecompile` is therefore
correct and serial; `retdec.h` and `docs/PERFORMANCE.md` say so. Lifting the
lock is what the provider refactor is for.

The two mutable statics in `ModulePassPrinter` that the same batch path raced --
`LastPhase`, a `std::string` every pass assigned to, and
`passWallStartForTimedPass` -- are `thread_local` now. They are per-pipeline
state and there is one pipeline per thread.

`src/neural/llama_inference.cpp:242` — `llama_tokenize` is called with
`parse_special=false`, so the ChatML template `prompts.cpp` builds is fed to the
model as literal text and never applied. Every non-mock refinement runs the
model outside its instruction-tuned distribution.

~~`src/retdec/semantic_recovery_export.cpp:95` — semantic comments are placed by
DWARF/PDB *source* line number~~ — fixed. `definitionLineOf` finds the function
in the emitted output by name, using the same declarator tests the sidecar
generator uses, so a stripped binary gets its comments and a binary with debug
info gets them in the right place. Four tests, six of which fail against the
old placement.

~~`src/retdec/semantic_recovery_export.cpp:1583` — comment injection and sidecar
generation both run unguarded on the output file~~ — fixed. `-f json` and
`-f json-human` route llvmir2hll through a `JsonOutputManager`, so both now
refuse: `outputIsSourceListing` gates the injector, and
`maybeWriteBuildableSidecars` gained a config overload that does the same. The
same read-and-rewrite also dropped the file's trailing newline whether or not a
comment was added; it now leaves an untouched output byte for byte.

`src/retdec-decompiler/retdec-decompiler.cpp:891` — managed inputs
(`.apk`/`.jar`/`.pyc`/`.class`/`.wasm`) are written to a `.c` filename and
bypass `--silent`, `-f json` and log redirection entirely.

---

## Working on these

The three fast checks on this branch do not cover any of this code — they are
LLVM-free by construction, and all of it is behind LLVM. Use the real build:

```bash
cmake --preset core-release
cmake --build build/linux -j"$(nproc)"
ctest --test-dir build/linux --output-on-failure
```

For section 1, the measurement that matters is the corpus recompile rate:
`results/decompilebench-full.json`, regenerated per `docs/BENCHMARKS.md`. If
`tu_valid_rate` moves off 0.000, the fix landed.

---

# Residual findings from the SMT/adversarial-verification pass

A separate exercise from the audit above: thirteen arithmetic kernels were
proved with ESBMC, the call sites were routed through them, and every round of
fixes was then handed to an adversarial verifier whose brief was to disbelieve
it. Four rounds ran.

**Adversarial verification does not terminate.** Each round found fewer defects
in the fixes and more sites of the same class elsewhere; a fifth round would
find a sixth. What follows is the state at the point where that was called,
written down so the call is visible.

Everything in the previous version of this section that was reachable in this
environment has since been fixed — the second unbounded `descriptorToType`, the
LocalVariableTable extent in two more modules, three DEX conversions,
`set10Byte`'s un-inverted path, `nibblesFor`'s capacity, the divergent match
loop, and the stale citations. What is left is here.

## Behind a build this environment cannot run

Everything below is compiled only by the LLVM-dependent build, so a change to it
here would ship untested — the same reason the audit findings above were written
down rather than attempted. All were read and measured against the source; none
were compiled.

* `src/fileformat/file_format/elf/elf_format.cpp:2770-2773` and
  `coff_format.cpp:600-609` — `getDeclaredFileLength` overrides that shadow the
  base function which *was* fixed, each forming unclamped 64-bit sums (and, in
  the COFF case, an unchecked multiply) of header fields.
* `src/fileformat/file_format/file_format.cpp:2309` — `getXByte` still forms
  `secSeg->getOffset() + secOffset`.
* `file_format.cpp:583-593` (`computeSectionTableHashes`) and `:1380`
  (`getOverlayEntropy`) — unchecked multiply and sums on header fields.
* `src/fileformat/utils/other.cpp:407` — `getRealSizeInRegion`'s wrapping clamp.
  Re-measured: `(10, 2^64-5, 512)` returns `18446744073709551611`. Reached from
  `sec_seg.cpp:235/402` and `resource.cpp:93/218`.
* `src/fileformat/types/resource_table/bitmap_image.cpp:210, 302, 383, 437, 490`
  — unchecked `nBytesInRow * nRows` from header width/height/bitCount, feeding
  the clamp above.
* `src/loader/loader/segment.cpp:205`, `segment_data_source.cpp:81-82, 91` —
  post-hoc and pre-copy wrapping clamps. `Image::getXBytes` now refuses before
  them, but both have other callers.
* `src/cli_parser/cli_sig.cpp` — a self-referential TypeSpec whose signature is
  a GENERICINST with two or more self-referencing type arguments (e.g.
  `15 12 06 02 12 06 12 06`) drives an exponentially wide traversal under the
  depth-64 bound. The bound stops the depth, not the width. Reachable, but the
  fix is a visited-set through `ITypeNameResolver`, whose other implementation
  is behind the LLVM build.

## Structural: code no test can reach

**Mostly closed.** `src/utils/gpu_scanner.cu`'s CUDA half -- roughly 660 lines,
all of the device code and all of the runtime plumbing -- was compiled by
nothing in this tree, because `src/utils/CMakeLists.txt` selects it only when
CUDA is found and `gpu_scanner_cpu.cpp` includes it with
`RETDEC_GPU_SCANNER_HOST_ONLY` defined. An adversarial verify pass proved the
consequence by reverting four fixes inside that region and watching the suite
stay green at 13/13.

`tests/utils/cuda_stub/` is now enough of the CUDA runtime to compile it with an
ordinary C++17 compiler, and `scripts/standalone_check.sh` does so on every run.
It caught a real defect the first time it ran. What the stub provides is
documented at the top of `cuda_runtime.h`, including what it does *not* model:
no warp semantics, no coalescing or timing, and blocks serialised rather than
concurrent -- so a kernel with an inter-block race would pass there and fail on
a device.

What is still open is one step further:

* The CUDA half is **compiled but not executed** by the suite. The stub can run
  a kernel -- `__syncthreads()` is a real barrier across `std::thread` lanes, so
  a kernel that fills a `__shared__` array before the barrier and reads its
  neighbours' entries after it behaves correctly -- but nothing links it,
  because `GpuScanner`'s methods are already defined by `gpu_scanner_cpu.o` and
  the two cannot go in one binary. Closing it means either a second test binary
  that links the `.cu` instead of the `.cpp`, or splitting `GpuScanner` so the
  device path is a separate type.

### `src/opencl/` and its eight test suites are built by nothing

`src/opencl/` is 4,529 lines of OpenCL host code — context, disk cache,
profiler, buffer pool, parallel disassembler, type inferencer, Steensgaard
analysis, semantic hasher, e-graph simplifier — with a complete
`CMakeLists.txt` that no `add_subdirectory` anywhere in the tree names.
`tests/opencl/` has eight suites and a complete `CMakeLists.txt` that
`tests/CMakeLists.txt` likewise never adds. Neither the library nor its tests
is compiled by any build of this repository, and
`cmake/superbuild/stubs/retdec-ocl/` describes itself as the stub for a "real
implementation in `src/opencl/`" that nothing links.

Found by `scripts/check_cmake_sources.sh` once it was taught that a directory
carrying a working `CMakeLists.txt` that nothing includes is the same failure
as a source no `CMakeLists.txt` names.

Not wired in here, and the reason is specific rather than general:
`src/opencl/CMakeLists.txt` opens with `find_package(OpenCL REQUIRED)`, so
adding it unconditionally fails configure on every machine without an OpenCL
SDK — it needs an option and a guard, and the guard has to be exercised in both
states. No configure of this tree can run in this environment (it fetches and
builds LLVM), so wiring a 4,529-line library into the build here would be an
unverified change to the build of every consumer. `tests/opencl` carries that
reason in `UNBUILT_DIRS` in `scripts/check_cmake_sources.sh`, so the check
passes for a stated reason rather than because it cannot see the directory.

Closing it means: an `RETDEC_ENABLE_OPENCL` option defaulting OFF,
`add_subdirectory(opencl)` in `src/CMakeLists.txt` and `tests/CMakeLists.txt`
behind it, and a CI job that configures with it ON against a POCL or Mesa
Rusticl ICD — then the `UNBUILT_DIRS` entry comes out.

## Corrections to claims made in this branch's commit messages

### The Dalvik try-region wrap was not reaching a consumer

Commit `12ab168` fixed the uint32 wrap in `DexLifter::wireExceptions`
(`t.startAddr + t.insnCount`) and said the bad numbers "reach every consumer
that asks how long the region is", quoting the comment
`src/jvm_parser/jvm_lifter.cpp` carries about its own equivalent.

That sentence was borrowed rather than checked, and it is wrong for this tree.
Every reader of `BcExceptionHandler::startOffset`/`endOffset` is one of four:

* `src/jvm_reconstruct/stack_sim.cpp` and `src/jvm_reconstruct/local_rebuild.cpp`
  read only `handlerBlock` and `catchType`, never the offsets;
* `src/cil_reconstruct/cil_reconstructor.cpp` uses the pair as a grouping key,
  where a wrapped pair groups as consistently as a sane one — and that is the
  CIL path, not the DEX one;
* `src/java_emitter/java_stmt_emitter.cpp` is the only place a DEX-lifted
  region is read as a range, and it opens with
  `if (eh.endOffset <= eh.startOffset) continue;`.

That guard is total for this input, not lucky: `TryItem::insnCount` is a
`uint16_t`, so the sum wraps only for `startAddr >= 0xFFFF0001`, and then
`end < startAddr` always. Every wrapped region is inverted, and inverted
regions are exactly what line 125 already refuses. Nothing in the tree computes
`endOffset - startOffset` at all.

So the fix is defence in depth and consistency with `protectedRegionFits` on
the JVM side — refusing malformed input where it is read rather than relying on
a downstream guard — and not the live defect the commit message described. The
`catchAllAddrs` out-of-bounds index fixed in the same commit is unaffected by
this: that one segfaults, and the test for it exits 139 without the fix.

---

## Found while fixing the reproducibility defects, and deliberately left alone

### The def-use chain comparator is not a strict weak ordering either

`src/llvmir2hll/optimizer/optimizers/copy_propagation_optimizer.cpp`,
`ordered(const DefUseChains::DefUseChain &)`

`GlobalVarsSorter`'s comparator was rewritten because it was not an ordering
(see `REPRODUCIBILITY.md`). The def-use chain comparator in the same family
has the same defect, and is not being changed.

Its first five steps compare a tuple of totally ordered keys -- number of
uses, variable name, statement text, the set of use texts -- and that part is
a valid ordering. The tail is not. It walks both statements' predecessor
chains in step, comparing `(number of predecessors, first predecessor's text)`
at each level, and stops as soon as *either* chain reaches a statement with no
predecessors:

```cpp
} else {
    break;                 // one chain ran out; the two are "equal"
}
```

That makes a chain equal to any chain it is a prefix of, and prefix-equality
is not a transitive equivalence. Take chains `x`, `xy` and `xz`: `x` compares
equal to `xy` and equal to `xz`, but `xy` and `xz` differ at their second
element, so they do not compare equal to each other. `std::sort` has no
defined behaviour on a comparator like that -- it may read past the end of the
range.

Reaching it needs two distinct `(statement, variable)` entries that tie
through all five earlier steps: same number of uses, same variable name, same
statement text, same use-set texts, same parent and successor texts. The same
assignment on two arms of an `if`, surrounded identically, would do it.

**Why it is not fixed here.** The obvious repair is to end the comparison on
the statement's position in the function's CFG, the way `ordered(StmtSet)`
now does -- `stmtOrder` is already a member and already built for every
`performOptimization()`. But the chain walk currently *decides* comparisons,
not just ties, so replacing it changes the order def-use chains are processed
in, which changes which copy propagations happen first, which changes the
emitted C for binaries that are not misbehaving today. That is a real risk to
take without a binary that reproduces the defect, and there is no such binary
yet: `ordered(du)`'s input is a `std::vector` built by walking the CFG's node
vector in order, so the sort's *result* is deterministic on a deterministic
input even with a bad comparator. The exposure is the undefined behaviour, not
a wrong answer.

**What would settle it.** Run the corpus under a build that checks comparator
validity -- libstdc++'s `_GLIBCXX_DEBUG` diagnoses "comparison doesn't meet
irreflexive requirements" -- and see whether any binary trips it. If one does,
it is worth the output churn; DET-01 will show exactly how much churn.

### The sanitizers workflow never passed, and the reason was in its own options

`.github/workflows/sanitizers.yml`

**Answered and fixed; kept here because the answer is worth having written
down.** Runs 42 through 45 all failed and run 44's commit message says why:
"The sanitizers workflow has never got past the build." Run 45 was the first
that did, and it died in the ASan run itself with one line and no address in
it:

```
ERROR: Failed to mmap
```

The hypothesis recorded here first was that the deliberately `-no-pie` image
loads at `0x400000` and grows up into ASan's low shadow at `0x7fff8000`. That
was wrong. `ASAN_OPTIONS=verbosity=1`, added to make the next run say
something, said it:

```
|| [0x00008fff7000, 0x02008fff6fff] || ShadowGap ||
protect_shadow_gap=0: not protecting shadow gap, allocating gap's shadow
|| [0x000091ff6000, 0x004091ff6fff] || ShadowGap's shadow ||
...
ERROR: out of memory: failed to allocate 0x1000 (4096) bytes of
       InternalMmapVector (error code: 12)
```

`protect_shadow_gap=0` was in the workflow's `ASAN_OPTIONS`. It tells ASan
not to protect the shadow gap but to allocate shadow *for* it -- and the gap
spans 128 TB, so its shadow is about 63 TiB. The mapping goes through under
`vm.overcommit_memory=1`, and then the process cannot get another four
kilobytes: ENOMEM on a 4096-byte `InternalMmapVector`. The option exists for
running under an emulator that cannot protect the gap. Nothing here needs it.

Two things to take from it. The kernel knobs the workflow spends a step on --
`randomize_va_space=0`, `mmap_rnd_bits=28`, `overcommit_memory=1` -- were all
applied and all irrelevant; the answer was an option the workflow set itself.
And a gate that fails with one line and no address in it cannot be diagnosed
at all: the one-line `verbosity=1` is what turned three failed runs of
guessing into a five-minute read.

### The Windows build cannot compile two parsers, because nothing gives it zlib

`src/jvm_parser/CMakeLists.txt:19`, `src/dex_parser/CMakeLists.txt:15`

`ctest-windows` fails on `main`, eleven minutes in:

```
FAILED: src/jvm_parser/CMakeFiles/retdec-jvm-parser.dir/jvm_jar_reader.cpp.obj
src\jvm_parser\jvm_jar_reader.cpp(23): fatal error C1083:
    Cannot open include file: 'zlib.h': No such file or directory
```

Both parsers read ZIP containers -- a JAR and an APK are ZIP files -- so both
`#include <zlib.h>`, and both declare the dependency as the bare link name
`z`:

```cmake
target_link_libraries(retdec-jvm-parser PUBLIC retdec-bc-module z)
```

A bare `z` carries no include directory. On Linux it works by accident:
`zlib.h` is in the default include path and `libz.so` is in the default
library path, so neither half of the dependency has to be declared correctly.
On Windows neither is true, and nothing else in the tree supplies zlib for a
native MSVC build. `deps/zlib/` exists but returns immediately unless
`RETDEC_BUNDLED_ZLIB_WIN_CROSS`, which `cmake/options.cmake` defaults ON only
for `CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows"` -- the
MinGW cross path, not this one.

**The shape of the fix.** Build the bundled zlib for native Windows as well,
which cannot affect Linux or the cross path: `deps/CMakeLists.txt` adds the
subdirectory only under the cross condition, and `deps/llvm/CMakeLists.txt`
consumes `RETDEC_CROSS_ZLIB_*` only under the same one, so a native-Windows
branch is reachable from neither. Two details will cost an iteration each if
missed: zlib's CMake installs the static library as `libzlibstatic.a` under
MinGW but `zlibstatic.lib` under MSVC, and `zlibstaticd.lib` for a Debug
build, which `full-windows-debug` is. The two targets then need the include
directory and an `add_dependencies` on `retdec-zlib`, and the bare `z` should
stay only where a system zlib is what is meant.

**Not fixed here** because it is a build-system change for a platform this
environment cannot compile or test, and there may be more failures behind it
-- ninja stops at the first, and this is the first. `ctest-windows` is
`workflow_dispatch`-able and fails in about eleven minutes, so iterating on it
is cheap for someone who can watch it.

### The decompiler dumps core on C11 atomics

**Closed.** The symbolized stack named it exactly:

```
src/llvmir2hll/llvm/llvmir2bir_converter/llvm_instruction_converter.cpp:521:
visitInstruction: Fail (unsupported instruction:
  %var2 = atomicrmw add ptr inttoptr (i64 16412 to ptr), i32 1 seq_cst)
 #8  llvm_instruction_converter.cpp:521
 #9  InstVisitor<LLVMInstructionConverter, ...>::visitAtomicRMWInst
 #14 BasicBlockConverter::visitInstruction   basic_block_converter.cpp:287
```

`BasicBlockConverter` had no case for `atomicrmw`, `cmpxchg` or `fence`, so
all three fell through to the expression converter's `visitInstruction`,
which calls `FAIL` -- and `FAIL` aborts in release as well as debug. Eight
lines of C11 took the whole decompilation with them, and the guard directly
above the call site, which turns a null expression into an empty statement
rather than crashing, could never run because the abort came first.

All three have cases now (`basic_block_converter.cpp`). `atomicrmw` becomes
the read and the write it performs, `cmpxchg` becomes the read, the
comparison and a conditional store written as a ternary rather than as
control flow, and `fence` becomes nothing. The min/max and floating-point
`atomicrmw` forms are not lowered and take the empty-statement path, which is
what every other unhandled instruction in that converter does.

**What it cost to find it, and what that changed.** The first two DET-01 runs
printed thirty-six frames that named nothing: `tail -n 25` of a crash log is
the *bottom* of an LLVM backtrace -- `main`, `__libc_start_main` -- because
LLVM prints innermost frame first, and every frame read
`retdec-decompiler 0x00005643ba734e06` because the handler only runs
`llvm-symbolizer` when it can find one and otherwise falls back to `dladdr`,
which resolves exported symbols only. The excerpt is anchored on the report
banner now and prints the eighty lines above it, `llvm` is in the workflow's
apt list, and the third run named the instruction on the first line it
printed.
