# Confirmed audit findings not fixed here

A 14-dimension subsystem audit of this fork produced 217 findings, 129 of them
high or critical. The ones that could be built and tested without LLVM were
fixed on the branch that produced this file; these could not, so they are
written down rather than attempted.

**Why they are not fixed.** A full build needs the LLVM 8-era pin from
`cmake/deps.cmake`, and the environment this audit ran in could not build it.
Changing `llvmir2hll` or `src/retdec/retdec.cpp` there would mean shipping an
edit to the C emitter that had never been compiled, let alone run against the
216-binary corpus. Everything below is therefore *read and verified against the
source*, but not tested.

Each entry names the file and line, states what is wrong, and says what the fix
is. Verify before acting: this tree moves, and a finding may already be fixed.

---

## 1. The 0/216 recompile failure has one cause, and it is three lines

`src/llvmir2hll/optimizer/optimizers/no_init_var_def_optimizer.cpp:28`

This is the finding worth acting on first. `README.md` reports default `.c`
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
`src/llvmir2hll/optimizer/optimizer_manager.cpp:263` describes one that does not
exist:

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

`src/fileformat/utils/format_detection.cpp:373` — uint32 overflow in the PE
lattice-hint bounds check, giving a ~4 GB heap over-read. Reached from
`detectFileFormat` on every non-raw input, and once per archive member.

`src/retdec/retdec.cpp:212` — two mutable process-wide statics are mutated
concurrently by `parallelBatchDecompile`: a `std::string` and a
`std::vector<std::string>` that N threads `push_back` and `clear` at once. Heap
corruption, not a garbled log line.

`src/neural/llama_inference.cpp:242` — `llama_tokenize` is called with
`parse_special=false`, so the ChatML template `prompts.cpp` builds is fed to the
model as literal text and never applied. Every non-mock refinement runs the
model outside its instruction-tuned distribution.

`src/retdec/semantic_recovery_export.cpp:95` — semantic comments are placed by
DWARF/PDB *source* line number, so on a stripped binary (the normal input) they
never appear, and where debug info exists they land at arbitrary offsets in the
emitted C.

`src/retdec/semantic_recovery_export.cpp:1583` — comment injection and sidecar
generation both run unguarded on the output file, including when `-f json` was
requested. The machine-readable mode emits invalid JSON whenever the input has
debug info and any detection fires.

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
it. Three rounds ran. What follows is what the third round's verifiers still
list as live, after everything reachable in this environment was fixed.

**These are not the same kind of finding as the ones above.** Those are behind
an LLVM build this environment cannot produce. These are mostly reachable — some
were measured by execution — and were left because they are in a sibling file
outside the agent's assigned set, because they need a build this container
cannot run, or because the work converged and stopping was a judgement call
rather than a completion.

**Adversarial verification does not terminate.** Each round found fewer defects
in the fixes and more sites of the same class elsewhere; a fourth round would
find a fifth. What follows is the state at the point where that was called,
written down so the call is visible.

## Reachable and worth fixing

* `src/jvm_parser/jvm_class_parser.cpp:348-349` — the same unchecked
  LocalVariableTable extent fixed in `jvm_lifter.cpp`, in a sibling file the
  same `CMakeLists.txt` compiles into the same library. One-line fix, same
  shape.
* `src/jvm_reconstruct/local_rebuild.cpp:26-42` — an exact second copy of
  `descriptorToType`, still recursing once per `[` with no depth bound. Measured:
  20,000 brackets returns, 40,000 gives SIGSEGV at `-O1`. Fed from a `.class`
  file's descriptor. The `dex_parser` copy is fixed; this one is not.
* `src/jvm_reconstruct/local_rebuild.cpp:264-265` — `entry->startPc + entry->length`
  copied unchecked, the same LVT extent again in a third module.
* `src/dex_parser/dex_header.cpp:566` — `static_cast<int32_t>(r.uleb128())` in
  `parseCodeItem`; out-of-range unsigned-to-signed conversion of a value taken
  straight from the file, measured reachable with `0xFFFFFFFF`.
* `src/dex_parser/dex_header.cpp:74-77` and `:144` — the same conversion in
  `DexReader::s1/s2/s4/s8` and `uleb128p1`. No production caller today; the
  tests call them.
* `src/utils/byte_value_storage.cpp` — `set10Byte` memcpy's a `long double`'s
  object representation into eight bytes on the `!systemHasLongDouble()` path.
  The un-inverted twin of the `get10ByteImpl` path that was corrected.
* `src/utils/gpu_scanner.cu`, `gpuscan::nibblesFor` — the guard asks whether
  `size * 2` fits in `std::size_t`, not whether a `std::string` can hold it.
  Every size in `[2^61, 2^63)` passes and then throws `std::length_error` out of
  a `void` function. Refuse against `max_size()`, or return the failure.
* `src/cli_parser/cli_sig.cpp` — a self-referential TypeSpec whose signature is
  a GENERICINST with two or more self-referencing type arguments (e.g.
  `15 12 06 02 12 06 12 06`) drives an exponentially wide traversal under the
  depth-64 bound. The bound stops the depth, not the width.

## Behind a build this environment cannot run

Everything in `src/fileformat/` and `src/loader/` below is compiled only by the
LLVM-dependent build, so a change to it here would ship untested — the same
reason the audit findings above were written down rather than attempted.

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

## Structural: code no test can reach

* `src/utils/gpu_scanner.cu:234-895` is outside every suite in the tree.
  `src/utils/CMakeLists.txt` builds `retdec-gpu-scanner` from `gpu_scanner.cu`
  when CUDA is found and from `gpu_scanner_cpu.cpp` otherwise, and only the
  latter is ever compiled here. The shared arithmetic in the file's prologue is
  now covered — `gpu_scanner_cpu.cpp` includes it — but the six CUDA-side call
  sites that decide whether that arithmetic is called are not: a verifier
  reverted four fixes inside that region and the suite stayed green, 13/13.
  It is not unreachable in principle. The same verifier ran the real
  `__global__` kernel text under ASan with stub CUDA headers and caught a
  heap-buffer-overflow that way; the host half is easier still.
* `src/utils/gpu_scanner.cu` (`cpuMatchOne`) and `gpu_scanner_cpu.cpp` hold two
  copies of the match loop that **disagree**. Measured on `DE AD BE EF` with
  pattern `de/d`: `bestRatio 1.000000 / totalNibs 3` on the CPU build,
  `0.750000 / totalNibs 4` through the `.cu` copy.

## Documentation and hygiene

* Three stale line citations into `cli_reader.cpp` survive in
  `include/retdec/utils/byte_order.h:21` and
  `tests/verification/byte_order_proof.cpp:121, 202`. The routing that made them
  false is the fix in this branch. Cite functions, not lines.
* `src/utils/gpu_scanner.cu:626, 367` — `pos / 2` written as a bare literal in
  the file whose own comment says the factor is "stated here and nowhere else".
* `src/utils/gpu_scanner.cu:545-546, 719-720` — silent `static_cast<uint32_t>`
  narrowing of `size_t` file and window quantities into the kernel parameters.
