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

**The heading is wrong, and the number is measured now.** It took eight causes,
not one — this pass, the libc arity mismatch, four separate ways of losing a
goto's label, a `break` hoisted out of the loop it belonged to, a `void *` as
an operand of `|`, and `pthread_create` called with one argument where the
header declares four. The heading is left as written because the rest of the
section argues from it; the correction is here rather than in a rewrite.

**216/216.** CC-01 runs over the whole corpus on every `ctest-linux` run and
measured **216 of 216** at run 276, against the 0/216 this section was written
to explain. Both it and the 24-binary slice are floored at 1.0000. The trail is
runs 274 (208/216), 275 (214/216) and 276 (216/216), with F1 at 0.2302 and
DET-01 at 72 binaries 0 skipped throughout.

**Closed by 86970d0, by the narrow route.** The pass is no longer run:
`optimizer_manager.cpp` now carries a comment in place of the `run<>` call
naming the three facts in this tree that break the safety argument the old
comment made. The pass itself is untouched and still deletes every
initialiser-less `VarDefStmt` unconditionally, so it must not be re-enabled
without the use check described below; it is correct only for a back end that
emits no declarations, and this tree has only `c_hll_writer`.

**The 0/216 number was unmeasured, not fixed.** Nothing had ever handed the
emitted C to a compiler -- DET-01 compares two runs, CACHE-05 compares cache on
against cache off, the recovery gate reads detections, and all three treat the
output as text. `scripts/ci/check_emitted_c_compiles.sh` (CC-01) is that
missing measurement and runs in ctest-linux.

Its readings, in order: 21/24 on `d9e8108`, 23/24 on run 266 once the libc
arity mismatch was fixed, 24/24 on run 272 once the last of the goto-label
routes was. The floor is 1.0000 now, so a regression turns the run red rather
than being noticed a round later. Sections 2 and 3 below were the other
whole-file failures and are closed; a jump to 216/216 was never expected from
this section alone, and it did not come from it.

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

**And a fourth route, which is what run 266 was still reporting.** Pattern A
of the same pass takes the run of statements between the `if` and the goto's
target and makes it the new if-body. It *copied* that run: `buildChain()`
cloned each statement and called `redirectGotosTo()` on the original so the
label and the inbound gotos followed the clone.

That covers the run's own statements and nothing inside them.
`IfStmt::clone()`, `WhileLoopStmt::clone()` and `ForLoopStmt::clone()`
deep-copy their bodies through `Statement::cloneStatements()`, and no `clone()`
carries a label — so every statement nested inside a moved `if` or loop was
dropped along with its container while a goto elsewhere in the function went on
naming the label it had carried. `goto lab_0x112c;` with no `lab_0x112c:`.

The run is now *moved* rather than copied, which has nothing to fix up: every
statement keeps its identity, so its label, the gotos aimed at it and
everything nested inside it come along untouched. Two of the pass's helpers
(`buildChain`, `isForwardReachable`) were dead afterwards, the second of them
already dead before.

Moving exposed a smaller defect underneath. `Statement::transferLabelTo()` and
`transferLabelFrom()` assigned unconditionally, so transferring from a
statement with *no* label erased the label the destination had. The move
redirects the gotos aimed at the discarded `goto L` onto `L` itself, and `L` is
routinely labelled — the fix would have stranded a different goto than the one
it repaired. Both now return early with nothing to transfer. The two existing
tests named "does nothing when the statement has no label" could not see it:
they left the *other* statement unlabelled too, which cannot tell a no-op from
an assignment of the empty string.

### Closed by run 266: the emitted C contradicts the header it asks for

CC-01 (`scripts/ci/check_emitted_c_compiles.sh`) measured 21/24 on its slice
when this was written. One of the three failures was the goto symptom above by
a route the fixes did not cover; the other two were this:

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

**The emitter half of this is now fixed.** The entry is kept because the
reasoning it recorded is what the fix followed, and because the corpus effect
is confirmed by CI rather than here.

The blocker recorded first was that `bin2llvmir`, where the extra argument
originates, needs the pinned LLVM and cannot be built or tested in this
environment — so a change could only be validated by a CI round trip. That was
true of the *front end*. It was not true of the fix, which lives entirely in
`llvmir2hll`, and `scripts/ci/check_llvmir2hll_tests.sh` (L2H-01) builds and
runs that in about four minutes locally. Re-reading a stated blocker to see
whether it covers the thing actually proposed is worth doing; this one did not.

The three repairs rejected then are still rejected, for the reasons given:
truncating to what `getNameOfParam` knows breaks `printf`; emitting retdec's
own prototype trades *too many arguments* for *conflicting types*; suppressing
one header does nothing when another function pulls it in.

What settled it is the fourth option the entry named — an arity and a variadic
flag in the semantics layer, `Semantics::getArityOfFunc()`, beside
`getCHeaderFileForFunc()` where this knowledge already lives. The entry also
said "a table is only worth adding if it is right", so the table is **measured
rather than recalled**: `scripts/ci/check_libc_arity.py` compiles a call with
0–8 arguments against the real header for each of the 469 names the semantics
assign one, and reads the arity off which counts the declaration accepts. One
accepted count is a fixed arity; acceptance from N upwards is variadic with N
named parameters; anything else — a function-like macro such as `isnan`, or a
name not declared on the platform — is left out rather than guessed. 454
entries, 15 left out. Fifty-four independently known arities were cross-checked
against the C standard and all agreed.

That script is also ARITY-01, a CI check: the table cannot drift from the
headers unnoticed. It was self-tested four ways — a wrong arity, a missing
entry, an unmeasurable entry, and a wrong variadic flag — and catches each.

`CHLLWriter::visit(CallExpr)` now drops arguments a known, non-variadic
declaration cannot take. Variadic functions and unknown functions are left
alone; `std::nullopt` from the semantics means "no opinion", not "no
parameters".

What is *not* verified here is the corpus effect: running the decompiler over
`ring_buffer-gcc-O3` needs the front end, so whether CC-01's rate rises is for
CI to say. The floor stays at 0.8750 until a run measures otherwise, and
should be raised with the measurement.

**Run 266 said it.** 23/24, rate 0.9583 — both arity failures gone, and the
floor is now that number. The one file left is `generated_shell_sort-gcc-O2`,
`label 'lab_0x112c' used but not defined`: the goto symptom, and the fourth
route to it is the last entry of the section above.

---

## 3. Silent miscompilation, which is worse than a compile failure

These produce C that compiles cleanly and does the wrong thing. Nothing in the
pipeline catches them, because `tu_valid` and `recompile` both pass.

**~~`if_to_switch_optimizer.cpp` — range comparisons become single cases.~~**
**Fixed, and four tests asserted the defect.** An unbounded `v >= k` / `v > k`
else-if chain became a `switch` whose cases are single values:

```c
if (v >= 2) b = 2; else if (v >= 1) b = 1; else b = 0;
switch (v) { case 2: b = 2; case 1: b = 1; default: b = 0; }
```

`v = 7` sets `b = 2` in the first and `b = 0` in the second. Every value above
the highest bound is wrong, signed or unsigned. The four conversions are gone —
both `>=` and `>`, in the chain and the single-clause shape — and three tests
assert the chains stay as they are.

A correct construction exists for the *unsigned* case, and the comment where
the calls used to be says what it is: the top clause is the unbounded one, so
it belongs in the `default` and the else-clause body becomes a case. It is a
different transformation, it is wrong for signed compares for the reason below,
and it should arrive with F1, DET-01 and CC-01 behind it.

**~~`if_to_switch_optimizer.cpp` — signedness ignored.~~** **Fixed.** The
`Lt` and `Le` upper-bound patterns did not distinguish the signed compare from
the unsigned one, so `if (v < 1) A; else if (v < 2) B; else C;` on a signed
control expression became `switch (v) { case 0: A; case 1: B; default: C; }` and
every negative input took `C`.

The information was there: `ICMP_ULT` converts to `LtOpExpr::Variant::UCmp` and
`ICMP_SLT` to `SCmp`. The `Lt` matcher never read the variant; the `Le` matcher
read it only to require that every clause agreed with the first, which a chain
of signed compares does. Both require `UCmp` now.

`ElseIfLeUpperBoundChainConvertsToSwitch` built its chain with `SCmp` and
asserted the conversion — a test that encoded the defect. It is `UCmp` now, and
the signed shapes have tests of their own asserting refusal. Verified by
restoring the original file: all five new tests fail.

The nested and hoisting conversions in the same pass are unaffected and were
checked: their switch cases come from exact `v == k` tests rather than from
bounds, so a negative or out-of-range value matches no case and reaches the
same default it reached before.

**~~`copy_propagation_optimizer_ext.cpp` — propagation across an if.~~**
**Measured; does not reproduce, and the reading was right about the wrong
thing.** `canPropagateThroughStmts` is exactly as described: its walk is
`cur = cur->getSuccessor()`, so an IfStmt's body is stepped over, and
`isWrittenIn` only recognises an AssignStmt whose LHS is the variable — which
an IfStmt never is.

It is a *permission*, not the decision. `t = f(); if (c) { t = 0; } g(t);` gives
`g(t)` two reaching definitions of `t`, so `CopyPropagationOptimizer` takes its
"more than one definition" branch and refuses at the first check there, that all
the definitions are identical. Measured on a well-formed module: the argument
stays a variable. Forcing `canPropagateThroughStmts` to return `true`
unconditionally does not change it; bypassing the use-def branch does, which is
what makes the test evidence.

Two cases in `copy_propagation_optimizer_tests.cpp`: the dangerous shape is
refused, and an `if` that touches nothing the propagation cares about still
propagates — so a future repair of the walk cannot quietly become "refuse
everything".

One note for whoever reads the entry next: the first attempt at this
reproduction used a bare `Variable` named `f` rather than a declared `Function`,
so `getFuncByName` returned null and the optimizer bailed four guards earlier.
It looked like a refusal and was a malformed test.

**~~`while_true_to_for_loop_optimizer_ext.cpp` — loop direction by name.~~**
**Fixed, and three tests asserted the defect.** The function is
`isNonNegativeExt`, and its second branch was a substring match on the
variable's *name* — `size`, `len`, `cnt`, `num`, `idx`, `uint` — returning
`true` whatever the type.

A name is not a fact about a value. `int size = -1` is the commonest sentinel
there is, and a signed `idx` walking backwards is routine. What the answer
decides is whether a loop's exit test may be rewritten from `indVar != endValue`
to `indVar < endValue`, which is equivalent only when the step is non-negative:
with a negative step and `start > end`, `!=` runs the loop and `<` does not run
it at all.

The branch is gone. The type still answers when it can, which is sound and is
what should have decided it. The three tests — one per name, each on a *signed*
32-bit variable — now assert the refusal, and a fourth says the name stops
mattering in both directions. Verified by restoring the heuristic: all three
fail.

---

## 4. The public API cannot report failure — **all four fixed, and now read**

This section stood as an open list long after the code stopped matching it,
which is its own finding: a doc that sends the next reader to re-fix finished
work. Each item was checked against the source before this note was written.

* ~~`src/retdec/retdec.cpp` — `decompile()` is declared `bool` and returns
  `EXIT_SUCCESS`, which is `0`, which is `false`.~~ **Fixed.** It returns
  `true`, and the comment at that return records what the defect was.
* ~~`parallelBatchDecompile()` consequently reports every job as failed.~~
  **Fixed** by the above: `results[i] = futures[i].get()` takes `decompile()`'s
  answer directly, and a thrown exception still gives `false`.
* ~~`src/llvmir2hll/llvmir2hll.cpp` — failing to open the output file exits 0
  having written nothing.~~ **Fixed.** `getOutputStream()` returns `{}` on the
  error code and the caller returns `false`.
* ~~`src/retdec-decompiler/retdec-decompiler.cpp` — there is no unknown-option
  diagnostic anywhere in the CLI parser.~~ **Fixed.** `ProgramOptions::load()`
  throws `unknown option: <opt> (see --help)`, and `main` catches it and
  returns `EXIT_FAILURE`.

**None of the four was read by anything, which is how all four got in.** An
exit code nothing checks is an exit code nothing keeps right, and the code was
correct here only because somebody had recently looked.

`scripts/ci/check_cli_exit_codes.sh` (CLI-01) reads them, from ctest-linux where
the built decompiler is: an unknown option must exit non-zero, an unopenable
`-o` path must exit non-zero, and — the part that matters — a real decompilation
must exit zero having written something, because a gate that only asserts
failures passes on a decompiler that fails at everything.

Its self-test runs in the push gates and is two stub decompilers: one behaving
as the source does now, one exiting 0 for everything, which is the state this
section recorded. The check must pass the first and reject the second by name.

---

## 5. The SSA the detectors run on is not the SSA they were written against — **fixed and gated**

`src/retdec/llvm_to_ssa.cpp`

**Both halves are done, and this section stood open after they were.** That is
the second time in this file — section 4 was the first — and it costs what a
hidden defect costs: the next reader is sent to redo finished work. Checked
against the source before this note was written:

* `IrInstr::defValue` **is** assigned. `buildSsaModule`'s third pass sets it for
  every instruction with a non-void type, from the value map. The comment at
  `translateInstr` says why it cannot be done one instruction at a time: a phi
  at a loop header names a value from the latch, which has not been translated
  yet.
* **Every operand becomes a use**, for every translated instruction. The fourth
  pass is exactly the shape "The fix" below prescribed — a memo minting one id
  per LLVM value, a fresh `Immediate` per constant occurrence, a shared
  `VirtualReg` id otherwise — and the four-instruction-class restriction and the
  `ConstantInt`-only `continue` are both gone.

It is gated, too, which is the part section 4 was missing.
`scripts/ci/check_llvm_adapter.sh` (ADAPT-01) builds this file against the
system LLVM and runs `tests/retdec/llvm_to_ssa_test.cpp`, whose assertions are
not "some instructions define something": one requires that **every**
value-producing instruction defines a value, and another requires that the
def-use graph is actually connected — that at least one used value is one some
instruction defines. Either would fail on the state described below.

**What no longer follows.** The table further down lists four detectors as dead
on production input. That consequence rested on the adapter, and the adapter
delivers now. Whether each of the four contributes is a separate measurement —
the F1 gate reports the aggregate, not per detector — so the table is left as
the record of what was found rather than rewritten into a claim nothing here
took.

The rest of this section is kept as written, because the reasoning is what made
the fix possible and the "how it was found" is worth more than the list.

---

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

~~Eleven of the thirteen test sources in `tests/fileformat/` are named by no
`CMakeLists.txt`. Whether they still compile against the LLVM 23.1.0 migration
is not known here.~~ **Measured; they all compile, and they all run.** The
"not known here" was the load-bearing part of this entry and it was wrong:
`scripts/ci/check_fileformat_tests.sh` (FF-01) already compiled `src/fileformat`
against the distribution `llvm-dev`, so adding the directory was one line.
Every one of the sixteen files in `tests/fileformat/` compiles, and 83 of the
suite's 129 cases had been evaluated by nothing.

They stay out of the CMake target because nothing builds it — `ctest-linux`
names `retdec-decompiler`, `retdec-gui` and `retdec-gui-tests` and no other —
so listing them there would move them from one gate to none.
`scripts/check_cmake_sources.sh` carries them with that reason now, rather than
with "not yet re-enabled".

`tests/common/calling_convention_tests.cpp` is still unbuilt and still in that
list.

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

## Behind a build this environment cannot run — except it is not

This section's heading was the reason nothing here was attempted: "everything
below is compiled only by the LLVM-dependent build, so a change to it here would
ship untested — all were read and measured against the source; none were
compiled."

That was checked and it is false. `scripts/ci/check_fileformat_tests.sh` (FF-01)
compiles all of `src/fileformat`, `src/loader`, `src/common`, `src/pelib` and
`src/serdes` against the distribution `llvm-dev` and has done since it was
written — which is every file in the list below but the last, and that one is in
`src/cli_parser`, a module `standalone_check.sh` already builds. The premise held
when the audit was written and stopped holding when FF-01 landed; nobody went
back and re-read it. It is the fourth exclusion reason on this branch to survive
right up until somebody measured it, after `tests/cpdetect`+`tests/loader`,
`tests/unpacker`, and the `llvmir2bir_converter` exclusion on L2H-01.

So these were ordinary findings with an ordinary gate behind them. All seven
are resolved now, and the split is worth stating: **three were real** — the
`getRealSizeInRegion` clamp, the two loader clamps, and the CIL signature
traversal, which did not terminate on nine bytes — and **four were bounded by
something the entry had not looked for**: two readers that refuse an
out-of-range table, a check four calls upstream, and a short-circuit.

Each of the four says so below with the measurement, and three of them were
given a local bound anyway. Being unreachable is not the same as being
bounded, and the distance between the check and the arithmetic is what the
entries kept getting wrong in both directions.

* ~~`src/fileformat/file_format/elf/elf_format.cpp` and `coff_format.cpp` —
  `getDeclaredFileLength` overrides that shadow the base function which *was*
  fixed, each forming unclamped 64-bit sums (and, in the COFF case, an unchecked
  multiply) of header fields.~~ **Measured; not reachable, and the finding was
  wrong about why.** The sums are exactly as described — neither override
  clamps anything — but nothing gets hostile values into them.

  `ElfFormat` forms `max(shoff + sectionTableSize, phoff + segmentTableSize)`.
  ELFIO reports a table size of zero unless the table lies inside the file, so
  `e_shoff = 0xFFFFFFFFFFFFFFFF` arrives with a size of 0 and the sum is the
  offset itself. Driven at the top of the address space, with `e_shnum` at
  `0xFFFF`, the declared length comes back as `18446744073709551615` — the
  offset, unwrapped.

  `CoffFormat` forms `symbolTableOffset + numberOfSymbols * entrySize` and then
  `declaredSize + sizeOfStringTable`. LLVM's `COFFObjectFile` reports zero
  symbols for a table it could not read, which is the condition the override's
  own `if` already tests, and `getSizeOfStringTable` refuses an offset past the
  end before reading. With both 32-bit fields at `0xFFFFFFFF` the symbol count
  is 0 and the string-table size is 0. The multiply is 32×32 into a 64-bit
  `std::size_t` in any case, so it cannot overflow on any host this tree builds
  for; on a 32-bit one it would, and nothing here asserts otherwise.

  That makes both sums safe because of code some distance from them, which is
  not a thing to leave unwritten. `tests/fileformat/declared_file_length_tests.cpp`
  pins the invariant in five cases: a reader that starts reporting an extent for
  a table it could not read makes the sum wrap and the declared length fall
  below the offset it was formed from. Verified by making
  `ElfFormat::getSectionTableSize` return a size for a table ELFIO declined —
  the test fails, naming that.
* ~~`src/fileformat/file_format/file_format.cpp` — `getXByte` still forms
  `secSeg->getOffset() + secOffset`.~~ **Fixed, though not reachable.** The sum
  is real: `sh_offset` is unbounded — ELFIO validates that the section *table*
  lies in the file, not that each section's data offset does — so at
  `0xFFFFFFFFFFFFFFFF` it wraps to a small number the containment test below
  finds comfortably inside the file, and the read returns the wrong bytes from
  a valid address. It is not reachable because a section whose `sh_offset` is
  outside the file has nothing loaded, so `getLoadedSize()` is 0 and the *first*
  `rangeFitsWide` refuses before the second is evaluated.

  That is a property of the loader, and the sum is here. `regionEndSaturating`
  is the same arithmetic without the wrap, is proved over the whole 64-bit
  domain, and has cases in `tests/loader_sim/xbyte_width_guard_test.cpp`. Every
  offset that did not wrap is unchanged. No new test: the kernel is tested and
  the substitution cannot change a non-wrapping answer — pinning the call site
  would need an ELF with a valid section table and a hostile `sh_offset`, which
  is worth building the day something makes this reachable.

* ~~`file_format.cpp` (`computeSectionTableHashes`) and (`getOverlayEntropy`) —
  unchecked multiply and sums on header fields.~~ **Measured; neither can
  wrap.**

  `getOverlayEntropy` computes `bytes.size() < declSize + overlaySize`, and the
  sum cannot wrap by construction: `getOverlaySize()` returns `realSize -
  declSize` when `realSize > declSize` and 0 otherwise, so a non-zero
  `overlaySize` makes `declSize + overlaySize` exactly `realSize`, and a zero
  one short-circuits the `||` before the sum is formed. The read after it is in
  bounds for the same reason.

  `computeSectionTableHashes` forms `getSectionTableOffset() + i *
  getSectionTableEntrySize()` with `i` bounded by `sections.size()`. A non-empty
  `sections` means ELFIO loaded the table, which means `e_shoff` is inside the
  file — the same invariant `tests/fileformat/declared_file_length_tests.cpp`
  already pins, since it asserts a table size of zero for an out-of-range
  offset and that size is `sections.size() * entrySize`. `i * entrySize` is at
  most `sections.size() * 65535`, both bounded by the file, so the product does
  not overflow a 64-bit `std::size_t` either.
* ~~`src/fileformat/utils/other.cpp:407` — `getRealSizeInRegion`'s wrapping
  clamp.~~ **Fixed.** `offset + requestedSize > regionSize` is now
  `requestedSize > regionSize - offset`, which asks the same question without
  the sum; `offset < regionSize` is established two lines above, so the
  subtraction cannot wrap either. Six cases in `tests/fileformat/other_tests.cpp`,
  four of them pinning the in-range answers so the clamp cannot be tightened by
  accident. Verified by restoring the old line: the two wrapping cases fail and
  the four in-range cases do not.
* ~~`src/fileformat/types/resource_table/bitmap_image.cpp` — unchecked
  `nBytesInRow * nRows` from header width/height/bitCount, feeding the clamp
  above.~~ **Measured; not reachable, and the number is worse than the entry
  said.** The product is right and it is not a wrap: at width `0xFFFFFFFF`,
  height `0xFFFFFFFF` and bitCount `0xFFFF` it is `1152903920473874428` — 1.15
  exabytes — which is *below* `std::vector<std::uint8_t>::max_size()`, so
  `bytes.reserve` of it is an allocation attempt rather than a
  `std::length_error`. It raises `std::bad_alloc`, and nothing between
  `BitmapImage` and `main()` catches one. `image.reserve(height / 2)` is 51 GB
  of `std::vector` on its own.

  Nothing gets there. `parseDibFormat` is the only caller of any
  `parseDib*Data`, and it runs `parseDibHeader` first, which refuses
  `width > 512`, `height > 1024` and `bitCount > 32` — so `nBytes` on that path
  is at most `2048 * 512`, one megabyte.

  The bound is four calls from the multiplication, nothing near it says so, and
  every `parseDib*Data` is public. So the reserves are now clamped to the icon's
  own loaded size with `bounds::reserveFor`, which is a cap on the *hint* and
  not on the parse: `reserve` only pre-allocates, the containers still grow to
  whatever is read, and the `bytes.size() != nBytes` test after each read
  already refused anything short. No input that parsed before parses
  differently.

  `bounds::reserveFor` is ESBMC-proved and, until this, had no callers anywhere
  in the tree. It was written for this shape.

  `tests/fileformat/bitmap_image_tests.cpp` calls the public methods directly
  with a header `parseDibHeader` would have refused. Two of the six are
  evidence: 24 and 32 bpp have no palette to read first, so they reach the
  reserves, and removing the clamp makes exactly those two fail with
  `std::bad_alloc`. The other four say so in the file rather than being counted.
* ~~`src/loader/loader/segment.cpp`, `segment_data_source.cpp` — post-hoc and
  pre-copy wrapping clamps. `Image::getXBytes` now refuses before them, but both
  have other callers.~~ **Fixed.** These were the real ones in this list. Unlike
  the `getDeclaredFileLength` overrides and the `BitmapImage` product, nothing
  upstream bounds them and what they feed is a `std::copy`:

  ```cpp
  loadSize = loadOffset + loadSize >= getDataSize() ? getDataSize() - loadOffset : loadSize;
  std::copy(_data.data() + loadOffset, _data.data() + loadOffset + loadSize, ...);
  ```

  At `loadOffset` 1 with `loadSize` `0xFFFFFFFFFFFFFFFF` the sum is 0, which is
  not `>=` any data size, so the clamp is skipped and the copy reads
  18446744073709551615 bytes out of a four-byte buffer. `saveData` is the same
  shape with the copy going the other way, and `Segment::getBytes` is the same
  shape ending in `result.resize(size, 0)`.

  All three now compare against the space that remains, which the guard two
  lines above each already established cannot wrap — the same repair as
  `getRealSizeInRegion`, and equally answer-preserving: where the old test
  clamped on equality, so does the new one.

  `saveData` also gained a bound it never had: `saveSize` was checked against
  the destination and never against the source, so `saveData(0, 100, aVectorOfTen)`
  read ninety bytes past the end of the caller's vector. Every caller in this
  tree passes a long enough vector; none of them says so.

  Five cases in `tests/loader/segment_data_source_tests.cpp` and three in
  `segment_tests.cpp`. Verified by restoring each old line: four fail for
  `SegmentDataSource` and one for `Segment`. The two that do not are labelled in
  place — one is the guard the clamps rest on, and the other uses offset 0,
  where the sum does not wrap and the old clamp was right.
* ~~`src/cli_parser/cli_sig.cpp` — a self-referential TypeSpec whose signature
  is a GENERICINST with two or more self-referencing type arguments drives an
  exponentially wide traversal under the depth-64 bound.~~ **Fixed, and it
  reproduced.** This entry was right, including the byte sequence. As a field
  signature it is nine bytes:

  ```
  06 15 12 06 02 12 06 12 06
  ```

  FIELD, GENERICINST, CLASS TypeSpec row 1 — the row itself — two arguments,
  each the row itself again. Two branches per level, 64 levels: 2^65 nodes.
  `kMaxTypeDepth` never trips, because the cycle never gets deeper than 64.

  Measured against a resolver modelled on `CLIReader::typeSpecType`: **2,000,000
  resolver calls in 0.69 s, and still descending** when a counter in the test
  stopped it. At that rate 2^65 nodes is about 10^4 years. It does not
  terminate.

  The fix is not the visited-set this entry proposed — that needs
  `ITypeNameResolver` to change, and its other implementation is behind the LLVM
  build. A budget does the same job from inside this file: `kMaxTypeWork`
  counts levels *entered* rather than levels *deep*, in a `thread_local` beside
  `g_typeDepth`, reset only when a descent begins at depth zero. The cycle
  re-enters through the resolver with the enclosing frames still holding their
  guards, so it never sees zero and never gets a fresh budget. 100000 nodes is
  about 33 ms at the rate above, and four orders of magnitude above what a
  language compiler emits — `Dictionary<string, List<int[]>>` is a dozen nodes.

  After: exactly 100000 calls, in 40 ms.

  Three cases in `tests/cli_parser/cli_sig_width_test.cpp`. The resolver keeps a
  cap of its own so that a regression fails the test in about two seconds
  instead of never returning — a test that hangs is not one anyone can run.
  Verified by removing the budget: the branching case fails and names the line.
  The other two are labelled controls — the non-branching cycle, which the depth
  bound already handled, and an ordinary nested generic, which a budget that
  refused real work would break.

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

~~What is still open is one step further: the CUDA half is **compiled but not
executed** by the suite.~~ **Closed.** It is executed now, by the second test
binary this entry proposed.

Two things were needed, and neither was the `GpuScanner` split the entry
offered as the alternative:

* **The object.** `gpu_scanner.cu` is compiled without
  `RETDEC_GPU_SCANNER_HOST_ONLY` and passed to the link *explicitly*, so the
  linker satisfies `GpuScanner`'s methods from it and never pulls
  `gpu_scanner_cpu.o` out of `libutils.a`. Archive members are only taken to
  resolve what is still undefined, so the two never collide.
* **A device.** `cudaMalloc`, `cudaMemcpy` and `cudaFree` were already real host
  operations, but `cudaGetDeviceCount` returned 0, so `ensureGpu()` took its
  "CUDA absent" path and every method fell back to the host code the `utils`
  suite already covers — the kernels would still not have run. The stub
  presents one device now, behind `cudastub::presentOneDevice()`, off by
  default so every existing user of the header sees what it saw before.

**The assertions are `tests/utils/gpu_scanner_tests.cpp`, unchanged and not
copied** — the same 17 the `utils` suite runs against the CPU path. Two
implementations of one class, one set of assertions, and they have to agree.
That is worth more than a second set written for the device path alone, which
would only ever say what its author expected.

Verified by repeating the experiment this section describes. An off-by-one in
`findAllKernel` — `pos + needleLen >= dataSize` for `>` — is inside the
device-only region:

```
  ok  utils — 351 tests ran
 FAIL cuda half (tests)
[  FAILED  ] GpuScannerTests.UploadedFileIsScannable
```

The CPU suite does not notice, which is the state that let four fixes be
reverted with the suite staying green at 13/13. The new check does.

The first version of it reported `runs 0 test(s)` and returned 0, because the
count came from a `[ OK ]` grep that `--gtest_brief` suppresses. A binary that
runs no tests passes every assertion it has, so the count is read from the
"N tests ran" line now and floored at `CUDA_HALF_MIN_TESTS`.

What the stub still does not model is at the top of
`tests/utils/cuda_stub/cuda_runtime.h`: no warp semantics, no coalescing,
blocks serialised rather than concurrent. A kernel with an inter-block race
passes here and fails on a device.

### The Imortek C back end is compiled, unit-tested, and linked into no tool

`CodeGenPass::generateFunction()` and `generateUnit()` — the entry points of
`src/codegen`, which turns SSA plus a `StructNode` tree into a C AST — have
no caller anywhere in `src/`. Their only callers are `tests/codegen`.

The link graph says the same thing from the other end. `retdec-codegen` is
linked by exactly one target, `retdec-cxx-backend`; `retdec-cxx-backend` is
linked by exactly one target, `retdec-cxx-backend-tests`.
`retdec-decompiler` links `retdec::llvmir2hll` — the Avast back end — and
neither of the two. So `src/cxx_backend` consumes a `codegen::CUnit` that
nothing outside a test ever produces.

This is not the same as `src/opencl`, which no `add_subdirectory` names:
these are built, warned about, and unit-tested on every run of
`scripts/standalone_check.sh`. What they are not is *reached*. Every defect
found in them on this branch — the goto with no label, `CondNormaliser`
having no comparison to match, `GotoEliminator` having no label to fold
against, `Stats` counters that could only read zero — is a defect in code
the shipped decompiler does not execute.

That does not make the fixes wrong; a back end being built toward should be
correct before it is switched on, and each was verified by reverting it. It
does mean **none of them can explain a CC-01 failure**, which measures
`retdec-decompiler` and therefore `llvmir2hll`. Where a commit message on
this branch says the emitted C was broken by one of these, read it as the
code being broken rather than the output — the correction below says so
where it matters.

Closing this is not a small change and is not attempted here: it means
deciding whether the Imortek back end replaces `llvmir2hll` or sits beside
it behind an option, and either answer is a design decision with a corpus
measurement attached, not a wiring fix.

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












## The README published a number its own CI contradicted

`README.md`'s results table led with

| Recompile, default `.c` | 0/216 | 0/216 |

and explained it: `NoInitVarDefOptimizer` deletes every initializer-less local
declaration, so the emitted C assigns to variables it never declares —
"unfixed because it cannot be tested without the LLVM build".

Every clause of that was out of date:

* The pass is **not run** for the C back end. `optimizer_manager.cpp` says so
  and gives three reasons, each with a file and line.
* It **can** be tested without the LLVM build.
  `scripts/ci/check_llvmir2hll_tests.sh` runs that component against the
  system LLVM in about four minutes.
* The number is **contradicted by this repository's own CI**. CC-01 hands the
  emitted `.c` to a compiler on every `ctest-linux` run and measured 21/24,
  then 23/24, then 24/24 — against a published 0.

A stale measurement is replaced by another measurement, not by an argument,
so CC-01 now runs over the whole 216-binary corpus on **every `ctest-linux`
run**, not weekly. That cadence was a measurement too: run 272's CC-01 step
took ten seconds for 24 binaries, so 216 is about ninety seconds against a
job that already spends eight minutes building and seventeen in clang-tidy.
It went into the nightly first, on the assumption that a full-corpus pass was
expensive; the step timings said otherwise and it moved.

No `--min-rate` on the full corpus yet: it reports, and the number becomes
the floor in a follow-up commit that names the run it came from, exactly as
the 24-binary slice was introduced. Until that number exists the tables say
so, rather than printing a 0 nobody believes or a 216/216 nobody has
measured.

Stock RetDec's `0/216` is untouched — it was not re-measured and no claim
about it changes.

## `lab_0x112c`, found

`splitWhileTrueLoop()` refuses to split a `while (true)` loop when something
jumps into it:

```cpp
while (currStmt) {
    // When a statement in the loop is a goto target, we cannot split the
    // loop. Otherwise, after optimizing the loop, we might end up with
    // incorrect code.
    if (currStmt->isGotoTarget()) {
        return {};
    }
```

The comment is right. The test is one level too shallow: `currStmt` walks the
body's own successor chain, and nothing nested inside it.

All three `while_true_to_*` optimizers call this function, and all three keep
`splittedLoop->beforeLoopEndStmts` as the new body and throw the loop-end
`if` away. The `break` or `return` inside that `if` is exactly where a goto
lands, and exactly what is nested rather than top-level. Its label went with
the discarded `if` while the goto naming it stayed —
`goto lab_0x112c;` with no `lab_0x112c:`, which is what CC-01 had reported on
`generated_shell_sort-gcc-O2` for five rounds.

The question is asked over the whole body now, once, with
`GotoTargetAnalysis::hasGotoTargets()`. Three lines.

**Run 272 measured the effect: CC-01 24/24, rate 1.0000.** Every emitted C
file on the slice compiles, and the floor is that number now. F1 stayed at
0.2302 and DET-01 at 72 binaries with 0 skipped, so declining to convert
loops something jumps into cost neither detection quality nor determinism.

**It refuses more than it strictly has to**, and deliberately. Only the
loop-end `if` and `afterLoopEndStmts` are discarded; a goto into
`beforeLoopEndStmts`, which the new body keeps, is harmless. Checking only
the discarded region would convert more loops than the old code did — the
old test covered the whole top-level chain too — so it is a behaviour change
beyond fixing the defect, and it needs the F1 and DET-01 numbers behind it
rather than an argument. The narrow version also has to run *after* the
split, since the split is what decides which parts are discarded. Left for a
round that can measure it.

### How it was found, after four wrong answers

Four earlier rounds each read the passes, found a real defect, fixed it, and
watched the error come back at the same line. What broke the pattern was not
a better guess:

1. **`--save-failures`** made CC-01 keep the emitted `.c` and the `.ll` the
   back end was given, instead of deleting both with its temp directory.
2. **The `llvmir2bir_converter` exclusion** came off `check_llvmir2hll_tests.sh`
   once the reason was measured rather than assumed, which is what let the
   converter run here at all.
3. **`OptimizerManagerPipelineTests`** turned that into a local pipeline:
   LLVM IR in, thirty passes, emitted C out, scanned for a label jumped to
   and never defined.
4. **One CI round** produced the real IR. Reduced to the failing function it
   is 200 lines, and it reproduced on the first run.
5. **Delta-debugging** `OptimizerManager`'s `enabledOpts` found the smallest
   set of passes that still stranded the label: one. Then the next one. Then
   the shared helper both of them call.

Five rounds of reading to narrow it to a subsystem; about two minutes to name
the function once the input could be replayed. The reduced IR is now a test,
so the next regression is caught before CI rather than five rounds after it.

One honest note: of the three per-pass regression tests,
`WhileTrueToForLoopAloneKeepsTheLabel` passes with the fix reverted —
`WhileTrueToForLoopOptimizer` declines this particular input earlier, for an
unrelated reason. It is kept as a control on the third caller, not claimed as
evidence. A unit test written to make it load-bearing was dropped rather than
kept: it also declined for the unrelated reason, so it could not fail either.

## `removePredecessors(onlyNonGoto)` removed gotos

```cpp
void Statement::removePredecessors(bool onlyNonGoto) {
    ...
    // We remove only non-goto statements.
    for (const auto &pred : preds) {
        if (pred->getSuccessor() == thisStmt) {
            toRemoveStmts.insert(pred);
        }
    }
```

The comment says non-goto and the test says fall-through, and a goto passes
the fall-through test whenever it sits immediately before the statement it
jumps to: in `goto L; L: ...` the goto's *successor* is its own target.

So "only non-goto" dropped that goto from `L`'s predecessors.
`Statement::isGotoTarget()` walks `preds`, so it then answered `false`;
`CHLLWriter::emitGotoLabelIfNeeded()` asks exactly that question and stopped
writing `L:` — while the goto naming it stayed. Both statements still
emitted, one label missing, which is the shape of `goto lab_0x112c;` with no
`lab_0x112c:`.

Five callers, and every one wants the same thing: `setSuccessor()`,
`prependStatement()`, `IfStmt::addClause()` and the two clause setters each
mean "this is now the one statement that falls through into you". A goto
does not fall through into anything — its successor is where it sits, not
where it goes. Gotos are skipped now; `removePredecessors(false)` still
means all of them, and has a test saying so.

This one is in `llvmir2hll`, which is the path `retdec-decompiler` runs.

### The emitted C is now checked for undefined labels without CI

`OptimizerManagerPipelineTests` gained the question CC-01 actually asks. It
converts LLVM IR, runs the pipeline, hands the module to `CHLLWriter`, and
scans the emitted C for a label jumped to and never defined — resetting at
each function, because a C label is function-scoped. `label 'lab_0x112c'
used but not defined` is a compiler asking this; asking it here costs two
minutes instead of a corpus binary, a front end and a thirty-minute round
trip. The scan has its own test, including a label defined in the *next*
function, since an assertion that cannot fail is worse than none.

Getting the writer to run at all needed one fix.
`HLLWriter::emitAddressRangeForFuncIfAvailable()` decided "available" by
comparing against `NO_ADDRESS_RANGE`, which is `AddressRange(0, 0)` — a
sentinel. A range that is merely *undefined*, which is what a `Config`
returns when it knows nothing about the function, is not equal to that
sentinel, so it went past the guard into
`Address::toHexPrefixString()`, which asserts `isDefined()`. The guard asks
whether the endpoints are defined as well now. The shipped `JsonConfig`
returns the sentinel, so this was latent there; it aborts immediately on a
config that does not.

## The Imortek codegen wrote every `goto` with no label to match

`src/codegen/emitter.cpp` emits `goto L<blockId>;` for a
`StructNode::Kind::Goto` — which is what `CompilerStructurer` produces for
every irreducible region and every back edge it cannot fold. Nothing emitted
the label. `CStmt::labelStmt()` exists, `Emitter::emitStmt` renders
`Kind::Label`, `GotoEliminator` reads `Kind::Label` statements — and the only
callers of `labelStmt()` anywhere in the tree are five lines of
`tests/codegen/codegen_test.cpp`.

So the back end could not emit a `goto` and a working translation unit at the
same time. One undefined label is not one bad function: the C compiler
rejects the file, and every other function in it goes with it.

`gotoLabelsFor()` now walks the `StructNode` tree once, pairs each `Goto`
node with the block it names, and the `Block` case emits that label. Two
gotos to one block agree on the spelling because the label is looked up from
the same map that the goto is.

A goto naming a block the tree has no `Block` node for is dropped rather than
emitted: a label can only go on a block, so such a goto has nowhere to land,
and a wrong function is less destructive than a file no compiler will read.
The structurer only emits a goto to a block it has already visited, so this
guard is on a shape it does not currently produce — it has a test because a
guard nothing exercises is indistinguishable from one that does not work.

Both halves verified by reverting them one at a time.

**And the goto eliminator had never eliminated anything.**
`GotoEliminator` matches a goto against a `Kind::Label` statement —
`countLabelsInTree()` collects them and `rewriteStmtTree()` closes its guard
at one. With no label ever created, that set was always empty, no goto was
ever eliminable, and `Stats::gotosEliminated` was structurally zero on every
input: the same shape as `condRewrites` and `castsRemoved` one section down,
a pass that cannot fire beside a counter that cannot move.

Emitting the label turns the pass on. It is now the reason the first test
above has to set `enableGotoElim = false` — with it on, the eliminator folds
that goto away, which is the pass working and would have hidden the
emitter's half. A third test asserts the other side: with elimination on, the
same input reports `gotosEliminated == 1`, `gotosRemaining == 0`, and emits
no `goto` at all.

## The whole back end, from LLVM IR, in one test

Every other test under `tests/llvmir2hll` exercises one pass on a module
built by hand. What the decompiler actually does — convert, then run thirty
passes in order, each on the output of the last — was covered by nothing but
the corpus, which needs the front end and a CI round trip.

`OptimizerManagerPipelineTests` takes LLVM IR, converts it, runs
`OptimizerManager` over the result, and asks whether every goto still has a
target the emitter reaches. Three shapes so far: a multi-exit loop, the
nested multi-exit shape read off `generated_shell_sort-gcc-O2`'s emitted C,
and an irreducible two-block region. All three hold, so none of them is the
shape that loses `lab_0x112c`.

The harness takes a `disabledOpts` set, which is the point: once a shape does
reproduce, bisecting the pipeline names the pass in a few local runs rather
than a few CI rounds.

What is missing is the real input. `--save-failures` now keeps the `.ll` the
back end was given alongside the `.c` it produced — the decompiler writes it
beside its output and only removes it with `--cleanup`, which CC-01 does not
pass. One CI round hands over the exact IR for the function that fails, and
that IR goes straight into this harness.

## 384 more tests, and the pass that decides what becomes a `goto`

`scripts/ci/check_llvmir2hll_tests.sh` excluded
`src/llvmir2hll/llvm/llvmir2bir_converter/` and `tests/llvmir2hll/llvm/`
outright, with a reason recorded: they use LLVM APIs that moved between the
system LLVM and the pinned one, naming
`ConstantPointerNull::getPointerType` and the `LoadInst` constructor overload
set.

Both were true. Neither justified the exclusion. Compiling the twenty sources
and nineteen test files one at a time against LLVM 18 found **one**
production call and **two** calls in one test file that did not build:

* `cNullPtr->getPointerType()` → `cNullPtr->getType()`. `ConstantPointerNull`
  specialises `getType()` to return a `PointerType*` in every LLVM this code
  has been built against; `getPointerType()` went with typed pointers.
* `new llvm::LoadInst(ty, p, "", false, Align(1), nullptr)` — `nullptr`
  matches both the `Instruction *InsertBefore` and `BasicBlock *InsertAtEnd`
  overloads. Dropping the argument takes the `InsertBefore` default and is
  unambiguous everywhere.

Three lines. What they were keeping out included the nine hundred lines of
`structure_converter.cpp`, which is where retdec decides what becomes a
`goto` — the component behind the one CC-01 failure that three rounds of
reading pass sources had not found. The suite goes from 1,793 tests to
2,179, and the floor from 1,500 to 2,000.

This is the third time on this branch that a recorded reason for not running
something did not survive being checked: `tests/cpdetect` and `tests/loader`
were said to need the pinned LLVM and did not, `tests/unpacker` turned out to
need YARA rather than LLVM, and now this. Writing the reason down is what
makes it checkable; checking it is a separate act.

The converter's own goto handling now has a regression test asserting the
property that matters — every goto the emitter reaches has a target it also
reaches — on a multi-exit loop and on the nested multi-exit shape
`generated_shell_sort-gcc-O2` actually has. Both hold, so the front end is
not where `lab_0x112c` is lost; the next place to look is the optimizer
pipeline that runs after it, which this check can now drive end to end.

## Three fields that could only ever hold their default

A sweep of every scalar field declared under `include/retdec` for one that
nothing anywhere assigns turned up 357 candidates, most of them options a
caller sets and a few of them parse artefacts of the sweep itself. Three
survived reading:

* **`ReplacementNode::countReg`** was read and never written.
  `debugStr()` renders a `Memset`/`Memcpy`/`Memmove` with a negative
  `countImm` as `memset(r3, 0, r0)` — where the `r0` is this field, always
  zero. The only matcher that produces these nodes recognises unrolled vector
  copies, whose length is the number of stores times their width and is
  therefore always constant, so `countImm` is always set and the register
  half of the pair never carried anything. An unknown length now prints as
  unknown, and the file comment that promised "countImm or countReg" — and a
  `fillReg` that does not exist at all — says what the matcher actually
  emits.

* **`WatEmitter::DisState::paramCount`** was written once, from
  `mod.types[typeIdx].params.size()`, under a comment reading "Count params",
  and read by nothing. `localId()` resolves a local by index against the name
  section and does not need it.

* **`WatEmitter::DisState::labelCounter`** was neither written nor read.

All three are gone. The sweep is not a gate: at 357 candidates against 2,755
fields the accepted list would be larger than the signal, and the ones that
matter are found by asking whether a field is *read* — which is what OPT-01
already does for options.

## Three merges that drop a body drop the labels inside it

`IfStructureOptimizer::tryOptimization4` and patterns 6 and 7 of
`if_structure_optimizer_ext.cpp` all merge two `if` statements with
*identical* bodies into one `if (A || B)`. Identical means
`Statement::areEqualStatements` — equal by value, two distinct objects — so
one of the two bodies is kept and the other is dropped on the floor, along
with anything nested inside it.

`Statement::clone()` carries no label and neither does this: a label on a
statement inside the discarded body goes with it, while the goto elsewhere in
the function that names it stays. `goto lab_x;` with no `lab_x:` anywhere,
which is the same symptom as `GotoCFGOptimizer` pattern A and the same shape
of cause.

The remedy is the one already applied in the same codebase:
`DeadCodeOptimizer` asks `GotoTargetAnalysis::hasGotoTargets(body)` before it
discards a clause body, six times, and `unreachable_code_in_cfg_remover.cpp`
asks it once. These three now ask it too, each about whichever of the two
bodies it is the one to drop — pattern 7 drops the leading `if`'s body rather
than the second's, so its guard is on the other half.

Each verified by reverting that guard alone. Whether any of the three is what
CC-01 reports on `generated_shell_sort-gcc-O2` is **not** established: three
rounds of reading pass sources have produced three real defects with real
fixes and the same error at the same line 120 each time. The next round reads
the emitted C itself, which `--save-failures` now keeps.

## A workflow that does not parse does not fail — it does not run

`workflow_dispatch` on `ctest-linux.yml` came back with

    failed to parse workflow: (Line: 285, Col: 11):
    'if-no-files-found' is already defined

The file was already pushed. Nothing in this repository read these files, and
there are twenty of them. What an unparseable one costs depends on the
trigger, and the correction below records what was actually observed rather
than what the commit message assumed.

The part worth recording is *how* it got past a check. The file was validated
before the push with `yaml.safe_load()`, which accepts a repeated key and
keeps the last one. It loaded cleanly here and was rejected there. A checker
more permissive than the thing it stands in for is not a checker.

`scripts/ci/check_workflow_yaml.py` (WF-01) parses every workflow with a
loader that refuses duplicate keys at any level, and checks the structure
GitHub requires: a name, a trigger, at least one job, `runs-on` and steps on
every job that is not a reusable-workflow call, and exactly one of `uses` or
`run` on every step. Eleven self-test cases, including the exact shape that
got through — with an assertion in the test that `safe_load` does accept it,
so the reason this check exists cannot quietly stop being true.

Verified against the broken file itself: on the commit as pushed it fails and
names line 285, the same line GitHub named.

## Every condition reached the C writer as a subtraction

`src/retdec/llvm_to_ssa.cpp` mapped both `icmp` and `fcmp` to
`ssa::IrInstr::Op::Compare` and kept nothing else. LLVM names the question a
comparison asks — `slt`, `eq`, `uge` — and the adapter dropped the name.

Downstream, `ExprCoalescer` had nothing to work from, so `Op::Compare` became
`lhs - rhs`: the value a machine `CMP` conceptually computes. Nothing else in
`src/codegen/` constructs a comparison `BinOpKind` either, so **no expression
the C back end ever built contained `<`, `<=`, `>`, `>=`, `==` or `!=`.**
`if (a < b)` came out as `if (a - b)`, and a condition that reached the writer
through a flag read came out as a bare `v37`.

That also made `CondNormaliser` — a 165-line file with nine unit tests, whose
entire job is tidying comparisons — unable to match anything on the emitter
path. Its tests passed because they hand it comparisons directly. Nothing in
the pipeline ever did.

`ssa::CmpPred` now rides on `Op::Compare`, the adapter sets it from the LLVM
predicate, and the coalescer emits the matching C operator. Unsigned
predicates get their operands cast to the unsigned type of the operand width,
because C puts the signedness in the operands and not in the operator, so
`icmp ult` is `(uint32_t)a < (uint32_t)b` and never a signed `a < b` on the
same bits.

Two cases deliberately stay as they were:

* **Unordered float predicates**, and `ORD`/`UNO`. No C comparison operator
  asks whether an operand is NaN, and approximating one with an operator that
  differs exactly there is worse than the subtraction.
* **A machine `CMP`/`TEST`**, which asks nothing on its own: the condition is
  in the branch that reads the flags. `IrInstr::specificFlag` records *which*
  flag a `Jcc` reads and nothing records the polarity, so `je` and `jne` are
  indistinguishable in the IR. Recovering those needs a field the lifter would
  set, and the lifter is in the front end this environment cannot build. Until
  then a binary lifted straight to the SSA IR keeps `a - b`; one that goes
  through LLVM gets the operator.

### The counters beside it, and the knobs above it

`CodeGenPass::Stats::condRewrites` and `::castsRemoved` are documented as
counts of what those two passes did and were assigned by nothing, so they read
`0` on every unit — the same shape as `DVSA::Result::carvedAccesses`. Both are
now threaded through and counted, and `condRewrites` is non-zero end to end
now that there are comparisons to rewrite.

`Config::enableCondNorm` and `Config::enablePtrSyntax` sat beside
`enableGotoElim` and `enableCoalescing`, which are both consulted, and were
read by nothing: the two passes ran whatever the config said. They are
consulted now, and OPT-01's accepted list of inert knobs is down from 69 to
67.

**Still open**: `castsRemoved` reads 0 through the pipeline even so.
`PointerSyntax::recover()` is called on exactly one expression — the
destination of a `Store` — and nothing in `src/codegen/` builds a `Cast` node
that reaches it. `minimiseCasts()` is correct and unreached. Closing it means
running pointer-syntax recovery over expressions generally, which is a
behaviour change to every emitted statement and wants its own measurement.

## Two invariants in `Statement` that nothing was holding

Neither of these had a corpus symptom to point at, which is why they are
recorded here rather than claimed as a fix for one. Both are wrong answers
from a public API, reproduced at that level.

**`setSuccessor()` never took the replaced edge off the record.**

```cpp
if (succ) {
    // Update the predecessors of the old successor.
    succ->preds.erase(succ);
}
```

The comment says what was meant; the line erases the old successor from its
*own* predecessor set, a no-op for any statement that is not its own
predecessor. So after `a->setSuccessor(b); a->setSuccessor(c);`, `b` still
records `a` as reaching it, and nothing cleans it up later:
`removePredecessors(true)` keeps a predecessor whose successor is no longer
this statement, which is exactly the stale one.

Most readers of `preds` are guarded — `isGotoTarget()`, `redirectGotosTo()`
and `removeStatement()` all re-check `getTarget() == this` — but
`getUniquePredecessor()` is not, and `pre_while_true_loop_conv_optimizer`
walks backwards through it and rewrites around the answer. A statement that
does not flow into another is not its unique predecessor. Three tests.

**`removeStatement()` sent a goto's label and its jumps to two different
places.** A goto never falls through, so a jump into `X: goto L;` means L.
The function agreed where the label was concerned — it moves a goto's label
onto the goto's target — and then sent the inbound gotos to the goto's
*successor*, the code the jump existed to skip, and copied the label there as
well. Two statements carrying one label, and the jumps resolving through
neither of them reliably.

It is latent today: every caller that removes a goto pre-empts it.
`GotoStmtOptimizer` prepends the replacement first, and `prependStatement()`
ends in `notifyObservers(stmt)`, which is how an inbound goto gets redirected
onto the prepended statement — so by the time `removeStatement()` runs there
is nothing left in `preds` for it to get wrong. `GotoCFGOptimizer` pattern B
removes a goto only when it is not a goto target; pattern D transfers the
label and redirects the gotos first. An end-to-end test through
`GotoStmtOptimizer` was written and then dropped, because it passed with the
fix reverted: the prepend had already done the redirect. Four tests at the
API level instead, one of them the control for an ordinary statement, which
still falls through to its successor.

## Corrections to claims made in this branch's commit messages

### "This was the last entry on the audit's list" — it was not

48145da's message says the `cli_sig.cpp` traversal "was the last entry on the
audit's 'behind a build this environment cannot run' list", and counts the list
as six with two real. It was seven, two of them still open when that was
written: `getXByte`'s offset sum and the `computeSectionTableHashes` /
`getOverlayEntropy` pair. Both are resolved in the commit that adds this note,
and the count that holds is: seven entries, three real — the
`getRealSizeInRegion` clamp, the loader clamps, and the CIL traversal — and
four bounded by something the entry had not looked for.

The mistake is the one this section keeps catching: a claim about a *set*,
written from the entry in front of me rather than from the list.


### The codegen goto fixes are not on the shipped path

`d4d9c9e` and `782117d` fix real defects in `src/codegen` — every `goto` it
emitted named a label nothing wrote, and `GotoEliminator` consequently had
nothing to eliminate against. Both messages describe the consequence as an
uncompilable translation unit, which is what the code would produce.

It produces nothing today. `CodeGenPass` has no caller outside
`tests/codegen`, and `retdec-decompiler` links `llvmir2hll` instead — see
the structural entry above. So neither fix changes any output the corpus
measures, and neither is a candidate explanation for CC-01's
`label 'lab_0x112c' used but not defined`, which comes from the `llvmir2hll`
path.

### `getType()` is not a portable spelling of `getPointerType()`

`ce86f3d` replaced `cNullPtr->getPointerType()` with `cNullPtr->getType()` and
said `ConstantPointerNull` "specialises `getType()` to return a
`PointerType*` in every LLVM this code has ever been built against". That was
checked against the system LLVM and asserted of the pinned one, which is the
same mistake as the exclusion the commit was undoing.

Run 270 failed to build:

```
llvm_constant_converter.cpp:236: error: cannot convert
'shared_ptr<Type>' to 'shared_ptr<PointerType>'
```

`typeConverter->convert()` is overloaded on `Type*` and `PointerType*`. The
system LLVM does specialise `getType()` and picked the `PointerType*`
overload; the pinned one does not, picked the `Type*` overload, and
`ConstNullPointer::create()` would not take the result. The call compiled
cleanly here and broke there.

`llvm::cast<llvm::PointerType>(cNullPtr->getType())` names the type at the
call site, so the overload chosen is the same whichever LLVM is in hand. That
is sound by construction rather than by which header happens to be on the
include path — which is what the original line was not, in either direction.

### "A workflow that does not parse does not run" — half right

`9e54dd8`'s message says an unparseable workflow "produces no run at all, so
there is no red X anywhere". That is what `workflow_dispatch` does — the API
call is rejected with the parse error and no run is created — and it is what
the message generalised from.

The push trigger behaves differently, and run 268 is the evidence: the push
of `e69d0b0` created a run that failed immediately, `created_at`,
`run_started_at` and `updated_at` all equal, listed under the workflow's
*path* rather than its name because GitHub had no name to read. So a push
does leave a red X.

What does not change is the reason WF-01 exists: nothing in this repository
looked at these files before the push, and the YAML check that ran did not
reject a duplicate key. Whether the cost is a failed run or no run at all,
the file should not have reached the branch.


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

### `DerefOpExpr::getType()` returns the pointer's type, not the pointee's

`src/llvmir2hll/ir/unary_op_expr.cpp:27`

```cpp
ShPtr<Type> UnaryOpExpr::getType() const {
	return op->getType();
}
```

`DerefOpExpr` does not override it. So `*p` reports the type of `p`: ask
whether the operands of `*p | 8` are pointers and the answer is yes for every
well-typed expression of that shape in the tree.

Found by writing a scan for the `invalid operands to binary |` defect below and
watching it report a function whose emitted C was correct. The scan unwraps one
dereference itself; the IR was left alone.

Changing it is not a small change. Every consumer of a dereference's type in
the back end — the C writer's cast decisions, `ExprTypesAnalysis`, the
optimizers that compare operand types before rewriting — currently reads the
pointer type and has been tuned against that. A one-line fix here would move
all of them at once with no measurement behind it, and this branch has a rule
about that. It is written down so the next person to be surprised by a type
query finds this instead of re-deriving it.

The bar for doing it: a run with `DerefOpExpr::getType()` returning the
contained type, F1 and DET-01 unchanged, and CC-01 no worse than the floor it
had.

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

## A `#include` for a header that no longer exists

**Closed.** `GCCGeneralSemantics` assigned `<stropts.h>` to `ioctl` and seven
others, and `<libio.h>` to three. Assigning a header is not a note: it is what
makes `CHLLWriter` emit an `#include` for it --
`HeadersForDeclaredFuncs::getHeaders` collects one per declared function and
`c_hll_writer.cpp:376` writes them out. Neither header exists on a current
glibc. `libio.h` became internal in 2.28 and `stropts.h` went with STREAMS in
2.30, so any binary that declared one of those eleven produced C that failed at
its own first line.

`ioctl` is the one that matters: it is ordinary, and it is declared in
`<sys/ioctl.h>`, which is where it is assigned now. Of the libio three,
`__overflow` is still declared by `<stdio.h>` and moves there; `__uflow` and
`__underflow` are declared nowhere includable, so they get no header at all and
are emitted as declarations instead, which compiles. The other seven STREAMS
names get none for the same reason.

Moving `ioctl` also gave it an arity, which it could not have had before: the
arity table is measured against whatever header the semantics assigns, and
nothing is measurable from a header that cannot be included. It is
`(2, variadic)` now, matching `int ioctl(int, unsigned long, ...)`, so an
under-supplied `ioctl` call gets the cast the writer exists to add.

**How it was found, and why not earlier.** Not by reading the header map --
by ARITY-01's new header-availability probe, added for an unrelated
portability defect, which reported `<libio.h>` and `<stropts.h>` as assigned
but unavailable. CC-01 compiles all 216 corpus outputs and has been at 216/216;
it never saw this because no corpus binary calls any of the eleven. That is the
limit of a corpus gate stated plainly: it can only find defects the corpus
provokes, and `ioctl` is common in the world and absent from these 216.

**Gated twice.** Four tests in
`tests/llvmir2hll/semantics/semantics/gcc_general_semantics_tests.cpp` assert
the assignments directly; reverting the header map fails three of them. The
fourth reads the arity table, so its guard is ARITY-01 instead: with the header
map reverted the probe fails, naming both headers.

### The case that would still have been invisible, and is not any more

Fixing `ioctl` left the more uncomfortable question standing. It was caught
because `<stropts.h>` does not exist; a name assigned a header that *does*
exist but does not declare it fails in exactly the same way and leaves no
trace. ARITY-01 considers only two causes when a name will not compile under
its header -- a function-like macro, or not on this platform -- and records
either as "left out". A wrong-but-real header is silently the third, and the
emitted C gets an `#include` that does not declare the function it is for.

So it was measured. `--audit-assignments` takes every name the measurement
drops, excludes the ones that really are function-like macros under their own
header, and tries the rest under the union of every header the semantics
assigns. Anything that then compiles is misassigned.

**1968 names across both modules, 0 misassigned.** A clean result, which is
only worth the words if the check could have said otherwise: moving `strlen`
out of `<string.h>` makes it report `strlen`. It is not part of `--check` --
it is a few thousand extra compiles and the header map changes rarely -- so it
is a flag to run when that file changes.

That bounds the concern rather than closing it by assertion: the 555 names both
tables leave out are left out because they are macros or are genuinely not on
this platform, and not because the map sent them to the wrong header.
