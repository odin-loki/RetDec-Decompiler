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

### The CMake half is still open; "compiled by nothing" is not

**Partly closed by OCL-01.** The reason above is about `find_package`, and the
sources do not need CMake to be compiled: they include `<CL/cl.h>`, the
standard library and their own headers, and no LLVM. Checking that was one
command, and the entry above stood for as long as nobody ran it.

`scripts/ci/check_opencl_tests.sh` compiles all 19 translation units, builds
every kernel, and runs all eight suites. It is in CI and in the push gates.
What that found, none of which any check could have seen before:

  * **`src/opencl` did not compile.** `ocl_type_inferencer.cpp:297` does
    `ptr[sl] |= (opPtr[i] != 0)` on a `std::vector<bool>`, whose `reference`
    proxy has no `operator|=`. Four lines up, `cpu_dsu::unite` spells the same
    merge as `ptr[ra] || ptr[rb]`.

  * **Three of the five kernels did not build**, and so did three of the five
    *embedded copies* — `schar` is not an OpenCL C type, `&(uint)expr` is not
    an lvalue, and `atomic_inc`'s cast to `atomic_uint*` made the call
    ambiguous. There are two copies of every kernel: `kernels/*.cl`, and a C++
    raw-string literal in `src/opencl/*_sources.cpp`. **The runtime compiles
    the embedded copy**, so a check that read only the `.cl` files would have
    checked the copy that never runs. They are not the same text — the
    embedded `semantic_hash` is 832 lines against the `.cl` file's 952 — and
    the comment claiming they are "embedded verbatim" is wrong.

  * **The BITFIELD rewrite could only ever fire for a shift of zero.** Rule 5
    turns `(x >> k) & mask` into a bitfield extract, and checked contiguity of
    `mask >> k`. The mask is applied *after* the shift, so it already sits at
    bit 0; shifting it again by `k` zeroed every mask narrower than the shift.
    Present identically in the CPU path and both kernel copies.

  * **The CPU emulator decoded almost nothing.** `host_emu::emulate` handled
    NOP, RET, jumps, push/pop and `MOV reg, imm`. Every other opcode advanced
    the instruction pointer by one byte and carried on — which resumes
    decoding inside the next instruction, so every byte after the first
    unknown one is read as an opcode. `xor eax,eax` and `mov rax,42` were both
    unhandled, which is why two functions with different behaviour hashed
    identically. It has a ModRM core now (the ALU groups in both directions,
    `TEST`, the `MOV` forms, `MOVZX`/`MOVSX`), and an opcode it does not model
    stops the run and reports `Unsupported` instead of producing a hash from a
    desynchronised stream.

**Why none of this showed up as a failure.** `OCLContext` falls back to the CPU
path when a kernel fails to build, and the GPU tests assert only that they get
the right number of non-zero hashes. So they passed whether the kernel compiled
or not — and they did. That is why OCL-01 compiles the kernels itself rather
than trusting the suite, and why it requires zero skips when a device is
present.

**What is still open.** The CPU fallback and the kernel are two implementations
of one emulator, and they are not at parity: the kernel has a real
instruction-length decoder, memory operands, `CALL` with a depth limit, string
operations and the flag instructions; the CPU has none of those. No test
compares the two against each other, so a divergence in the shapes both handle
would not be caught. The CMake wiring above is also still open — OCL-01 builds
these sources, `cmake` still does not.












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

The full corpus reported rather than gated at first, so that the floor would
be a measurement naming the run it came from, exactly as the 24-binary slice
was introduced. That number now exists: run 276 measured 216/216, and the
step carries `--min-rate 1.0000`. The corpus has since grown to 252 — six
floating-point sources joined it, and until they did not one of its 367
sources contained a `float` or a `double`, so every "emitted C compiles"
figure before that was 216 integer programs. The floor is a rate, so the
count moving does not touch it; what changed is that the rate now covers
floating point at all.

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


## Four architectures nothing had ever asked a question about

**Closed by C2L-01.** Asked to bring every CPU architecture to parity with
x86-64, the first thing to establish is what the gap actually is. It is not
what it looks like from the instruction tables:

| arch    | translated | listed | tests |
|---------|-----------:|-------:|------:|
| x86     |        331 |   1343 |  1965 |
| arm     |        175 |    435 |   534 |
| arm64   |        189 |    461 |   457 |
| mips    |        142 |    627 |   598 |
| powerpc |        283 |   1192 |   808 |

("listed" counts the entries in each translator's `_i2fm`; the difference is
entries mapped to `nullptr`, which fall through to a pseudo-call. x86 is at
24.6% by that measure, the *lowest* of the five, because it lists every SSE and
AVX form. A ratio over an ISA's own instruction count does not compare across
ISAs.)

The gap is not coverage. It is that **every end-to-end gate in this repository
measures exactly one architecture**: all 216 binaries in
`tests/algorithm_recovery/corpus` are x86-64, so CC-01, DET-01 and the F1
recovery gate — the three numbers this branch publishes — say nothing at all
about ARM, ARM64, MIPS or PowerPC.

And the one place their semantics *are* checked, `tests/capstone2llvmir`, was
on `UNGATED_SUITES`: 46,066 lines and 4,370 assertions evaluated nowhere. The
recorded reason was

> same LLVM 20+ drift (4-arg APInt, Intrinsic::getOrInsertDeclaration), and its
> tests need `<keystone/keystone.h>`, which deps/keystone is a download stub for

Both halves are facts about the machine rather than about the code.
`llvm-20-dev` is in Ubuntu noble's own universe pocket, and Capstone 5.0.9 and
Keystone 0.9.2 — the exact revisions `cmake/deps.cmake` pins — build from
source in about three minutes between them. With those present all 17
translator sources and all six test files compile **unchanged**, and all 4,370
tests pass. The suite was never broken. Nothing had run it.

The system Capstone is not enough, and that is where the "drift" reading came
from: it is 4.0.2, and the x86 translator names `X86_REG_BND0..3`,
`X86_INS_FUCOMPI` and `X86_INS_FCOMPI`, which arrived in 5.x. Six errors, and
reading them as drift rather than as the wrong Capstone is what kept the entry
standing.

**The floors are per architecture** — `MIN_X86`, `MIN_ARM`, `MIN_ARM64`,
`MIN_MIPS`, `MIN_POWERPC` — because a single total lets 1,965 x86 cases hide
the disappearance of all 598 MIPS ones while the aggregate barely moves.
Falsified by deleting `mips_tests.cpp`: `FAIL Mips: 0 test(s), floor is 598`,
with the other four still reported.

`tests/llvmir-emul` rides along, off the same list for the same reason; its
library is already linked because the translator tests execute the IR they
produce. 4,380 tests.

**What this does not close.** Parity of *coverage* is untouched: MIPS still
translates 142 instructions and PowerPC 283, and whether those are the ones
real binaries use is not measured here. Nor is there a non-x86 binary anywhere
in the corpus, so end-to-end behaviour on the other four architectures remains
unmeasured. What changed is that all five now have their instruction semantics
checked on every push, which is the standing x86 already had and the other four
never did.


## 0/40: what the other four architectures actually did

**Two root causes fixed; the rate itself comes from CI.** ARCH-01's first run,
ctest-linux 285 at 69624d4, is the number the previous entry could not supply:

```
ARCH-01: arm        0/10   0.0000
ARCH-01: arm64      0/10   0.0000
ARCH-01: mips       0/10   0.0000
ARCH-01: powerpc    0/10   0.0000
ARCH-01: overall    0/40   0.0000
```

Zero, on every architecture, in the same run where CC-01 reports 216/216 for
x86-64. Not "worse than x86-64" — nothing at all. Both causes are single
defects, and both are invisible to a corpus that is entirely 64-bit x86.

### Thirty of forty: a constant fold that cannot represent a negative number

arm, mips and powerpc all dumped core in the decoding phase, at the same
assertion:

```
APInt.h:127: llvm::APInt::APInt(unsigned, uint64_t, bool, bool):
  Assertion `llvm::isUIntN(BitWidth, val) && "Value is not an N-bit unsigned value"' failed.
  #13 SymbolicTree::_simplifyNode()  symbolic_tree.cpp:418
  #22 Decoder::getJumpTarget()       decoder.cpp:926
```

`_simplifyNode` folds constant arithmetic with

```cpp
value = ConstantInt::get(c1->getType(), c1->getSExtValue() + c2->getSExtValue());
```

`getSExtValue()` widens to `int64_t`, and `ConstantInt::get`'s `uint64_t`
overload asserts that the value fits the type as an **unsigned** N-bit number.
Add two negative 32-bit constants and the sum is a negative `int64_t`, which as
a `uint64_t` is enormous and does not fit `i32`.

**On a 64-bit target every `int64_t` fits `i64`, so the assertion can never
fire.** That is the whole reason 216 x86-64 binaries never found it and all
thirty 32-bit ones die on it — and jump-target simplification is on the path
of every binary, so it is not an edge case on those targets, it is all of them.

Six folds (add, sub, or, and, the nested add, and the global-address add) use
`APInt` arithmetic now, which is modular at the operand width — what the
machine does, and what cannot assert. The global-address fold also masks the
image address to the pointer width before widening, because there the two
sides genuinely differ.

### Ten of forty: one missing vector arrangement

Every ARM64 binary failed with

```
[capstone2llvmir]: Arm64: extractVectorValue(): Unknown VESS type
```

`extractVectorValue` switches on the vector arrangement specifier and throws on
anything it does not name. Nothing catches that throw, so it ends the
decompilation of the **whole binary** rather than of one instruction. Of the
fifteen `ARM64_VAS_*` capstone 5.0.9 defines, fourteen were named. The missing
one was `ARM64_VAS_2D` — two 64-bit lanes, which is what every double-precision
SIMD operand looks like and which glibc's string and math routines are full of.

It does **not** share the `1D` path, and the first attempt at this did, which
was wrong: `1D` ends in a bitcast to `double`, so routing `2D` through it made
`add v0.2d, ...` an integer add on a double and every ARM64 binary failed with
*"Tried to create an integer operation on a non-integer type"* instead. A `.2d`
lane is 64 bits and the arrangement alone does not say whether the instruction
reads it as an integer or a double, so it returns the integer lane — the choice
`VAS_INVALID` already makes for `vN.d[i]`.

A C enum switched on with a `default` gets no `-Wswitch` help, so C2L-01 now
checks the exhaustiveness textually: every `ARM64_VAS_*` in capstone's header
must appear in `arm64.cpp`. Falsified by deleting the new case.

### Instruction coverage, and why the table ratio is the wrong number

`scripts/ci/check_instruction_coverage.py` (COV-01) disassembles the corpus and
asks what fraction of the instructions **actually present** each translator
implements:

| arch    | decoded   | skipped | rate   |
|---------|----------:|--------:|-------:|
| arm     | 1,571,329 | 474,577 | 0.8981 |
| arm64   | 3,085,112 |      36 | 0.9826 |
| mips    | 3,683,664 |      72 | 0.9952 |
| powerpc | 3,981,728 |   8,212 | 0.9942 |

This inverts the picture the dispatch tables give. By table ratio MIPS looks
worst at 22.6%; weighted by what binaries contain it is the **best** of the
four, at 99.52%. x86 is the lowest of all five by table ratio (24.6%) because
it lists every SSE and AVX form it declines. A ratio over an ISA's own
instruction count is not comparable across ISAs and should not be used to
decide anything.

`skipped` is the honesty column. ARM's 474,577 is static glibc interleaving ARM
and Thumb while this disassembles in one mode: the Thumb regions decode as
garbage, much of it as coprocessor instructions that are not in the binary at
all — which is why ARM's top-uncovered list is led by `STC`, `LDC` and `CDP`,
and why **the ARM figure is a lower bound with a wide error bar and its
uncovered list must not be used to choose what to implement.**

### What the arm64 measurement did justify

Two families, both confirmed present in real binaries before any code changed:

  * The six bitfield-move aliases — `UBFX`, `UBFIZ`, `SBFX`, `SBFIZ`, `BFI`,
    `BFXIL`, 7,526 occurrences — were all `nullptr`. Capstone reports these
    rather than the `UBFM`/`SBFM`/`BFM` they alias (zero occurrences of those),
    so the aliases are what reaches real code. Each is a shift and a mask.

  * `BTI`, `PACIASP`, `AUTIASP` and the rest of the ARMv8.3/8.5 control-flow
    integrity set — 9,829 occurrences — had **no entry at all**, not even
    `nullptr`, because the table predates both extensions. Current toolchains
    emit them unconditionally, so almost every function prologue carried a
    pseudo-call. They model as nothing: BTI is a landing pad, and PACIASP/
    AUTIASP are a sign/authenticate pair whose composition is the identity on
    X30.

arm64 coverage moved 0.9770 → 0.9826 on the same corpus, which is the only
reason to believe the two families were worth the change. Nine tests, each
failing when the dispatch entries are reverted; keystone 0.9.2 predates ARMv8.3
and cannot assemble those mnemonics, so those three go in as encodings checked
against capstone.


### One fix is not one site: the same assertion, four places

Fixing `symbolic_tree.cpp:418` did not move ARCH-01 off 0/40. It moved the
crash. Run 286, at `fcd4895`, still lost all forty — and the failure logs show
the same `isUIntN` assertion firing from three *different* places, one per
architecture, each further down the pipeline than the last:

| arch    | site                                        | reached from |
|---------|---------------------------------------------|--------------|
| mips    | `symbolic_tree.cpp:399`                     | StackAnalysis |
| powerpc | `strength_reduction.cpp:146`                | strength reduction |
| arm     | `fileimage.cpp:143`                         | image reads |

They are all one class — `ConstantInt::get(Type*, uint64_t)` asserts rather
than truncating when the value does not fit the type as an unsigned N-bit
number — and all four instances are shapes that only a 32-bit target reaches:

  * `~((1ULL << n) - 1)` is every high bit set **in sixty-four bits**, so it
    fits no narrower type. (Also undefined for `n >= 64`.)
  * an image address, or a function entry address, written into whatever type
    the load had.
  * `ci->getZExtValue() - 1` where `ci` is zero: `0 - 1` as a `uint64_t` is
    every bit set. Not yet observed, found by looking for the shape.

A sweep of all 457 `ConstantInt::get` call sites in `src/` narrowed to 50 whose
value is an address, a mask, or a shift expression; 32 of those are in the x86
translator, where the constant is built at the operand's own width and which
216 binaries of evidence say is fine. Of the remaining 18, four were wrong and
are fixed; the rest write an address into a default type that is wide enough
for it.

**The lesson recorded rather than the fix.** Each of these was hidden behind
the one before it. A single ARCH-01 number cannot distinguish "one defect" from
"four in a row", and three CI round-trips at forty-five minutes each is what
finding them one at a time costs. The sweep is the answer to that, not another
round trip.

### ARCH-01's 0/40 was two different failures, and one of them was the corpus

Run 287, at `784324e`, still read 0/40 — but the forty logs no longer say the
same thing, and that is the finding.

**Thirty of them are no longer crashes.** ARM, MIPS and PowerPC now run the
entire pipeline. `binary_search-mips-gcc-O0` reaches *Disassembly generation*
at 57.88 s and is killed by ARCH-01's 60-second timeout; PowerPC and ARM get
to *Simple types recovery* at 57.65 s and 59.72 s. The four `ConstantInt::get`
fixes did what they were supposed to do. What stops those thirty is wall clock.

**And the wall clock is my own doing.** `build_multiarch_corpus.sh` passed
`-static`, on this reasoning, written in its own header:

> a dynamically linked cross binary leaves the algorithm in a PLT stub and the
> decompiler with almost nothing to recover

That is wrong, and one command falsifies it: `aarch64-linux-gnu-nm` on a
dynamically linked cross build finds `bubble_sort` and `main` in the binary.
Only the libc calls go through the PLT — exactly as in
`tests/algorithm_recovery/corpus`, the x86-64 corpus the F1 gate scores every
run and which is built with no `-static` at all. What `-static` added was the
whole of glibc: 400–700 KB per binary against 8–70 KB.

So ARCH-01 was not measuring what it claimed. Its question is whether the other
four architectures reach x86-64's result, and it was handing them a
categorically harder input than the x86-64 gates get. The builder now defaults
to the same flags as the x86-64 corpus (`-O0 -fno-inline -fno-builtin`,
dynamic), and `--link static` keeps the harder set for deliberate use — it is
worth keeping, because glibc's hand-written SIMD is what surfaced every bug in
this section.

The claim is now checked rather than argued: the builder fails if `main` is not
a defined text symbol in every binary it produces.

What that changes, measured (COV-01 on the dynamic corpus, against the same
tool's numbers for the static one):

| arch    | static          | dynamic |
|---------|-----------------|---------|
| arm     | 0.8981          | 0.9581  |
| arm64   | 0.9826          | 0.9998  |
| mips    | 0.9952          | 1.0000  |
| powerpc | 0.9942 → 0.9978 | 1.0000  |

(PowerPC's static figure is before → after the load/store work below. Nothing
in this batch changes any dispatch table for the other three, so their two
columns differ only by corpus. Both ARM figures are lower bounds with a wide
error bar — static glibc interleaves ARM and Thumb, COV-01 disassembles in one
mode, and the skipped-byte column says so: 474,577 static, 426 dynamic.)

Instruction coverage on binaries comparable to the x86-64 corpus is already
essentially complete for three of the four. Whatever ARCH-01 finds next, it is
not mostly a coverage problem.

### ARM64: an integer add on a float, in all ten binaries

The tenth of ARCH-01's forty is a different failure, and a real translator bug.
All ten ARM64 binaries die in the same place:

```
Assertion `getType()->isIntOrIntVectorTy() && "Tried to create an integer
operation on a non-integer type!"' failed.
  ... Capstone2LlvmIrTranslatorArm64_impl::translateAdd (arm64.cpp:1255)
```

`add v0.4s, v1.4s, v2.4s` is four 32-bit adds. Two things have to be wrong for
it to reach `CreateAdd`:

1. `extractVectorValue` extracts a lane even when there is no lane to extract.
   A whole-register operand carries an arrangement and `vector_index == -1`;
   every branch of that switch multiplies the index by a lane width and shifts
   by the result, so -1 became a shift by 2^128−32 — poison — then a truncation
   of the poison to a lane type. For the S arrangements that type is `float`.
2. `translateAdd` bitcasts FP operands back to integers only when
   `isFPRegister(operands[0])`, and that predicate knows Q, D, H and S
   registers but not V. So the bitcast never fired.

Both are fixed. A negative index now returns the register whole, which is also
exactly right for the bitwise vector instructions (`eor`/`and`/`orr`/`mov` on
`.16b` are lane-agnostic), and `translateAdd`/`translateSub` take the
`ifVectorGeneratePseudo` guard that five other translators in the same file
already use — a lanewise add is not a 128-bit add, and saying "not modelled" is
better than emitting the wrong arithmetic.

**Reproduced without an assertions build.** The distribution LLVM is compiled
with `NDEBUG`, so the assertion cannot fire here. The regression tests do not
depend on it: they read the emitted IR and fail on an integer opcode carrying a
floating-point type, and on a shift by a constant wider than 64 bits. Reverting
either fix makes them fail with exactly the defect the CI log named.

### PowerPC had no floating point at all

Every one of the 44 `PPC_INS_F*` entries in the dispatch table was `nullptr`,
and so were all sixteen float load/store forms — while `powerpc_init.cpp` maps
`PPC_REG_F0..F31` to `double`. The register file was modelled and nothing ever
wrote to it. COV-01 put `STFD` and `LFD` at the top of PowerPC's untranslated
list, 0.37% of every instruction in the corpus between them.

This is not a coverage statistic. A float load that becomes
`call @__asm_lfd(...)` severs the dataflow the rest of the decompiler runs on,
and the store form was worse: capstone marks the memory operand as read, so the
generic pseudo-asm fallback emitted a *load* from the address a `stfd` stores
to.

Fixed for the loads and stores — `LFS LFSU LFSX LFSUX LFD LFDU LFDX LFDUX` and
the eight `STF` forms — and for the 64-bit integer forms, which were equally
absent and are every pointer load in a ppc64 binary: `LD LDU LDX LDUX STD STDU
STDX STDUX LWA LWAX LWAUX`. PowerPC's corpus coverage goes 0.9942 → 0.9978
(1.0000 on the dynamic corpus), and its suite 808 → 834 tests.

The arithmetic — `FADD FSUB FMUL FDIV FMR FABS FNEG FCMPU`, the `FMADD`
family, the conversions — is still `nullptr`. It is the next piece of work, not
a thing this section claims to have done.

**Not doing, and why it would be cheating.** `SC`, `MFFS`, `TRAP`,
`DCBST`/`ICBI`/`DCBZ`, MIPS `RDHWR` (0.45% of MIPS, the single largest
untranslated entry it has), ARM64 `MRS` and `SVC` all already reach
`translatePseudoAsmGeneric`, which reads capstone's per-operand access flags
and emits exactly the call an explicit pseudo-asm wiring would. Pointing their
table entries at a helper would move roughly 1% of MIPS and 0.5% of ARM64 out
of COV-01's untranslated column without changing one instruction of output.
That is moving the metric, not the product.

### 32-bit ARM had no floating point either, and one entry was unreachable

The last of the five. All 149 `ARM_INS_V*` entries were `nullptr`, so every
floating-point instruction on 32-bit ARM was an opaque `__asm_*` call -- while
`arm_init.cpp` models 32 S registers as f32 and 32 D as f64. A six-line dot
product built for `arm-linux-gnueabihf` emits `vldr`, `vstr`, `vmul.f64`,
`vadd.f64` and `vmov.f64` in one function, none of which reached the rest of
the decompiler.

Scalar VFP is now translated: VLDR VSTR VADD VSUB VMUL VDIV VNMUL VNEG VABS
VSQRT VMLA VMLS VNMLA VNMLS VFMA VFMS VFNMA VFNMS VCMP VCMPE VCVT VCVTR VMOV
VMRS. NEON is not, and says so: anything whose `cs_arm::vector_data` is an
integer type, or whose operands are Q registers or carry a lane index, goes to
`translatePseudoAsmGeneric` -- the answer arm64 already gives through
`ifVectorGeneratePseudo`. A lanewise add is not a scalar add.

**The operand shapes were measured, not assumed.** Encodings taken from a real
`arm-linux-gnueabihf-gcc -O0` build and run through the capstone revision
`cmake/deps.cmake` pins. Three would have been wrong:

  * `vcmp.f64 d7, #0` gives an **IMM 0**, not an FP operand, so the zero has to
    be built from the other operand's type.
  * `vmov d6, r0, r1` is low-word-first -- confirmed from the `ldrd r0, r1` and
    `__floatdidf` call that precede it in real output.
  * `vmrs`'s operands are both marked `access=0`, so the access flags cannot
    drive it.

**And one entry could never have fired.** Capstone 5.0.9 decodes
`vmrs APSR_nzcv, fpscr` as `ARM_INS_FMSTAT` (58), the pre-UAL alias, while
printing the mnemonic as "vmrs". `ARM_INS_VMRS` is 378 and is the general
`vmrs rN, fpscr` form. The ARM table had no key for 58 at all, so wiring
`ARM_INS_VMRS` looked exactly like a fix and did nothing -- which is what it
did, until the test said so.

This is the same shape as the duplicate `ARM64_INS_HINT` key: a dispatch entry
that is never reached is dead, and it reads as done. C2L-01 guards the
duplicate case textually; this one is only catchable by assembling the
instruction and asserting the translation, which is what the 40 new ARM tests
do. A count of what each table omits, for the record:

| arch    | table keys | capstone ids | absent |
|---------|-----------|--------------|--------|
| arm     | 437       | 471          | 36     |
| arm64   | 468       | 1288         | 821    |
| mips    | 627       | 625          | 0      |
| powerpc | 1192      | 1726         | 536    |
| x86     | 1343      | 1523         | 182    |

Most of those are unreachable-in-practice (arm64's are SVE and SME, which the
table predates) and all of them still reach the pseudo-asm fallback, so an
absent key is latent rather than broken. It becomes a defect the moment someone
wires the canonical name for an encoding capstone reports under an alias.

**Verified on the floating-point binaries**, which is the only evidence that
counts here: COV-01 over cross-built float programs reports powerpc **1.0000**,
mips 0.9916 (`MTHC1`), arm64 0.9846 (`LD1`/`ST1`). The 32-bit ARM figure cannot
be read the same way -- COV-01 disassembles in one mode and this corpus is
Thumb -- so ARM's evidence is the 40 tests, which run in both CS_MODE_ARM and
CS_MODE_THUMB.

### Six sources, so that the floating-point work is measured at all

The section below records that no corpus source contained a `float` or a
`double`. Six now do, and they were chosen so that between them they emit the
instructions the last three commits implemented:

| source | what it exercises |
|--------|-------------------|
| `float_dot_product` | double load, multiply, accumulate, return |
| `float_mean_variance` | division by a converted int, nested calls |
| `float_newton_sqrt` | an iterative loop, compare against a constant |
| `float_matrix_multiply` | `float` rather than `double`, 2-D indexing |
| `float_compare_sort` | floating-point comparison driving control flow |
| `float_int_conversion` | every direction of int/float conversion |

No libm: a call to `sqrt()` would land in a PLT stub and say nothing about a
translator, so Newton-Raphson is written out in multiplies and divides.

Across the six, PowerPC emits `lfd` (84), `stfd` (34), `fsub` (16), `fctiwz`
(16), `fmr` (13), `fcmpu` (11), `fmul` (10), `lfs` (8), `stfs` (6), `fneg`,
`fadd`, `frsp`, `fmadd`, `fdiv`, `fadds`, `fmuls`; 32-bit ARM emits `vldr`
(40), `vmov` (39), `vcvt` (21), `vstr` (15), `vmul`, `vadd`, `vmrs`, `vcmpe`,
`vneg`, `vdiv`, `vsub`, `vmls`. Every one of those was an opaque `__asm_*`
call at the start of this session.

COV-01 over the cross-built set: **powerpc 1.0000, mips 1.0000**, arm64 0.9958
(`LD1`/`ST1`, NEON, out of scope by design).

What this costs, and what it risks: the x86-64 corpus goes 216 -> 252 binaries
and the multiarch one 144 -> 168. CC-01's floor is a rate, so the count moving
does not touch it -- but its full run now covers floating-point programs for
the first time, and if their emitted C does not compile the gate goes red. That
would be the finding, not a regression.

ARCH-01's floored step takes `--limit 40`, the first ten programs by name, and
the float sources sort past that window. So a second step runs the whole 168
and reports without a floor -- the road CC-01 took from 24 binaries to 252, and
that ARCH-01 itself took from 0/40 to 40/40. A floor picked before the first
number is either vacuous or a fiction.

### Two instructions the IR-to-HLL converter aborts on

The whole-corpus ARCH-01 step existed to measure the 128 binaries the floored
one never sees. Its first run, ctest-linux 290 at `938f6be`, read **162/168**,
and the six failures are two decompiler crashes:

```
ARCH-01: arm       41/42   0.9762
ARCH-01: arm64     40/42   0.9524
ARCH-01: mips      41/42   0.9762
ARCH-01: powerpc   40/42   0.9524
ARCH-01: overall  162/168  0.9643
```

| instruction | failures | why it is there |
|-------------|----------|-----------------|
| `fneg`      | 4 — arm, arm64, mips, powerpc | LLVM 11 replaced the `fsub -0.0, x` idiom with a dedicated unary operator; this converter predates it |
| `freeze`    | 2 — arm64, powerpc | LLVM's own optimiser inserts it; no translator chose to emit it |

Both reach `LLVMInstructionConverter::visitInstruction`, which calls `FAIL()`
and aborts the process. Not a wrong result — the whole decompilation ends.

**`fneg` is not new, and not from this session's work.** `git blame` puts the
five `CreateFNeg` calls in `arm64.cpp` at `894733d`, the initial import. Any
ARM64 binary containing a floating-point negation has ended the decompiler
since then. Nothing caught it because not one of the 367 corpus sources
contained a `float` — which is exactly what the six added in the previous
commit were for, and they found it on the first run.

The conversions are both one line of meaning. `fneg x` is `NegOpExpr(x)`.
`freeze x` is `x`: it pins any undef or poison in its operand to a value the
instruction does not name, and a decompiler has no undef to pin, so there is
nothing to emit but the operand.

**`gcd_euclid` is the `freeze` one, and it is an integer program.** So this was
never a floating-point problem; widening the measurement is what found it. The
floored ARCH-01 step takes the first ten programs by name and would not have
reached either binary.

### A gate that reused objects it could not identify

Diagnosing the two above cost a stash-and-rebuild, because L2H-01 reported

```
undefined reference to llvm::APInt::toString(SmallVectorImpl<char>&, unsigned, bool, bool, bool)
```

from `const_int.cpp` — a file nothing in the working tree had touched. It was
not the working tree. `L2H_WORKDIR` reuses an object whenever the object is
newer than its source, which says nothing about the compiler or the LLVM
headers that produced it, and the directory I pointed it at held objects dated
six days earlier, built against a different LLVM. `APSInt::toString` is an
inline; the stale `const_int.o` carried a call to the five-argument
`APInt::toString` that LLVM 20 does not export, and the linker only complained
once something referenced that weak section.

The flags, the compiler and the LLVM version are stamped into the workdir now,
and a workdir stamped differently is emptied rather than reused.

**The first version of that guard missed its own motivating case.** It trusted
a workdir carrying no stamp — and the directory that caused the trouble
predated stamping, so it carried none. Run against it, the guard passed the
stale objects straight through and reproduced the identical link error. An
unstamped workdir is now discarded too: its provenance is unknown, which is
precisely what the check refuses. That is the third time in this work a checker
turned out to be more permissive than the thing it stood in for.

Two other notes from the same episode, neither worth a guard but both worth
not repeating:

  * Editing a shell script while it is running corrupts that run. Bash reads a
    script incrementally, so an edit shifts the bytes under the interpreter;
    this one died on `STAMP: unbound variable` at a line that defines `STAMP`
    twenty lines earlier.
  * The evidence that the working tree was innocent came from `git stash` and a
    clean rebuild — 2207 tests, linking fine — not from reading the diff and
    deciding it looked unrelated.

### 32-bit ARM claimed a capacity it had no registers for

`arm_conv.cpp` set

```cpp
_numOfFPRegsPerParam = 2;
_numOfVectorRegsPerParam = 4;
```

and declared neither `_paramFPRegs` nor `_paramDoubleRegs` nor
`_paramVectorRegs` — the only one of the eleven conventions that named none of
them. AAPCS-VFP passes floating-point arguments in s0–s15 and d0–d7 and returns
in s0 or d0, and this project's ARM corpus is hard-float, so every
floating-point parameter and return value on 32-bit ARM was invisible to
parameter recovery.

Nothing failed. `usesFPRegistersForParameters()` returns false when both lists
are empty, so the analysis never looked — while the two "how many registers may
one parameter span" numbers sat there reading as though the subject had been
thought about.

**Both lists, and why that is enough.** The blocker I recorded earlier was that
`arm_init.cpp` gives the S and D registers separate globals instead of
modelling them as one bank, so naming either alone catches only half the ABI.
MIPS has the identical split and solves it by declaring both `_paramFPRegs`
(single) and `_paramDoubleRegs` (double); ARM now does the same. The aliasing
that is still not modelled would matter only if a value were written as S and
read as D, which compiler output for a single argument does not do.

**And the hard/soft question does not block it either.** On soft-float ARM
floats pass in r0–r3. This analysis is driven by which registers a callee
actually *reads* before writing, so a soft-float function simply never reads
s0 and nothing is reported: the lists add candidates, they do not assert an
ABI. Reading `EF_ARM_ABI_FLOAT_HARD` would be the faithful thing and nothing
reads it today — `ElfFormat::getAbiVersion()` reads `EF_ARM_ABIMASK` and stops
there — but it is not needed to make this correct.

**What no gate here checks.** Nothing in this repository measures parameter
recovery on ARM: the F1 gate is x86-64 and nine binaries, and ARCH-01 checks
that decompilation exits 0, not what it recovered. So this change is a
declaration matching a published ABI, verified to compile and following the
MIPS precedent — and that is the whole of the evidence for it.

What *is* checked is the shape of the mistake. CC-CONV fails any convention
that sets `_numOfFPRegsPerParam` without declaring FP or double registers, or
`_numOfVectorRegsPerParam` without vector ones. Falsified against the real
pre-fix file, which it rejects with both rules.

### What is left on the other three, measured

COV-01 over the whole 168-binary multiarch corpus, floating-point sources
included:

| arch    | rate   | what is left |
|---------|--------|--------------|
| mips    | 1.0000 | nothing |
| powerpc | 1.0000 | nothing |
| arm64   | 0.9993 | `LD1`/`ST1` (NEON, 2 each) and `LDADDAL` (1) |
| arm     | 0.9517 | 536 skipped bytes; see COV-01's own header |

ARM's figure is not comparable and the tool says so: static and Thumb-mode code
disassembled in a single mode decodes as garbage, which is why its
uncovered list is led by `STC`, `CDP` and `LDC` -- coprocessor instructions
that are not in the binary at all.

`LDADDAL` is worth naming because it is not NEON and not a misdecode. The
ARMv8.1 LSE atomics -- `LDADD`, `LDCLR`, `LDEOR`, `LDSET`, `SWP`, `CAS` and
their acquire/release forms -- have **no entry at all** in the ARM64 table, not
even `nullptr`, while `LDAXR`/`STLXR` are implemented. Modern glibc uses LSE
where the target supports it. One occurrence in this corpus, so it is recorded
rather than fixed; `atomicrmw` is the obvious mapping and the existing
`setAtomic` calls show the shape.

### MIPS: the other half of the move

`mtc1` and `mfc1` move the low half of a 64-bit FPU register. `mthc1` and
`mfhc1` move the high half, and a compiler emits them in pairs to get a double
into and out of the FPU without going through memory. Both were `nullptr`, and
`MTHC1` was the only instruction COV-01 found untranslated in MIPS
floating-point programs.

The double lives in the `FDn` register that
`singlePrecisionToDoublePrecisionFpRegister` maps `$fN` onto, so the fix reads
that register's bits, replaces one half and writes it back. The low half
surviving is the whole point, so the test starts from `0x3ff0000012345678` --
a non-zero low half, which the obvious wrong implementation (store the shifted
high word) would clobber. Falsified exactly that way.

Keystone 0.9.2 will not assemble `mthc1` in its MIPS32 mode ("instruction
requires a CPU feature not currently enabled" -- it is MIPS32r2), so both go in
as encodings checked against capstone 5.0.9 first. MIPS coverage on
floating-point binaries: 0.9948 -> **1.0000**.

### ARCH-01: 40/40, and the floors go in

Run 288 at `517db16`:

```
ARCH-01: arm       10/10   1.0000
ARCH-01: arm64     10/10   1.0000
ARCH-01: mips      10/10   1.0000
ARCH-01: powerpc   10/10   1.0000
ARCH-01: overall   40/40   1.0000
```

against CC-01's 216/216 for x86-64 in the same run. Three commits earlier it
was 0/40 on every architecture. The four defects between those two numbers:

| what | where |
|------|-------|
| `ConstantInt::get(Type*, uint64_t)` asserts instead of truncating | four sites, each hidden behind the last |
| `ARM64_VAS_2D` missing from a switch with a throwing default | `extractVectorValue` |
| a lane extracted from a whole-register operand, yielding a `float` to an integer add | `extractVectorValue`, `translateAdd` |
| `-static`, which made the corpus a harder problem than the one x86-64 is asked | `build_multiarch_corpus.sh` |

Only the first was visible as "the decompiler is broken". The second and third
needed the failure logs read rather than the wall clock; the fourth was mine,
and needed the premise checked rather than believed.

The step is a floor now, per architecture and set at what was measured, which
is the road CC-01 took from 0/216 to 216/216. Per architecture and not in
aggregate: an overall floor of 1.0 would pass while one architecture went to
zero and another gained the same count, which is exactly what a parity gate
must not do.

### PowerPC floating-point arithmetic, and the emulator that could not run it

The load/store half landed first. This is the other 44 entries: `FADD FSUB
FMUL FDIV` and their single-precision forms, `FMR FNEG FABS FNABS FSQRT FRSP
FCPSGN FSEL`, the `FMADD` family, `FCMPU`, the estimates (`FRE FRSQRTE`), the
rounding forms (`FRIM FRIN FRIP FRIZ`) and the twelve integer conversions.
PowerPC's dispatch table now has no `PPC_INS_F*` left at `nullptr`.

Three things worth writing down.

**`FCMPU` cannot use `storeCrX`.** That helper builds integer comparisons, and
it writes a constant zero into the fourth bit of the CR field because for the
integer compares that bit is a copy of XER, which is not modelled. For a
floating-point compare the fourth bit is FU, "unordered", and it is the only
thing that distinguishes comparing a NaN from comparing two equal numbers. So
`translateFcmp` writes the field itself, from `fcmp olt/ogt/oeq/uno`.

The `crReg -> {lt, gt, eq, so}` switch was written out three times in
`powerpc.cpp` already. A fourth copy was not the answer; `crFieldRegisters()`
is, and the existing three now call it.

**The single-precision forms round, and my first test could not tell.** An `S`
form computes in double and rounds the result to single before writing it
back. The obvious test value is 0.1, whose double and float-rounded-to-double
representations differ by about 1.5e-9 — and this harness compares doubles to a
tolerance of 0.001, so that test passed whether the rounding happened or not. I
found this by deleting `roundToSingle` and watching the test still pass. The
value is 2^24+1 now: the smallest integer a float cannot represent, which
rounds to 2^24, a difference of 1.

**The emulator could not execute any of it.** `tests/capstone2llvmir` runs the
IR it produces, and `src/llvmir-emul` handled none of the floating-point
intrinsics:

  * `IntrinsicLowering` turns most of them into libcalls — `sqrt`, `floor`, … —
    which the interpreter cannot resolve, so `fsqrt 16.0` came back 0.
  * For `llvm.fma` it does not even do that. It reports "Code generator does
    not support intrinsic function" through `report_fatal_error`, which ends
    the process: one `fmadd` test killed the whole 4,477-test binary.
  * `llvm.fabs`, `llvm.minnum` and `llvm.maxnum` were already excluded from
    lowering, with the comment "can not lower those functions", and then fell
    through to the unhandled-external path — where a `double` return leaves
    `GenericValue::DoubleVal` uninitialised. They were returning whatever was
    on the stack, and the ARM64 and x86 tests that use them only ever asserted
    that the call was *recorded*, never what it returned.

`visitCallInst` now computes fabs, sqrt, floor, ceil, trunc, round, roundeven,
nearbyint, rint, copysign, minnum, maxnum and fma directly, for f32 and f64.
Not for x86_fp80: `GenericValue` keeps those in `IntVal`, not `DoubleVal`, so
the fp80 forms keep the path they had. And it still records the call, because
six ARM64 and x86 tests assert exactly that — which is how I found out I had
broken them.

Eleven tests in `tests/llvmir-emul`, under a class name the C2L-01 filter
actually matches (`LlvmIrEmul*`). The first draft called it `FpIntrinsicTests`,
which the gate's own filter would have skipped: eleven tests that could
disappear without the floor noticing.

### Not one of the 367 corpus sources contained a float

```
$ find tests/algorithm_recovery/sources -name '*.c' | wc -l
367
$ find tests/algorithm_recovery/sources -name '*.c' | xargs grep -lE '\b(float|double)\b' | wc -l
0
```

CC-01, DET-01, the F1 recovery gate and ARCH-01 all run on this corpus. None of
them has ever put a floating-point instruction through the decompiler — on
*any* architecture, x86-64 included. "216/216 emitted C files compile" is 216
integer programs.

That is the honest limit on the PowerPC floating-point work above: it is
covered by 834 unit tests in `tests/capstone2llvmir`, and by no end-to-end gate
at all, because no end-to-end gate has anything to run it on.

Two related gaps, found while checking whether the FP work would even reach the
emitted C:

  * **The PowerPC path is otherwise complete.** `powerpc_conv.cpp` already
    lists `_paramFPRegs` F1–F8 and `_returnFPRegs` F1. The machinery was
    waiting for values that no instruction produced.

  * **32-bit ARM has no FP registers in its calling convention at all.**
    `arm_conv.cpp` sets `_numOfFPRegsPerParam = 2` and
    `_numOfVectorRegsPerParam = 4` and then declares neither `_paramFPRegs` nor
    `_returnFPRegs` — the only one of the eleven conventions that does not.
    `arm_init.cpp` models `ARM_REG_S0` as f32 and `ARM_REG_D0` as f64, so the
    registers exist. The corpus is built for `arm-linux-gnueabihf`, whose ABI
    passes floats in s0–s15 and returns in s0, so on that ABI every float
    parameter and return is currently invisible to parameter recovery.

    Not fixed here, and not a one-line fix: `arm_init.cpp` gives S and D
    registers separate globals rather than modelling the aliasing, so choosing
    `_paramFPRegs = {S0..S15}` or `{D0..D7}` decides which half of the ABI
    works. And which list applies at all depends on EF_ARM_ABI_FLOAT_HARD in
    the ELF header, which nothing reads. Writing the register list without
    settling those two would be guessing.

## The "nobody builds this" check never asked it about src/

**Closed by extending `check_cmake_sources.sh`.** The check exists to catch
sources no CMakeLists names — its own comment says *"a check written to catch
'nobody builds this' that skips the directories nobody builds is worse than
none, because its OK is trusted"* — and it walked `tests/` only. `src/` was
never asked the question.

Found while chasing the `ConstantInt::get` class: `types_propagator.cpp`
appeared in a compile sweep with

```
error: 'resolveTypes' was not declared in this scope
error: no declaration matches 'bool TypesPropagator::resolveTypes()'
```

which cannot compile on any compiler — and CI builds bin2llvmir fine, because
nothing compiles that file.

Three sources under `src/` are named by no CMakeLists, and the check now
carries each with its reason:

  * `unpackertool/plugins/example/example.cpp` — a template for writing an
    unpacker plugin. Compiles; building it would register an unpacker that
    unpacks nothing. Legitimately unbuilt.
  * `rtti-finder/vtable/vtable_xref.cpp` — 244 lines, **does not compile**: 14
    errors against the current API, including `VtableGcc::virtualFunctions`,
    which does not exist. Its header is declared and included by nothing.
  * `bin2llvmir/optimizations/types_propagator/types_propagator.cpp` — 347
    lines, **does not compile**, as above. `simple_types/` is the types pass
    the build actually uses.

591 lines across the two broken files, neither of which has ever been through
a compiler. They are recorded rather than repaired: making dead code compile is
polish on something nothing runs, and wiring an unverified optimisation pass
into the pipeline is a behaviour change with no evidence behind it. What is
fixed is that the next one cannot arrive unnoticed — falsified two ways, by
adding an unnamed source under `src/` and by deleting a reason from the list.


## bin2llvmir's "real drift across many files" was one API in seven files

The largest ungated suite carried this reason:

> uses LLVM 20+/21+ APIs (CmpPredicate, Intrinsic::getOrInsertDeclaration,
> Value::hasUseList); measured against system LLVM 18, it is real drift across
> many files, not a missing -I

Measured against `llvm-20-dev` rather than 18, it was 8 files of 125. One of
those was a missing `-Ideps/eigen`; one was `types_propagator.cpp`, which is
built by nothing and does not compile at all. The other six, plus one more,
were a single API: `Value::hasUseList()`, which LLVM 21 added when it stopped
giving every Value a use list and made `users()` assert for the ones without.

`llvm_utils::hasUseList()` spells that for both — `hasUseList()` from 21, and
`true` before it, where every Value had a use list and `users()` asserted for
none. The shipped build takes the first branch, so this is a no-op there.

With it: **all 124 buildable `src/bin2llvmir` sources compile against LLVM 20,
and 30 of the 31 in `tests/bin2llvmir`.**

**What still blocks the suite, precisely.** `src/debugformat/dwarf.cpp`
includes `llvm/DebugInfo/DWARF/LowLevel/DWARFExpression.h`, which exists only
from LLVM 21, and `DebugFormat::loadDwarf()` is referenced by the link — so the
test binary cannot be linked here even though almost everything compiles.
Separately, `tests/bin2llvmir/utils/simplifycfg_tests.cpp` `#include`s an LLVM
*source* file (`../lib/Transforms/Scalar/SimplifyCFGPass.cpp`) from the
vendored llvm-project tree, so it only builds inside the full build.

That is worth the distinction. "Real drift across many files" is a reason to
stop looking; "one file needs one LLVM 21 header" is a reason to keep going,
and it is what the measurement says.


## The 124 ARM64 atomics that were not `nullptr` — they were absent

COV-01's arm64 leftovers named `LDADDAL`, and the entry above recorded it
rather than fixing it: one occurrence in the corpus. Then I went looking for
the entry to change and there wasn't one. `LDADD`, `LDCLR`, `LDEOR`, `LDSET`,
`LDSMAX`, `LDSMIN`, `LDUMAX`, `LDUMIN`, `SWP` and `CAS` — ten operations, four
ordering suffixes each (plain, `A`, `L`, `AL`), three widths each (`B`, `H`,
word/doubleword) — had **no key at all** in `arm64_init.cpp`'s dispatch map.
Not a translator that declined to model them. Ids the dispatch could never
reach, which is a different thing from a `nullptr` and reads differently in a
`grep`: a `nullptr` count says "we know about these"; an absent key says
nothing at all.

`LDAXR`/`STLXR`, the ARMv8.0 load/store-exclusive pair they replace, *are*
implemented. So the gap is specifically ARMv8.1, and a compiler told
`-march=armv8.1-a` or later emits LSE for every atomic in the program. Modern
glibc does this at runtime through ifuncs. On x86-64 the equivalent
instructions — `LOCK XADD`, `CMPXCHG` — translate to `atomicrmw` and
`cmpxchg`, and llvmir2hll converts both into C. This was the same path all
along; only ARM64's use of it was missing.

**The operand order is uniform, and CAS breaks it.** Measured with capstone
5.0.9:

```
op0 = Rs (the value)   op1 = Rt (the destination)   op2 = [Xn]
```

so `ldadd w1, w2, [x3]` reads `w1`, adds it to the memory at `x3`, and writes
the *old* memory value to `w2`. `cas` uses the same three slots for different
roles: `cas Rs, Rt, [Xn]` compares against Rs, stores Rt on a match, and writes
the old value back to **Rs**. The destination is operand 0, not operand 1 — the
opposite of the other nine. That is the sort of thing a family-wide loop gets
wrong silently, so `translateCas` is a separate function.

**Two things do not map straight across.**

`LDCLR` clears the bits set in Rs, which is `And` with the complement, not
`And` — `ldclr` with operand `0xff` clears a byte, it does not keep it. There
is no `AtomicRMWInst::AndNot`, so the value is `CreateNot`-ed first and the op
is `And`.

`CAS`'s failure ordering may not be stronger than its success ordering and may
not be `Release` or `AcquireRelease` at all (LLVM asserts). `CASL` and `CASAL`
therefore take `Monotonic` on the failure path, which is what the architecture
means anyway: a release barrier on a store that did not happen is not a thing.

`CASP`, the 128-bit register-pair form, is deliberately not here. It needs a
register pair on each side and `cmpxchg` takes one value; it falls through to
the pseudo-asm path, as it did before.

**The width comes from the mnemonic, not the register.** `ldaddb w1, w2, [x3]`
names W registers and touches one byte. The first version of the test could not
have caught this getting it wrong, twice:

* First attempt: plain values, so an `i32` RMW and an `i8` RMW at the same
  address computed the same answer.
* Second attempt: a neighbouring `0xff` byte that a 32-bit access would drag
  in — which also passed, because the llvmir-emul memory model is an
  address→`GenericValue` map, not a byte array. Access *width* is not
  observable through that emulator at all, no matter what you put next door.

The test that works asserts on the IR: find the `AtomicRMWInst`, ask
`getValOperand()->getType()->getIntegerBitWidth()`, require 8. Falsified by
returning `getDefaultType()` from `lseAccessType` — 8 vs 32, which is the
failure the first two versions were supposed to produce.

**Keystone cannot assemble any of this.** 0.9.2 is from 2017 and predates
ARMv8.1; it refuses every LSE mnemonic. The encodings are from
`aarch64-linux-gnu-as` and checked against capstone 5.0.9 before use, the same
route the MIPS `mthc1` entry above took.

### Three emulator defects the atomics tests surfaced

`llvmir-emul` is what the capstone2llvmir tests run the translated IR on, so
adding atomics to ARM64 immediately ran into what it does with them.

`atomicrmw` handled `Xchg` and nothing else — every other op fell through to
the same "store the new value" path, so `add`, `and`, `or`, `xor`, `max`,
`min`, `umax` and `umin` all silently behaved like `xchg` and returned the
right *old* value while writing the wrong *new* one. A test that only reads the
result would pass. Now all twelve integer ops compute.

`cmpxchg` was not handled at all.

`extractvalue` returned a default-constructed `GenericValue` — i.e. whatever
was on the stack — instead of the requested element. That one is not
atomics-specific: it is wrong for every aggregate, and it was reachable before
this branch by any IR that used one. `cmpxchg` is what made it load-bearing,
since its result is a `{value, i1}` pair.

### llvmir2hll: four `atomicrmw` kinds that reached the empty statement

The earlier entry on `atomicrmw` conversion wired `add`, `sub`, `and`, `or`,
`xor` and `xchg`, and said the remaining forms "are not lowered and take the
empty-statement path". `LDSMAX`/`LDSMIN`/`LDUMAX`/`LDUMIN` are exactly those
remaining forms, so that path stopped being hypothetical. All four are now
`TernaryOpExpr`s — `a > b ? a : b` with `GtOpExpr::Variant::SCmp` for the
signed pair and `UCmp` for the unsigned — which is what the operation means and
what a reader wants to see.

`Nand` is still not converted, and this is a real limitation rather than an
oversight: BIR has no bitwise-not. `NotOpExpr` emits `!`, the logical one, so
writing `!(a & b)` for `~(a & b)` would be a silent miscompilation of exactly
the kind section 3 is about. It keeps the empty-statement path until BIR grows
the operator.

### What it is gated at

`MIN_ARM64` goes 471 → **481**. The ARM64 suite is the only one that moves.



## The architecture everything else is measured against could not add two doubles

Asked to bring ARM, ARM64, MIPS and PowerPC up to x86-64's level, I spent most
of this branch on those four. Then I went to compare `x86_init.cpp` against
`arm_init.cpp` and found this:

```
{X86_INS_ADDSD, nullptr},
{X86_INS_SUBSD, nullptr},
{X86_INS_MULSD, nullptr},
{X86_INS_DIVSD, nullptr},
{X86_INS_UCOMISD, nullptr},
{X86_INS_COMISD, nullptr},
{X86_INS_SQRTSD, nullptr},
{X86_INS_MAXSD, nullptr},   {X86_INS_MINSD, nullptr},
{X86_INS_CVTTSD2SI, nullptr},
{X86_INS_XORPD, nullptr},   {X86_INS_ANDPD, nullptr},
{X86_INS_MOVSS, nullptr},
```

The whole double-precision half of SSE2. On x86-64 the System V ABI passes and
returns a `double` in an XMM register, and gcc compiles `a + b` on doubles to
`addsd`; there is no other way to write it. So the one architecture with 252
binaries of end-to-end evidence behind it, the control every other number in
this branch is quoted against, turned every floating-point program into a wall
of `__asm_addsd` and `__asm_movsd` calls.

`MOVSD` was worse than `nullptr`. Capstone gives `movsd xmm0, qword ptr [rdi]`
and the string instruction `movsd` **the same id**, and the table pointed that
id at `translateMoveString`, which rejects anything whose operands are not both
memory. There is even a TODO in that function naming the exact address in the
exact sample where somebody hit it. Counted over the six floating-point corpus
programs, `movsd` is the second most frequent instruction in the whole text
section, behind `mov`.

### Why no gate saw it

Three reasons, and each is worth fixing in its own right.

**COV-01 did not measure x86.** Its four architectures were the non-x86 four,
because the tool was written to answer "are the other four as good as x86-64"
and nobody thought to point it at x86-64. Its own header explains at length why
a dispatch-table ratio is the wrong number and a corpus-weighted one is right;
the corpus it weighted by had no x86 in it. It does now:

| arch    | before | after  | what is left |
|---------|--------|--------|--------------|
| x86_64  | 0.9846 | 0.9931 | `HLT` -- one per binary, alignment padding |

**COV-01 cannot see a translator that declines.** Its question is whether the
id has a non-null function pointer, so `MOVSD` counted as covered in both
columns above while producing a pseudo-asm call every time. The 0.9846 was
therefore an overstatement, and the real gap was larger than the table shows.
This is recorded rather than fixed: answering it properly means running the
translator over every decoded instruction and asking whether a `__asm_` call
came out, which is a different tool from a disassembler and a dispatch table.

**Nothing ran COV-01 at all.** It is a script with no caller -- not in
`check_push_gates.sh`, not in any workflow. A measurement nobody takes is a
measurement nobody has.

**`x86_sse.cpp` had no tests.** 668 lines, 24 translators, and the string
`XMM` appeared in `tests/capstone2llvmir/x86_tests.cpp` eight times, all of
them inside comments about `FXSAVE`. Two of those translators --
`translateSsePshufd` and `translateSsePbyteShift` -- had no dispatch entry
either: written, declared, compiled, unreachable. The file's own header says
"the apply script does this automatically via sed", and for those two it did
not.

### What is implemented

The machinery was already there. `scalarFltBinOp` and `packedFltBinOp` both
take an `isDouble` flag and neither had a caller that passed `true`.

* `translateSseFltArith` -- ADD/SUB/MUL/DIV across SS, SD, PS and PD. The
  scalar forms leave the upper lanes alone; that is the entire difference
  between `addsd` and `addpd`, and it is what the tests check.
* `translateSseFltMinMax` -- MAX and MIN. Deliberately not `llvm.maxnum`:
  x86 defines `MAXSD` as `dst > src ? dst : src`, so a NaN in either operand
  yields the **source**, where `llvm.maxnum` returns the non-NaN operand. A
  select spells the architecture's definition exactly and the intrinsic does
  not.
* `translateSseSqrt` -- the one shape nothing else has: the root of the
  *source's* low lane, with the *destination's* upper lanes kept.
* `translateSseComi` -- UCOMIS\*/COMIS\* to EFLAGS, branchlessly. The
  architecture's table is
  `unordered ZF=PF=CF=1`, `less CF=1`, `equal ZF=1`, `greater all clear`,
  which is `ZF = fcmp ueq`, `CF = fcmp ult`, `PF = fcmp uno` and nothing else.
  COMIS\* differs from UCOMIS\* only in which NaNs raise an exception and this
  lifter has no exception state, so they translate the same.
* `translateSseFltLogic` -- AND/ANDN/OR/XOR on all 128 bits. These are in
  every floating-point binary because they are how a compiler writes negation
  and absolute value. ANDN complements the **destination**, the reverse of what
  the operand order suggests.
* `translateSseMovScalar` -- MOVSS and the SSE MOVSD, telling the SSE form from
  the string form by whether an operand names an XMM register. Three cases and
  the difference between the first two is the reason this cannot be a
  whole-register move: register-to-register preserves the upper lane, memory-to-
  register zeroes it.
* `translateSseMovHalf`, `translateSseCvtFlt`, `translateCvtTt2Si`,
  `translateSseUnpck`, `translateSseShufp`, `translateSseMovMsk` -- MOVLP\*/
  MOVHP\*, the float-to-float conversions, the truncating conversions to
  integer, UNPCK\*, SHUFP\* and MOVMSKP\*.

Plus the two orphans wired, and MOVUPS/MOVUPD pointed at the whole-register
move they always were.

### Two rounding bugs found while reading the neighbours

`translateCvtSd2Si` carried the comment *"Round toward nearest (C default); use
FPToSI (truncation)"* -- two halves of a sentence that contradict each other.
`FPToSI` truncates, so `cvtsd2si rax, xmm0` on 2.7 answered 2. The instruction
that truncates is `CVTTSD2SI`, which is why it is a separate opcode; both are
now what they say they are, and the test for each would fail on the other's
implementation.

`translateCvtPs2Dq` had the same shape: it truncated, `CVTPS2DQ` rounds, and
`CVTTPS2DQ` -- the one it was actually implementing -- was not in the table.

Both also took their destination width from `_basicMode`, so a 64-bit program
was assumed to be converting into a 64-bit register. The width is the operand's:
`cvttsd2si eax, xmm0` is a 32-bit conversion inside a 64-bit program. For
in-range values the two agree after `storeOp` truncates, which is why no test
here claims this as a behaviour fix -- it is written from the right source now
and that is all.

### The emulator returned zero for every `extractelement`

```cpp
void LlvmIrEmulator::visitExtractElementInst(llvm::ExtractElementInst& I)
{
	GenericValue dest;
	_globalEc.setValue(&I, dest);
}
```

Identical to the `visitExtractValueInst` defect recorded above, and reachable
by anything that reads a lane. `ADDPD` passed its test on the first run because
a whole-vector `fadd` never extracts; `ADDSD` answered 0.0 for 1.0 + 2.0. Thirty
of the new tests failed on it, which is how it was found, and no test could have
found it before because none of them had ever used a vector.

`visitInsertElementInst` is fixed alongside it: it sized the result from its
input vector, and an insert into `poison` arrives with an empty `AggregateVal`,
so the inserted lane was dropped.

### x86-64 in the multiarch corpus, as the control column

`build_multiarch_corpus.sh` built four architectures because the x86-64 corpus
already existed. But the parity question is "is every architecture at x86-64's
level", and that is only answerable if x86-64 is measured on the same sources,
the same flags and the same tools. It is built here now too, through its
triple-prefixed driver (`x86_64-linux-gnu-gcc`) so that nothing below needs a
special case, and ARCH-01 and COV-01 both report it alongside the other four.

Its first COV-01 number was the lowest of the five.

### COV-01 is a gate now, on all five

Measured over the 210-binary corpus, after everything above:

| arch    | rate   | what is left |
|---------|--------|--------------|
| x86_64  | 0.9931 | `HLT` -- one per binary, alignment padding |
| arm     | 1.0000 | nothing |
| arm64   | 0.9994 | `LD1`/`ST1` (NEON, 2 each) |
| mips    | 1.0000 | nothing |
| powerpc | 1.0000 | nothing |

x86-64 is the lowest of the five. What is left of it is `HLT`, once per binary,
in the alignment padding after `_start` -- code that never runs. It stays
uncovered rather than being pointed at a translator that emits the pseudo-asm
call it already emits, which would move the number and change nothing.

ARM's uncovered list no longer carries `<id 52> NO ENTRY`. That was
`ARM_INS_FCONSTD` -- `vmov.f64 d0, #1.0`, the VFP move-immediate, which
capstone prints as "vmov" but gives an id of its own, and which was not a key
in the table. `FCONSTS` is its single-precision twin and was equally absent.
`translateVfpMov` already handled the `ARM_OP_FP` operand these carry, so both
are one dispatch entry each. The rest of ARM's list took a change to the tool
before it could be read at all; that is the next section.

The floors are the measured values, not a notch below them. That needed one
correction to be possible at all: the rate is printed to four decimals and was
compared as a full double, so `6003/6045` displayed as `0.9931` and compared as
`0.99305`, and every architecture failed the floor copied from its own output.
A gate whose displayed number and compared number differ is a gate nobody can
set a floor for. It compares at the printed precision now, and the self-test
covers the three outcomes -- no floor, an unreachable floor, a floor of zero --
each of which flips if the gating is removed. Falsified by making the failure
branch `return 0`: two of the four self-test cases fail.


## What 32-bit ARM was really missing, once it could be measured

COV-01's ARM row came with a paragraph of apology: 536 bytes it could not
decode, and an uncovered list led by `STC`, `CDP` and `LDC` -- coprocessor
instructions that are not in these binaries at all. The tool's own header said
so: an ARM figure from it "is a lower bound with a wide error bar, and its
uncovered list should not be used to decide what to implement."

That is fixable, and the fix is in the ELF. 32-bit ARM objects carry `$a`, `$t`
and `$d` mapping symbols marking runs of ARM code, Thumb code and inline data.
Following them, COV-01 disassembles each run in the mode it is actually in and
drops the literal pools rather than pretending to decode them:

```
                 decoded   skipped   covered     rate   uncovered kinds
before              5300       536      5055   0.9538   24
after               8060         0      7698   0.9551    6  (3808 bytes of data)
```

The rate barely moved. Everything else did. `skipped` is zero, a third more
instructions are being read at all, and the uncovered list stopped being
fiction:

```
ARM_INS_HINT   238   2.95%   listed, null
ARM_INS_IT      78   0.97%   listed, null
ARM_INS_ADR     43   0.53%   listed, null
ARM_INS_VPUSH    1           listed, null
ARM_INS_VPOP     1           listed, null
ARM_INS_ORN      1           listed, null
```

Six real instructions instead of twenty-four phantoms, and three of them are
one line of dispatch each.

### `ARM_INS_NOP` is a key capstone does not produce

`HINT` is capstone's id for the entire hint space -- `nop`, `yield`, `wfe`,
`wfi`, `sev`, `sevl`, `dbg`, the pointer-authentication and branch-target
hints -- and `ARM_INS_NOP` exists but is not what a `nop` decodes to. The table
had `ARM_INS_NOP` pointing at `translateNop` and no entry for `HINT`, so the
translator that does the right thing sat on a key nothing arrives at. Third
time this branch has found that shape: `ARM_INS_VMRS` against `ARM_INS_FMSTAT`,
a duplicate `ARM64_INS_HINT` key, and now this.

There was a test. It was called `ARM_INS_NOP`, and it asserted that `nop` comes
out as a call to `__asm_nop`:

```cpp
EXPECT_JUST_VALUES_CALLED({
    {_module.getFunction("__asm_nop"), {}},
});
```

Both halves wrong and agreeing with each other. The test was written from the
behaviour observed rather than the behaviour the instruction has, so it locked
the defect in: a `nop` that produced nothing would have failed it.

Most of the hint space is genuinely a no-op and is translated as one. `WFI`,
`WFE`, `SEV`, `SEVL` and `DBG` are not -- they wait on or signal an external
event -- and keep their pseudo-asm call rather than being silently dropped,
which is the difference between "this does nothing" and "we do not model what
this does". Capstone gives these no operand at all, so the mnemonic is the only
thing that separates them.

### Thumb reads PC as address + 4, at every width

```cpp
return llvm::ConstantInt::get(
        getDefaultType(),
        ((i->address + (2*i->size)) >> 2) << 2);
```

ARM reads PC as the instruction's address plus 8, and `2*size` is 8 for a
4-byte ARM instruction. Thumb reads it as the address plus 4 -- and plus 4
whether the instruction is 16 or 32 bits wide. `2*size` is 4 for a 16-bit Thumb
instruction, which is right, and 8 for a 32-bit one, which is four bytes past
where the architecture says PC is. Every Thumb-2 PC-relative load and every
32-bit `ADR` was off by a word.

It looked correct because it is correct in two of the three cases, and the
third had no test. `addw r0, pc, #20` at address 0 answered 28 and answers 24
now.

### `IT` is a no-op, and that is a fact about the dispatcher

`IT` makes the next one to four instructions conditional. Capstone puts that
condition on each of those instructions' `cc`, and `arm.cpp`'s dispatcher
already wraps any instruction whose `cc` is not `AL` in a generated condition.
So the block header has no effect of its own and the right translation is
nothing -- but only because of what happens elsewhere, which is why the entry
carries a comment saying so rather than looking like an oversight.

### `ADR`, `ORN`, `VPUSH`, `VPOP`

`adr rN, label` is PC plus an immediate -- how a compiler names an address in
its own function without a literal pool. Capstone reports the offset, not the
resolved address, so a translation that stored the immediate would answer 20
where the answer is 24.

`orn rd, rn, op2` is `rn | ~op2`; the complement is on the second operand, not
on the result.

`VPUSH`/`VPOP` cannot reuse `translateLdmStm`, which writes every slot at
`getArchByteSize()` -- four. A D register is eight, so a list of them pushed
four bytes apart overlaps itself. The test pushes `{d0, d1}` and checks both
addresses, which is the assertion the integer translator would fail.

### Where ARM ends up

```
arch         decoded   skipped    covered     rate  uncovered-kinds
arm             8060         0       8060   1.0000  0  (3808 bytes mapped as data)
```

Every instruction in the 42 ARM binaries has a translator. The number it
started this branch at was 0.9517, and the number it would still be showing
without the mapping-symbol change is 0.9551 -- with six real gaps hidden behind
twenty-four that were never there.

### One failure that has not come back

One C2L-01 run in this sequence reported `PPC_INS_MR/CS_MODE_64` failed. It is
a three-line test -- set r11, `mr 0, 11`, expect r0 -- in an architecture this
branch had not touched since the floating-point work, and it has not failed
again: the same binary has since run the whole suite clean twelve times, five
of them back to back with nothing else on the machine, plus six runs of the
PowerPC tests alone.

It is recorded rather than explained. "Flake" is not a root cause and this is
not being called one; what can be said is that it did not reproduce, and that
the counts and the gate are green on the tree being pushed. If it returns, the
thing to look at first is `GenericValue`'s default constructor, which leaves
`DoubleVal` uninitialised -- the same shape as the `extractelement` and
`extractvalue` defects above, and the only source of nondeterminism this
emulator has.


## The hollow-binary check reported failure exactly when it succeeded

`--link static` has been unusable since the hollow check went in, and the
reason is four characters:

```sh
if ! "${triple}-nm" "${bin}" 2>/dev/null | grep -qE '^[0-9a-f]+ [Tt] main$'; then
```

`set -o pipefail` is on. `grep -q` exits the instant it matches, which closes
the pipe; `nm` is then killed by SIGPIPE and exits 141; `pipefail` makes 141 the
pipeline's status; the `!` turns that into "no defined main". The check reports
a hollow binary **because** the symbol is there.

It never fired on the dynamic corpus and fired on all 210 binaries of the
static one, which is the tell. A dynamically linked cross binary has a hundred
or so symbols -- a few kilobytes, well inside the pipe buffer, so `nm` finishes
writing before `grep` has decided anything and never sees SIGPIPE. A static one
has thousands, comfortably past 64 KB, so `nm` is always still writing.

`grep` without `-q` reads its input to the end. That is the fix.

The rest of the repository was checked for the same shape. Seven other scripts
combine `pipefail` with `| grep -q`, and all of them pipe from `printf` of a
small variable, `find` over a handful of files, or `head -n 8` -- outputs that
fit in the pipe buffer, where the producer completes regardless of when the
consumer leaves. The hazard is specifically a large producer, which is why the
one instance that mattered was the one reading a symbol table.

This one is mine, from earlier in this branch, and it is the same lesson as the
rest: the check was written and observed to pass on the corpus at hand, and the
corpus at hand could not make it fail.

## What a static corpus says, which is a different thing

`--link static` drags in all of glibc: its hand-written SIMD, its string
routines, its syscall stubs. It is deliberately not a gate -- it is a harder
measurement than the x86-64 corpus rather than the same one -- but it is the
best guide there is to what to implement next. Over the same 210 programs built
`-static`:

| arch    | decoded   | rate   | led by |
|---------|-----------|--------|--------|
| x86_64  | 5,230,330 | 0.9447 | `VPCMPEQB`, `VMOVDQU`, `PMOVMSKB`, `VPMOVMSKB`, `VMOVDQU64`, `TZCNT`, `PALIGNR`, `KMOVD` |
| arm64   | 3,592,964 | 0.9836 | `UDF`, `MRS`, `EXT`, `ST1B`, `SVC` |
| arm     | 3,398,752 | 0.9937 | `MRC`, `PLD`, `TBB`, `STCL`, `LDCL` |
| mips    | 4,292,791 | 0.9952 | `RDHWR`, `PREF` |
| powerpc | 4,640,553 | 0.9980 | `SC` |

The question this branch was given was whether the other four architectures
reach x86-64's level. On glibc-heavy code they are all **ahead** of it, and not
narrowly: x86-64 is four points behind the next worst, because its list is
AVX2 and AVX-512 -- `VPCMPEQB`, `VMOVDQU64`, `KMOVD`, `VZEROUPPER` -- and
nothing in `src/capstone2llvmir/x86` models a YMM or ZMM register at all.

That is recorded, not fixed. AVX needs 256-bit registers in the register file
before any of its instructions can be translated, which is a subsystem rather
than a table entry, and none of it is in user code -- it is in `memcpy` and
`strlen`, behind ifunc dispatch.

Four arm64 ids have **no entry at all**, which is the "the table predates the
instruction" shape rather than a decision: `UDF` (17,340 occurrences, the most
frequent untranslated arm64 instruction in this corpus), `LD1B` and `ST1B`
(SVE) and `LDG` (MTE). They already take the pseudo-asm path, since an id with
no entry falls through to it, so adding entries would move the number without
changing what the decompiler emits -- which is the same reason x86-64's `HLT`
is left where it is.

COV-01 follows AArch64's `$x`/`$d` mapping symbols now as well as ARM's
`$a`/`$t`/`$d`, so arm64's list is code rather than padding. It changed nothing
on the dynamic corpus, whose `.text` carries no inline data, and it is the
honest thing to do before quoting an arm64 uncovered list at all.


## Two aborts the new translations reached, and the gates that found them

ctest-linux 292, at `fb31221`, went red on CC-01 and on ARCH-01's arm64 floor.
Both were mine, both were aborts rather than wrong answers, and neither was
reachable before this branch because before this branch the instructions that
reach them came out as `__asm_*` calls.

This is the gates doing their job on the first run after the change, which is
what they are for. It is also the third time in this branch that widening what
gets translated has turned an unreachable path into a reachable one, after
`fneg` and `freeze`.

### `atomicrmw` was inlinable, and inlining it aborted

```
llvm_instruction_converter.cpp:554: visitInstruction: Fail (unsupported
  instruction: %4 = atomicrmw add ptr %2, i32 %3 acq_rel, align 4)
#9  InstVisitor<LLVMInstructionConverter,...>::visitAtomicRMWInst
#16 LLVMInstructionConverter::convertExtCastInstToExpression
#17 LLVMInstructionConverter::visitZExtInst
```

`BasicBlockConverter` converts `atomicrmw` as a *statement*, and has done since
the C11-atomics fix earlier in this branch. `LLVMInstructionConverter` converts
values used as operands into *expressions*, and has no case for it. Which of
the two sees an instruction is decided by `LLVMSupport::isInlinableInst()`, and
that said yes: one use, same basic block, not a call or a load or a phi.

ARM64's `ldadd` writes the old value of the memory into a register, so the
`atomicrmw` has exactly one use -- a `zext` -- and every binary containing one
ended the decompiler. `generated_atomic_counter-arm64-gcc-O0`, the whole
reason that source is in the corpus.

`atomicrmw` and `cmpxchg` belong on the not-inlinable list for the reason
`LoadInst` is already on it, and more so: each is a load *and* a store, so
moving one to its use site reorders a write. With them on it, the use becomes
the variable `BasicBlockConverter` already assigns to.

Three tests, in `tests/llvmir2hll/llvm/llvm_support_tests.cpp`. The third one
is there because the first two would pass against a function that always
returns false.

### `insertvalue` does not take a vector

```
Assertion `ExtractValueInst::getIndexedType(Agg->getType(), Idxs)
           == Val->getType() && "Inserted value must match indexed type!"'
#13 (anonymous namespace)::convertToType  ir_modifier.cpp:394
#15 IrModifier::modifyFunction
#16 ParamReturn::applyToIr
```

All fifteen CC-01 failures were this one assertion, and so were four of
ARCH-01's five.

`convertToType()` grouped vectors with structs and arrays and converted into
them with `insertvalue`. `extractvalue` and `insertvalue` are struct and array
only: `ExtractValueInst::getIndexedType(<2 x double>, {0})` returns **null**,
so the assertion compares null against `double` and fires. Checked directly
against LLVM 20 rather than inferred -- `getIndexedType` answers `<null>`, and
`insertvalue <2 x double> undef, double %a, 0` builds without complaint on an
assertions-off LLVM and is rejected by the verifier as "Invalid InsertValueInst
operands!". CI's LLVM has assertions on, so it aborts instead.

The vector forms are `insertelement` and `extractelement`. The other direction
was missing too and was worse: a `<2 x double>` asked to become a `double` fell
off the end of the chain into `llvm_unreachable()`.

`ParamReturn` asks for the conversion when it rewrites a function's return
type, which is why this arrived the moment `ADDSD` and friends started
producing `<2 x double>` values instead of pseudo-asm calls.

Falsified both ways against a real module: with the fix, `insertelement` and
`extractelement` and a clean verifier; with `ir_modifier.cpp` alone reverted,
`Invalid InsertValueInst operands!` on the first conversion and
`UNREACHABLE executed at ir_modifier.cpp:420` on the second.

### What ARCH-01's new column said

```
ARCH-01: x86_64    38/42   0.9048
ARCH-01: arm       42/42   1.0000
ARCH-01: arm64     41/42   0.9762
ARCH-01: mips      42/42   1.0000
ARCH-01: powerpc   42/42   1.0000
```

x86-64 is in this table for the first time, unfloored, as the control column --
and on its first run it was the worst of the five. That is the entire argument
for putting it there.


## Asking the question up front instead of four times after the fact

Four aborts of one shape were found in this branch, each by a corpus run after
the change that made it reachable:

| instruction | reached by | found |
|-------------|------------|-------|
| `fneg` | every FP negation, any architecture, since the initial import | the first corpus with a `float` in it |
| `freeze` | LLVM's own optimiser, on an integer program | widening ARCH-01 to the whole corpus |
| `atomicrmw` | ARM64 LSE, whose `ldadd` writes the old value to a register | ctest-linux 292 |
| `cmpxchg` | ARM64 `cas` | the same |

All four end in `LLVMInstructionConverter::visitInstruction()`, which calls
`FAIL()` and aborts the process, and all four are answerable from LLVM's own
headers with no corpus at all. `scripts/ci/check_ir2hll_opcodes.py` asks it.

An instruction reaches the expression converter unless `isInlinableInst()` or
`shouldBeConvertedAsInst()` keeps it out, so every opcode in LLVM's
`Instruction.def` has to be one of four things: it has a `visit<Class>` in the
converter; it is named in `isInlinableInst()`'s exclusion list; it is a
terminator, excluded by `isTerminator()`; or it is in the script's
`ACCOUNTED_ELSEWHERE` table with the reason it cannot arrive. There are six in
that last group -- `alloca` (excluded a layer up), `store` and `fence` (void
result), and the three exception-handling pads, which no translator emits and
no machine code carries.

Today, on LLVM 20, that is 43 + 7 + 11 + 6 = 67, the whole list.

Run against the tree that broke ctest-linux 292 it names `AtomicRMW` and
`AtomicCmpXchg`. Run against `894733d`, the initial import, it names all four:

```
IR2HLL-01: FAIL these opcodes reach visitInstruction(), which aborts:
           FNeg (UnaryOperator)
           AtomicCmpXchg (AtomicCmpXchgInst)
           AtomicRMW (AtomicRMWInst)
           Freeze (FreezeInst)
```

The self-test neuters one accounted-for opcode at a time -- deleting a
converter case, and dropping an entry from the exclusion list -- and requires a
failure for each, then requires the real tree to pass. Without that last part a
checker whose parse silently broke would report "OK" on everything.

It reads the LLVM the product is built with, not whatever the runner has
installed: `build/linux/external/src/llvm-project/llvm/include/llvm/IR/Instruction.def`
in CI, and whatever `llvm-config` points at locally. An LLVM upgrade that adds
an instruction is exactly the case this is for, and asking a different LLVM
than the one being compiled against would answer a different question.


## Where this lands

ctest-linux 293 at `811319d`, every step green:

```
ARCH-01: x86_64    42/42   1.0000
ARCH-01: arm       42/42   1.0000
ARCH-01: arm64     42/42   1.0000
ARCH-01: mips      42/42   1.0000
ARCH-01: powerpc   42/42   1.0000
ARCH-01: overall  210/210  1.0000
```

Same 42 sources, same flags, same tools, all five architectures. That is the
question this branch was given, and it is now a floor rather than a reading:
all five are at `--arch-min 1.0`.

COV-01 on the same corpus, also a floor:

| arch    | rate   | what is left |
|---------|--------|--------------|
| x86_64  | 0.9931 | `HLT` in alignment padding |
| arm     | 1.0000 | nothing |
| arm64   | 0.9994 | `LD1`/`ST1` (NEON, 2 each) |
| mips    | 1.0000 | nothing |
| powerpc | 1.0000 | nothing |

### Prefetch hints are not instructions to model

`PLD`, `PLDW` and `PLI` on ARM, `PRFM`/`PRFUM` on ARM64 and `PREF` on MIPS
touch no register and no byte of memory and raise no addressing exception.
There is nothing to translate, which is the answer `NOP` and `BTI` already
get, and as `nullptr` entries they came out as `__asm_pld` calls -- 2,142 of
them across the 42 static ARM binaries, 924 `__asm_prfm` on ARM64 and 840
`__asm_pref` on MIPS. Noise in the output for instructions that do nothing.

Falsified one architecture at a time by reverting the dispatch entries: each
time, exactly the new tests for that architecture fail.

C2L-01 floors: Arm 592 -> 596, Arm64 481 -> 482. 4,650 tests.


## The PowerPC flake was a silent miscompilation

`PPC_INS_MR/CS_MODE_64` failed once in a C2L-01 gate run and was recorded above
as "one failure that has not come back", with a note that the thing to look at
if it returned was `GenericValue`'s default constructor. It came back on the
next gate run, so it got looked at properly.

It reproduces. Not in isolation -- 120 runs of that one test under four
competing CPU hogs, all clean -- but in the PowerPC suite under the same load
it failed on run 14 of 25. The difference is what the process had allocated
before it, which is the shape of an uninitialised read.

The failing run's IR says what happened:

```
  4660 :   %0 = load i64, ptr @r11
  4660 :   store i64 %0, ptr @r0
     0 :   %1 = icmp slt i64 %0, 0
     0 :   store i1 %1, ptr @cr0_lt
     1 :   %2 = icmp sgt i64 %0, 0
     ...
```

`mr` is not a record form and sets no flags. `storeCr0()` runs only when
`pi->update_cr0` is set, so capstone had reported this `mr` as `mr.`.

### Where it comes from

```c
void PPC_post_printer(csh ud, cs_insn *insn, char *insn_asm, MCInst *mci)
{
	...
	if (strrchr(insn->mnemonic, '.') != NULL) {
		insn->detail->ppc.update_cr0 = true;
	}
}
```

The post-printer reads `insn->mnemonic`. `fill_insn()` calls it *before* it
copies the mnemonic in -- the line above the copy in capstone's own source is a
commented-out `// memset(mnem, 0, CS_MNEMONIC_SIZE);`. So the field it reads is
whatever was in the buffer beforehand.

With `cs_disasm()`'s array that is the previous instruction's mnemonic, which
is capstone's bug and is at least deterministic. `capstone2llvmir` does not use
that: it calls `cs_malloc()` per instruction so that every `cs_insn` can be
kept, and `cs_malloc()` sets only `detail` -- `mnemonic` is uninitialised heap.

Demonstrated directly, same four bytes, same handle, three buffers:

```
trial 0 (cleared):        mnem='mr' ops='r0, r11' update_cr0=0 bh=0
trial 1 ("addc." in it):  mnem='mr' ops='r0, r11' update_cr0=1 bh=0
trial 2 ("bdnzt+" in it): mnem='mr' ops='r0, r11' update_cr0=0 bh=1
```

So this was never a test problem. Any PowerPC instruction RetDec decodes can
be given a CR0 update it does not perform, decided by the allocator, and the
conditional branches that read `cr0_lt`/`cr0_gt`/`cr0_eq` downstream read it.
That is a wrong answer rather than a crash, which is the worse kind, and it
predates this branch entirely.

Capstone reads the mnemonic in exactly one architecture's post-printer --
PowerPC's, three times, for `'+'`, `'-'` and `'.'`. The other six read only the
`insn_asm` buffer they are handed.

### The fix, and the test

`cs_malloc()` is wrapped: the two strings capstone reads before it writes them
are cleared. It belongs in this repository rather than in the pinned capstone,
and it is two assignments.

The test primes the hazard rather than waiting for it. It assembles first, so
nothing else allocates in between, then frees a chunk of exactly
`sizeof(cs_insn)` carrying `"addc."` at the mnemonic offset; glibc's tcache is
LIFO per size class, so `cs_malloc()`'s next allocation of that size gets it
back. Falsified by removing the two assignments: the new test fails, and
nothing else does.

C2L-01 floor: Powerpc 880 -> 882. 4,652 tests.

### What the first write-up got wrong

Recording it as "did not reproduce, here is where to look if it returns" was
the wrong call. Twelve clean runs of the whole suite is not evidence of
absence when the failing condition is heap contents: the runs that reproduce
it are the ones under load, and I had been running it on an idle machine. The
second occurrence is what made the difference, and it should not have taken
one.


## IR2HLL-01 found one on its first CI run, and one in itself

ctest-linux 294 was green on everything except the gate added the commit
before, which is the gate doing its job: it reads the LLVM the product is
compiled against, LLVM 23.1.0, and the local run had read the system LLVM 20.

```
IR2HLL-01: FAIL these opcodes reach visitInstruction(), which aborts:
           FNeg (FPUnaryOperator)
           FAdd (FPBinaryOperator)
           FSub (FPBinaryOperator)
           FMul (FPBinaryOperator)
           FDiv (FPBinaryOperator)
           FRem (FPBinaryOperator)
           PtrToAddr (PtrToAddrInst)
```

Six of those seven are the checker's fault and one is real.

**The six.** LLVM 23 split `FNeg` out of `UnaryOperator` into `FPUnaryOperator`
and the five floating-point arithmetic opcodes out of `BinaryOperator` into
`FPBinaryOperator`. `InstVisitor` generates a visit method per class and each
one falls through to its base's:

```cpp
RetTy visitFPBinaryOperator(FPBinaryOperator &I) { DELEGATE(BinaryOperator); }
```

so a converter defining `visitBinaryOperator` covers `FPBinaryOperator` as
well, and the six were never uncovered. The checker was matching class names
without following that chain. It reads the chain out of `InstVisitor.h` now
rather than hard-coding it, because which classes exist and what they delegate
to is precisely what moves between LLVM releases.

**The one.** `ptrtoaddr` is new in LLVM 22: the address part of a pointer,
split out of `ptrtoint` for targets with fat pointers, where the capability
metadata is dropped. `InstVisitor` delegates it to `visitCastInst`, and this
converter names the specific casts rather than `CastInst`, so it reached
`visitInstruction()` and would have aborted -- the fifth instance of that
shape, after `fneg`, `freeze`, `atomicrmw` and `cmpxchg`, and the first one
found before a binary reached it rather than after.

There is nothing in BIR to drop, so `visitPtrToAddrInst` is the same cast as
`ptrtoint`, behind `#if LLVM_VERSION_MAJOR >= 22` because the class does not
exist in the LLVM the standalone suites build against.

### A local gate weaker than the CI one

The checker searched `llvm-config` first and the vendored build tree last, so
a local run answered for LLVM 20 while CI answered for LLVM 23 -- and LLVM 20
does not have `ptrtoaddr` at all. That is the same shape as the push gate that
ran `--self-test` where CI ran the real check, and as the workdir guard that
trusted an unstamped directory: a checker more permissive than the thing it
stands in for.

It looks in the vendored tree first now. Where there is no build tree, as on a
fresh checkout, it falls back and the line it prints names which LLVM it asked,
so the weaker answer is visible rather than assumed:

```
IR2HLL-01: 67 opcodes in /usr/lib/llvm-20/include
IR2HLL-01: 69 opcodes in build/linux/external/src/llvm-project/llvm/include
```

Falsified against both: removing `visitPtrToAddrInst` fails the LLVM 23 check
by name and leaves the LLVM 20 check passing, which is correct -- the opcode
is not in LLVM 20.


## The last two on ARM64: LD1 and ST1

COV-01's arm64 row had two entries left, `LD1` and `ST1`, two occurrences
each, and the note above said NEON. That is true of the family and not of
these two forms.

`ld1 {v0.16b}, [x0]` is a plain 128-bit load. `ld1 {v0.8b}, [x1]` is a 64-bit
one that zeroes the top half of the register, which is what every D-form write
does. A list moves consecutive registers to or from consecutive addresses.
None of that needs a lane model, and that is the whole reason these two can be
translated while the rest of NEON stays where it is: `V0`..`V31` are `i128`
globals in this translator, so the arrangement decides only the total width.
`ld1 {v0.4s}, [x0]` and `ld1 {v0.16b}, [x0]` move the same 128 bits.

Deliberately left on the pseudo-asm path: writeback forms (`[x0], #16` has to
update the base register), lane forms (`ld1 {v0.s}[2], [x0]`, which the width
helper rejects by returning zero), and LD2/LD3/LD4 and their stores, which
de-interleave rather than copy.

Four tests. The interesting two are the D-form pair: the load starts with
`0xdeadbeefdeadbeef` in the upper half of `V0` so that failing to zero it is
visible, and the list store checks that the second register lands eight bytes
on rather than at the same address or sixteen.

Falsified by forcing the access width to 128 bits and the list offset to zero.
The suite does not pass: it aborts, in `APInt::getZExtValue()`, because a
128-bit store then lands where the harness reads 64 bits. A blunt signal, but
an unambiguous one -- the mutation does not survive.

### Where COV-01 ends up

```
arch         decoded   skipped    covered     rate  uncovered-kinds
x86_64          6045         0       6003   0.9931  1
arm             8060         0       8060   1.0000  0  (3808 bytes mapped as data)
arm64           6742         0       6742   1.0000  0
mips            8519         5       8519   1.0000  0
powerpc         8904         0       8904   1.0000  0
```

Four of the five translate every instruction their 42 binaries contain. The
fifth is x86-64, and what it has left is `HLT` in the alignment padding after
`_start` -- code that never runs. It stays uncovered rather than being pointed
at a translator that emits the pseudo-asm call it already emits.

The branch started with x86-64 as the only architecture anything measured, and
ends with x86-64 as the only one below 1.0.


## x86-64 is the one below 1.0, and the static corpus says why

COV-01 over the 210-binary dynamic corpus has x86-64 at 0.9931 with one
uncovered kind. That is a true number about those binaries and a misleading
one about the translator, because a dynamically linked `hello world` contains
almost nothing: the interesting code lives in `libc.so.6`, which the corpus
binaries call rather than contain.

`scripts/build_multiarch_corpus.sh --link static` builds the same 210 sources
with glibc linked in. Five million instructions instead of thirty thousand,
and a very different answer:

```
arch         decoded   skipped    covered     rate  uncovered-kinds
x86_64       5230330         0    4940904   0.9447  84
arm                                         0.9943
arm64                                       0.9840
mips                                        0.9954
powerpc                                     0.9980
```

(The four non-x86 rows are the rates from the same static build; only the
x86-64 row is reproduced in full here because it is the one this section is
about. ARM's is a lower bound with a wide error bar for the reason COV-01's
own header gives.)

x86-64 is the worst of the five by a wide margin, and the 289,426 instructions
it does not translate are not exotica. They are what glibc's string, memory and
maths routines are made of.

### What the gap is made of

The top of the uncovered list, by occurrences over 5,230,330 decoded:

```
X86_INS_VPCMPEQB     27914  0.53%      X86_INS_PCMPGTB       8600  0.16%
X86_INS_VMOVDQU      24208  0.46%      X86_INS_VMOVDQA       8348  0.16%
X86_INS_PMOVMSKB     21102  0.40%      X86_INS_VPANDN        8274  0.16%
X86_INS_VPMOVMSKB    19946  0.38%      X86_INS_VMOVUPS       7266  0.14%
X86_INS_VMOVDQU64    15913  0.30%      X86_INS_PCMPISTRI     6393  0.12%
X86_INS_TZCNT        11662  0.22%      X86_INS_VPMINUB       5730  0.11%
X86_INS_PALIGNR      10740  0.21%      X86_INS_PREFETCHT0    5544  0.11%
X86_INS_KMOVD        10519  0.20%      X86_INS_PUNPCKLQDQ    5004  0.10%
X86_INS_VPADDB        9890  0.19%      X86_INS_SYSCALL       4627  0.09%
X86_INS_VZEROUPPER    9336  0.18%      X86_INS_MOVNTPS       3360  0.06%
```

Roughly two thirds of it is AVX and AVX-512 -- every `V`-prefixed entry, plus
the `K` mask-register instructions. Those are not a table-entry problem. YMM0
and ZMM0 do not exist in this translator's register file at all, so there is
nothing for a `VMOVDQU` translator to write to; adding them is a subsystem,
with its own aliasing rules against XMM, and it is recorded here rather than
attempted.

The other third is SSE2 and BMI1 -- 128-bit and general-purpose, on registers
this translator already has. That part is a table-entry problem, and this
commit is the first batch of it.

### Batch A

| instruction | occurrences | now |
| --- | --- | --- |
| `PMOVMSKB` | 21,102 | `translateSseMovMsk` |
| `TZCNT` | 11,662 | `translateBitCount` |
| `PREFETCHT0` | 5,544 | `translateNop` |
| `MOVNTPS` | 3,360 | `translateSseMovWhole` |
| `PREFETCHT1` | 2,688 | `translateNop` |
| `MOVNTDQ` | 1,008 | `translateSseMovWhole` |
| `LZCNT`, `POPCNT` | below the cut | `translateBitCount` |
| `PREFETCH`, `PREFETCHNTA`, `PREFETCHT2`, `PREFETCHW` | below the cut | `translateNop` |
| `MOVNTDQA`, `MOVNTPD` | below the cut | `translateSseMovWhole` |
| `MOVNTI`, `MOVNTQ` | below the cut | `translateMov` |

**`PMOVMSKB`** is the single most frequent untranslated x86 instruction in the
corpus, and it is frequent for one reason: it is the second half of glibc's
SSE2 string routines. `PCMPEQB` compares sixteen bytes at a time, `PMOVMSKB`
asks which of those sixteen matched, and `TEST`/`Jcc` branch on the answer.
`PCMPEQB` was already translated, so the comparison was being computed and
then discarded at an `__asm_pmovmskb` call, and the branch after it read a
value from nowhere. It is the same sign-bit gather as `MOVMSKPS`, over sixteen
byte lanes instead of four dword ones, so it is four lines in the translator
that was already there.

**`TZCNT` and `LZCNT` are not `BSF` and `BSR`.** They share an encoding but
for the `F3` prefix, and on a CPU without BMI1 a `TZCNT` decodes and executes
as `BSF` -- which is the whole reason the encoding was chosen, and also the
whole reason translating one as the other is wrong. For a **zero source** they
disagree completely:

```
        src == 0            BSF                  TZCNT
        destination         unmodified           operand width (32 or 64)
        reported through    ZF = 1               CF = 1
```

A compiler emits `TZCNT` precisely so that it does not have to branch around
the zero case. `llvm.cttz`/`llvm.ctlz` with `is_zero_poison=false` have exactly
the defined behaviour, and that is why the second argument is `false` here and
`true` in `translateBsf`.

`POPCNT` is the odd one of the three on flags: `ZF` from the **source**, and
`CF`, `OF`, `SF`, `AF` and `PF` architecturally **cleared** rather than
undefined, so they are written. `TZCNT` and `LZCNT` leave those four alone,
because "undefined" is not a value worth asserting.

**The prefetches** are the fifth instance in this branch of the same shape,
after ARM's `PLD`/`PLDW`/`PLI`, ARM64's `PRFM`/`PRFUM` and MIPS's `PREF`: an
instruction that names an address, does not read it, writes nothing and cannot
fault. As `nullptr` entries they came out as `__asm_prefetcht0` calls with a
memory operand, which reads as a side effect the instruction does not have.

**The non-temporal moves** differ from their ordinary counterparts only in
cache allocation policy, which has no architecturally visible effect on the
value moved. `MOVNTPS`, `MOVNTPD`, `MOVNTDQ` and `MOVNTDQA` are the 128-bit
move `translateSseMovWhole` already is; `MOVNTI` and `MOVNTQ` are the plain
store `translateMov` already is.

### Deliberately not in Batch A

`SYSCALL`, 4,627 occurrences, stays on the pseudo-asm path and should. It is
an opaque, side-effecting transfer to the kernel that clobbers `RAX`, `RCX`
and `R11` and can do anything at all to memory; `__asm_syscall()` is a more
honest model of that than any sequence of loads and stores would be. Same for
`HLT` and `PCMPISTRI`.

`MOVNTSS` and `MOVNTSD` (AMD SSE4a) are left alone: zero occurrences, and the
only way to reach them shares a width calculation with `MOVD`/`MOVQ`, so the
change would perturb two translated instructions to reach two untranslated
ones that nothing emits.

### What the falsification could and could not show

Four mutations, each reverted alone, each rebuilt and run:

| mutation | result |
| --- | --- |
| `TZCNT`/`LZCNT` dispatched to `translateBsf` | the five zero-source and count tests fail |
| `PMOVMSKB` treated as `MOVMSKPS` (four 32-bit lanes) | both `PMOVMSKB` tests fail |
| prefetch and non-temporal entries back to `nullptr` | those six tests fail |
| `POPCNT` no longer clearing the five flags | the three `POPCNT` tests fail |

One mutation did **not** fail, and it is worth recording rather than quietly
dropping. Flipping `is_zero_poison` from `false` to `true` -- the difference
between "the answer for a zero input is the width" and "the answer for a zero
input is poison" -- leaves the whole suite green, because
`tests/llvmir-emul` implements `llvm.cttz` as `APInt::countr_zero()` and
ignores the second argument entirely:

```cpp
else if (id == Intrinsic::cttz)
    dest.IntVal = APInt(bw, src.IntVal.countr_zero());
```

So that argument is correct on the instruction manual's authority and not on
this suite's, and the honest thing is to say so. It matters against a real
LLVM -- `is_zero_poison=true` tells the optimiser the source is never zero,
which for `TZCNT` is a lie and would license folding away the `CF` computation
that the zero case exists to report -- and nothing here can demonstrate it.
A test that cannot fail proves nothing; a test that cannot be written should
be admitted to, not implied.

### Where it leaves the static number

```
                 decoded   skipped    covered     rate  uncovered-kinds
before          5230330         0    4940904   0.9447  84
after           5230330         0    4986268   0.9533  78
```

45,364 instructions, 0.86 points, six kinds. The measurement is in CI from
this commit -- `COV-01 instruction coverage (static corpus, x86-64)` -- and
unfloored on its first run for the reason the step's own comment gives: the
rate depends on which glibc the runner's cross toolchains link in, and a floor
measured here and asserted about there is the mistake that let the local
IR2HLL-01 gate read LLVM 20 while CI read LLVM 23. It is floored next commit
at whatever number CI itself reports.

### 182 instruction ids are not in the x86 table at all

While measuring this, COV-01 reported five uncovered kinds as `NO ENTRY`
rather than `listed, null`: `INCSSPQ` (630), `RDSSPQ` (378), `VPTERNLOGD`
(3,550), `VPTESTMB` (2,981) and `VPTESTNMB` (717). They are not in
`x86_init.cpp`'s table under any spelling. Checking the whole enum: of the
1,523 instruction ids in the pinned Capstone 5.0.9, **182 have no row**.

This is not a defect. The dispatch is

```cpp
auto fIt = _i2fm.find(i->id);
if (fIt != _i2fm.end() && fIt->second != nullptr)
```

so a missing id and a `nullptr` row behave identically -- both fall through to
the pseudo-asm path. It is a staleness signal: the table was written against
an older Capstone and instructions added since (CET shadow stack, AVX-512
VBMI, GFNI, the `K*` mask ops) were never listed. Recorded because the *next*
time someone reads "listed, null" as "everything is accounted for", these 182
are the counterexample.


## Batch B: the packed integer instructions glibc actually uses

After Batch A the static-corpus x86-64 rate is 0.9533 with 78 uncovered kinds,
and the top of what is left is still recognisable: `PALIGNR` 10,740,
`PCMPGTB` 8,600, `PUNPCKLQDQ` 5,004, `PMINUB` 3,168. All 128-bit, all on
registers this translator already has.

| instruction | occurrences | now |
| --- | --- | --- |
| `PALIGNR` | 10,740 | `translateSsePalignr` |
| `PCMPGTB`/`W`/`D`/`Q` | 8,600+ | `translateSsePcmpeq` |
| `PUNPCKLQDQ`, `PUNPCKHQDQ`, `PUNPCKLWD`, `PUNPCKH{BW,WD,DQ}` | 5,634+ | `translateSsePunpckl` |
| `PMIN`/`PMAX`, signed and unsigned, B/W/D | 3,546+ | `translateSsePminMax` |
| `PCMPEQQ` | — | `translateSsePcmpeq` |
| `PAVGB`, `PAVGW` | — | `translateSsePavg` |

Every one of these is decided by a property the mnemonic spells out and the IR
does not, and each of the three properties had a way of going wrong that was
already sitting in this file:

**Signedness.** `PCMPGT` is a **signed** comparison and `PMINU`/`PMAXU` are
unsigned ones. The two readings disagree on every lane whose top bit is set,
which in the code these appear in -- the output of a `PCMPEQB`, where a match
is `0xff` -- is all the interesting ones. So the tests use `0xff` against
`0x01` and `0x80` against `0x0f`, not values that happen to agree.

**Lane width.** `translateSsePcmpeq` ended its width switch with
`default: bits = 32`. That was harmless while exactly three ids reached it and
would have silently compared 32-bit lanes the moment `PCMPEQQ` -- sitting
`nullptr` two lines away in the table -- was pointed at it. The switch is total
now and throws on an id it does not know.

`translateSsePunpckl` had the same shape in a shorter spelling:

```cpp
unsigned bits = (i->id == X86_INS_PUNPCKLBW) ? 8 : 32;
```

correct for the two ids that reached it, `PUNPCKLBW` and `PUNPCKLDQ`, and
wrong for `PUNPCKLWD`, which was `nullptr` directly below them in the table.
Wiring it would have interleaved 32-bit lanes under a 16-bit mnemonic: no
crash, no assertion, a different answer. This is the same defect as the ARM
`ARM_INS_NOP`/`ARM_INS_HINT` and PowerPC `update_cr0` findings earlier in this
branch -- a table whose correctness depends on which entries happen to be
`nullptr`.

**Which half.** `PUNPCKH` takes the top half of each operand rather than the
bottom. Same shuffle, source indices moved up by half the lane count.

`PALIGNR` is the one with an arithmetic trap rather than a naming one. It
concatenates destination and source into 256 bits and takes a 128-bit window
`imm` bytes up. Written as one i256 shift it reads better; written as i128
operations it stays inside what `tests/llvmir-emul` executes, and the three
cases have to be separated in C++ anyway, because `imm == 0` and `imm == 16`
are exactly the two values that would make an LLVM shift equal to its
operand's width, which is poison. `imm >= 32` is architecturally zero.

`PAVG` rounds half up and adds **one bit wider than the lane**. At the lane's
own width `0xff + 0x02 + 1` wraps to `0x02` and the answer comes out `0x01`
instead of `0x81`. The widening is not a precaution, it is the instruction, so
the test uses inputs whose sum carries out of the lane.

### The emulator change that turned out to be dead

`visitBinaryOperator()`'s vector branch lists add, sub, mul, the four division
and remainder forms, and, or and xor -- and not `shl`, `lshr` or `ashr`, which
would fall into its `default:` and `llvm_unreachable()`. `PAVG` emits a vector
`lshr`, so the three cases were added.

The falsification run said they were not needed: removing them again left the
suite green. `InstVisitor` dispatches `Shl`, `LShr` and `AShr` to
`visitShl()`, `visitLShr()` and `visitAShr()`, each of which has its own
vector branch, so `visitBinaryOperator()` never sees them and the missing
cases are unreachable.

The cases were reverted and a comment left in their place saying why they are
absent. The four tests stay: the vector branches of the three shift visitors
had no test either way, and the eight packed-shift instructions
`PSLLW`..`PSRLQ` will depend on them.

This is the second time in two commits that a falsification run came back
green and the right reading was "check the mutation" rather than "report the
result" -- the first being the `PMOVMSKB` sed that matched the wrong
indentation. The difference is that this one was the code being wrong, not the
mutation.

### Falsification

Eight mutations, each reverted alone, each rebuilt and run:

| mutation | result |
| --- | --- |
| `PCMPGT` compares unsigned | 2 tests fail |
| `PCMPEQQ` at 32-bit lanes | 1 test fails |
| `PUNPCKLWD` at 32-bit lanes | 1 test fails |
| `PUNPCKH` takes the low half | 2 tests fail |
| `PMIN`/`PMAX` signedness flipped | 2 tests fail |
| `PALIGNR` shifts by bits, not bytes | 1 test fails |
| `PAVG` adds at the lane's own width | 2 tests fail |
| emulator vector shift cases removed | **suite stays green** -- see above |

### Where it leaves the static number

```
                 decoded   skipped    covered     rate  uncovered-kinds
before Batch A  5230330         0    4940904   0.9447  84
after Batch A   5230330         0    4986268   0.9533  78
after Batch B   5230330         0    5015166   0.9589  68
```

74,262 instructions and sixteen kinds across the two. What is left above 0.01%
is, with three exceptions, AVX and AVX-512: `VPCMPEQB`, `VMOVDQU`,
`VPMOVMSKB`, `VMOVDQU64`, `KMOVD`, `VPADDB`, `VZEROUPPER` and the rest of the
`V`- and `K`-prefixed list, which need YMM and ZMM in the register file.

The three exceptions are deliberate, not pending: `PCMPISTRI` (6,393),
`SYSCALL` (4,627) and `HLT` (294). The first is a string comparison whose
result is an index computed from an aggregate of sixteen lane comparisons
under a four-way mode immediate -- translatable, but a subsystem rather than a
table entry, and modelling it wrongly is worse than not modelling it. The
other two are opaque by nature.


## Batch C: what is left of the gap that is not AVX

After Batch B the static corpus reads 0.9589 with 68 uncovered kinds. Sorted
by occurrences, everything above 0.01% is `V`- or `K`-prefixed except three
deliberate exceptions -- and a tail of general-purpose instructions that is
short enough to finish.

| instruction | occurrences | now |
| --- | --- | --- |
| `BLSMSK` | 1,008 | `translateBls` |
| `SHRX` | 807 | `translateShiftX` |
| `SARX` | 756 | `translateShiftX` |
| `MOVBE` | 672 | `translateMovbe` |
| `BZHI` | 633 | `translateBzhi` |
| `SHLX` | 381 | `translateShiftX` |
| `PAUSE` | 168 | `translateNop` |
| `WAIT` | 126 | `translateNop` |
| `ANDN` | 84 | `translateAndn` |
| `BLSR` | 84 | `translateBls` |
| `BEXTR`, `BLSI`, `RORX` | below the cut | as above |

**`ANDN` complements the first source, not the second.** `dst = ~src1 & src2`.
Two files over, `PANDN` is `~dst & src`. Both spellings look right and they
disagree on every input pair that is not symmetric.

**`BLS*`'s carry flag does not mean the same thing three times.** `BLSI` sets
`CF` when the source is **not** zero; `BLSMSK` and `BLSR` set it when the
source **is**. There is exactly one input -- zero -- on which the three
disagree, so the tests use it.

For `BLSMSK` the manual clears `ZF` outright rather than computing it, on the
argument that the result cannot be zero (all ones for a zero source, at least
1 otherwise). Computing it from the result gives the same answer for every
input and does not depend on that argument staying true.

**`BEXTR` has two shift amounts that come from a register**, and either can
exceed the operand width, where an LLVM shift is poison rather than zero. Both
are guarded by a select: past the top the extracted value is zero, and a
length at or beyond the width is the full mask -- which `(1 << len) - 1`
cannot produce without overflowing. Clamping the amount instead would answer
for a different instruction.

**`BZHI`'s `CF` is not `BLSR`'s question.** It reports that the index was at or
past the operand width, in which case nothing is zeroed.

**The BMI2 shifts do not touch the flags at all**, and that is the entire
reason a compiler emits them: the shift can be scheduled across a comparison.
Writing `SF`/`ZF`/`CF` in `translateShiftX` would be a wrong answer of the kind
that shows up several instructions later, at the `Jcc` that reads a flag this
instruction was chosen for not disturbing. The test for it is
`EXPECT_JUST_REGISTERS_STORED({{X86_REG_EAX, ...}})` -- it fails if anything
else was written.

### The second thing the emulator cannot show

`translateShiftX` masks the count to the operand width, because the hardware
does and because `shl i32 %x, 36` is poison. Removing that mask leaves the
suite green.

```cpp
unsigned getShiftAmount(uint64_t orgShiftAmount, llvm::APInt valueToShift)
{
    unsigned valueWidth = valueToShift.getBitWidth();
    if (orgShiftAmount < static_cast<uint64_t>(valueWidth)) return orgShiftAmount;
    // according to the llvm documentation, if orgShiftAmount > valueWidth,
    // the result is undfeined. but we do shift by this rule:
    return (NextPowerOf2(valueWidth-1) - 1) & orgShiftAmount;
}
```

For a 32-bit value that is `& 31` -- the same mask x86 applies -- so the
emulator masks whether the translator did or not. The mask stays, because what
ships is the IR and not the emulator's reading of it; the test stays, because
it pins the architectural answer; and the test's comment says it cannot
falsify the mask, which is the part that would otherwise be quietly implied.

Second instance after `llvm.cttz`'s `is_zero_poison`. Both are the emulator
being *more forgiving* than LLVM, which is the safe direction for a test
harness to be wrong in and the useless one for falsification.

### Falsification

Thirteen mutations, each reverted alone, each rebuilt and run. Twelve fail:

| mutation | result |
| --- | --- |
| `ANDN` complements the wrong source | 2 tests fail |
| `BLSI`'s `CF` direction flipped | 2 tests fail |
| `BLSMSK` uses `AND` rather than `XOR` | 1 test fails |
| `BEXTR`'s start and len swapped | 3 tests fail |
| `BEXTR`'s start guard removed | 1 test fails |
| `BEXTR`'s length guard removed | 1 test fails |
| `BZHI`'s `CF` direction flipped | 2 tests fail |
| `SARX` shifts in zeroes | 1 test fails |
| the BMI2 shifts write flags | 6 tests fail |
| `RORX` shifts instead of rotating | 1 test fails |
| `MOVBE` does not swap | 1 test fails |
| `PAUSE`/`WAIT` back to `nullptr` | 1 test fails |
| the shift count is not masked | **suite stays green** -- see above |

### Deliberately not in Batch C

`MULX`, `PDEP` and `PEXT` have zero occurrences in the corpus. `MULX` takes
`EDX`/`RDX` as an implicit operand, and guessing how Capstone reports an
implicit operand rather than checking is how a silent miscompilation starts;
`PDEP` and `PEXT` are bit-by-bit loops. `BLSIC` and the rest of AMD's TBM have
zero occurrences and no compiler that emits them.

### Where the three batches leave the static number

```
                 decoded   skipped    covered     rate  uncovered-kinds
before Batch A  5230330         0    4940904   0.9447  84
after Batch A   5230330         0    4986268   0.9533  78
after Batch B   5230330         0    5015166   0.9589  68
after Batch C   5230330         0    5019885   0.9598  58
```

78,981 instructions and twenty-six kinds. What is left is AVX, AVX-512, and
`PCMPISTRI`, `SYSCALL` and `HLT`, which are recorded above as deliberate.

### The local and CI numbers are not measured over the same bytes

ctest-linux 298 ran the new static step and reported

```
arch         decoded   skipped    covered     rate  uncovered-kinds
x86_64       5230246         0    4986184   0.9533  78
```

against 5,230,330 decoded here: 84 instructions apart, because the runner's
glibc is not byte-identical to this container's. The rate agrees to four
decimal places and the uncovered-kind count agrees exactly, which is the
evidence that made it safe to floor -- and the 84-instruction difference is
the evidence that made it right to measure in CI first rather than assert a
locally-measured floor about a machine that was never asked.


## COV-01 was counting 17,340 zero words as ARM64 instructions

Measuring the four non-x86 architectures over the static corpus put arm64 at
0.9840 -- the worst of the four -- and the single largest entry in its
uncovered list was an id the dispatch table does not list at all:

```
<id 1155>                    17340   0.48%  NO ENTRY
```

Capstone says that id is `udf`. AArch64's `udf` is the encoding whose top
sixteen bits are zero, so any all-zero word decodes as one.

GNU objdump finds no `udf` in these binaries and no all-zero word in `.text`
of the one I first looked at. Scanning all forty-two: seventeen of them have
exactly 1,020 all-zero words each, and in `bubblesort-arm64-gcc-O0` they are
one contiguous run of 4,080 bytes at `0x40f378`, immediately after
`_nl_cleanup_ctype`, covered by no `FUNC` symbol -- and inside an `$x` region,
so glibc has marked 4 KB of zero padding as **code**.

COV-01's data filter is the `$d` mapping symbols, and that is the right
mechanism when the producer marks its data. Here the producer did not.

### The fix, and why it is not the fix that moves the number

The answer is the same one x86-64's `HLT`-in-alignment-padding got: a rate is
only worth having if its denominator is instructions the program can execute,
so stop counting the padding, rather than pointing a translator at it.

`split_on_zero_runs()` splits each code region around runs of 64 or more zero
bytes. The threshold is sixteen AArch64 instructions or thirty-two x86
`add byte ptr [rax], al`, and no compiler emits either as a reachable
sequence. The excluded bytes are reported in **their own column**, not folded
into `skipped`:

```
arch         decoded   skipped     zeros    covered     rate  uncovered-kinds
x86_64       5230330         0         0    5019885   0.9598  58
arm          3398752         0         0    3379355   0.9943  20
arm64        3575624        42     69360    3542591   0.9908  47
mips         4292791        89         0    4273234   0.9954   3
powerpc      4640553      9561         0    4631149   0.9980  23
```

"Capstone could not decode this" and "this was not code" are different facts,
and a rule that quietly makes a rate better is the shape this branch has been
finding all along. Separate column, eight self-test cases pinning the
boundary (64 bytes dropped, 63 kept, leading, trailing, absolute offsets), and
the rule changes nothing on the **gated** dynamic corpus -- all five read
`zeros 0` there, so the existing CI floors are untouched.

Falsified by disabling the split: arm64 reads 0.9860 instead of 0.9908, 48
uncovered kinds instead of 47, and `udf` returns to the top of the list with
17,340 occurrences.

## ARM64: EXT, the bitwise selects, and the lane compares

With the padding out of the denominator, arm64's uncovered list is real:

| instruction | occurrences | now |
| --- | --- | --- |
| `EXT` | 5,124 | `translateNeonExt` |
| `CMEQ` | 1,512 | `translateNeonCmp` |
| `CMHS` | 294 | `translateNeonCmp` |
| `BIT` | 210 | `translateNeonBitSel` |
| `CMGE` | 42 | `translateNeonCmp` |
| `CMGT`, `CMHI`, `CMTST`, `BSL`, `BIF` | below the cut | as above |

**`EXT` is `PALIGNR` with the operands the other way round.** x86 makes the
destination the high half of the concatenation; AArch64 makes the second
source the high half. A translation copied across from the one written three
commits ago answers with the halves swapped for every index except zero, which
is why the test checks that byte 0 of the result is byte 4 of `vn` and byte 15
is byte 3 of `vm`.

It is done at the arrangement's own width rather than always at 128 bits: for
`.8b` the operation is 64-bit and the write zeroes bits 127:64, which is what
every D-form write does. The shifts are split by case in C++ for the same
reason `PALIGNR`'s are -- index zero would otherwise be a shift by exactly the
operand width. `index >= width` is architecturally UNDEFINED rather than zero,
so it goes to the pseudo-asm path instead of being given an answer.

**The three bitwise selects differ only in which register is the selector**,
and they are the one family of NEON data-processing instructions that needs no
lane model at all:

```
BSL vd, vn, vm    vd = (vn & vd) | (vm & ~vd)   the destination selects
BIT vd, vn, vm    vd = (vd & ~vm) | (vn & vm)   the second source masks
BIF vd, vn, vm    vd = (vd & vm) | (vn & ~vm)   the same, inverted
```

The three tests use identical operands and differ only in the mnemonic, so
each one is pinned by an answer the other two do not produce.

**The lane compares are one letter apart and half of them are signed.** `CMGE`
and `CMGT` are signed; `CMHS` and `CMHI` are the unsigned pair. The two
readings disagree on every lane with its top bit set, which for the byte lanes
a NEON string routine works on is all the interesting ones -- so the `CMHS`
and `CMGE` tests use identical operands (`0xff` against `0x01`) and expect
different answers. `CMTST` is not a comparison at all: `(vn & vm) != 0` per
lane.

`cmeq vd.<T>, vn.<T>, #0` -- compare against an immediate zero -- is the form
that matters, because it is how a NEON string routine asks which of sixteen
bytes is the terminator.

### Falsification

Eight mutations, each reverted alone, each rebuilt and run:

| mutation | result |
| --- | --- |
| COV-01's zero-run split disabled | arm64 reads 0.9860, `udf` leads the list |
| `EXT`'s two sources swapped | 3 tests fail |
| `EXT` always 128-bit | 1 test fails |
| `BIT` selects with the destination | 1 test fails |
| `CMHS`/`CMGE` signedness flipped | 3 tests fail |
| the compare's lane width forced to bytes | 1 test fails |
| `CMTST` made a comparison | 1 test fails |
| the ten dispatch entries back to `nullptr` | the suite aborts |

One mutation did not apply on its first attempt -- a multi-line Python string
inside a shell heredoc -- and the driver said so and skipped the run rather
than reporting a green suite. That guard exists because of the `PMOVMSKB`
mutation two commits ago that silently did not apply and was reported as a
result.

### What is left on ARM64, and why

`MRS`, 12,400 occurrences, is now the largest entry. 275 of the 293 in each
binary are `mrs xN, tpidr_el0` -- the TLS thread pointer -- and the rest are
`fpsr`, `fpcr`, `dczid_el0`, `ctr_el0` and `midr_el1`. Modelling `TPIDR_EL0`
as a register would be a real improvement: every TLS access currently ends at
an opaque `__asm_mrs()` call, and a modelled register would let the rest of
the pipeline propagate it. It is not done here because Capstone has no
`ARM64_REG_TPIDR_EL0` -- system registers are a separate namespace reported as
`ARM64_OP_SYS` -- so it needs a synthetic register id, and
`src/bin2llvmir/providers/abi/arm64.cpp` sizes its arrays with
`_id2regs.resize(ARM64_REG_ENDING, nullptr)`. An id past that end is an
out-of-bounds write, not a missing entry. That is a two-module change with a
memory-safety edge, and it is written down here rather than attempted between
two NEON batches.

> **This paragraph was wrong on both counts, and the section further down
> ("The thread pointer") is the correction.** `Abi::addRegister()` resizes its
> table for any id past the end, so there is no out-of-bounds write; and
> `arm64_init.cpp` already creates a global for every AArch64 system register
> Capstone knows, `tpidr_el0` among them, so ARM64 needed no new id at all --
> only the instruction that reads one. The `resize(ARM64_REG_ENDING)` line is
> real and was read correctly; what was not checked was the function that
> writes into the vector, eleven lines further down the same file. It is left
> here rather than edited away because a recorded blocker that turns out not
> to exist is worth being able to find again.

`SVC` (4,206) and `BRK` (339) are opaque by nature, like x86's `SYSCALL`.

`ST1B` (4,620), `LD1B` (2,688), `WHILELO` (588), `PTRUE` (168), `CNTB` and
`CNTD` are **SVE**, and `LDG` (1,302), `ST2G`, `STZG`, `STZ2G`, `IRG` and
`GMI` are **MTE**. Both need registers that do not exist in this translator --
Z and P for SVE, an allocation-tag model for MTE. Recorded, not attempted, for
the same reason AVX is: a subsystem, not a table entry.

`UMAXP` (882), `SHRN` (798), `UMINP`, `ADDP`, `ADDV`, `UZP1`, `SADDL`,
`UADDW`, `XTN`, `SHL`, `UMOV`, `CNT` and `MVNI` are ordinary NEON lane
operations that this register model can express. They are a next batch, not a
limitation.

### A weakness in COV-01 itself, recorded

COV-01 counts an instruction as covered when the dispatch table has a function
pointer for it. On ARM64 that is not the same as translating it:

```cpp
if (ifVectorGeneratePseudo(i, ai, irb))
{
    return;
}
```

`translateMovi` and several others begin with that line, so for a vector
operand they emit exactly the `__asm_movi` call a `nullptr` entry would have
emitted -- and COV-01 scores them covered. The number it reports is therefore
an upper bound on ARM64, and the honest metric would count `__asm_*` calls in
the decompiler's **output** rather than function pointers in its table.
ARCH-01 already runs the decompiler over this corpus, so the measurement is
available; it is the next thing to build, and until it exists the ARM64 rate
should be read as "has a function for", not "translates".


## PSEUDO-01: asking the translator instead of the table

COV-01 counts an instruction as covered when the dispatch table has a function
pointer for it. The last commit recorded that this is an upper bound and named
the mechanism -- `ifVectorGeneratePseudo()` returning early -- without
measuring it. This measures it.

`scripts/ci/pseudo_asm_probe.cpp` translates each instruction of a binary into
a throwaway function and asks `isPseudoAsmFunctionCall()`, the translator's own
predicate, whether anything it produced was a pseudo-assembly call. A fresh
function per instruction, because a translation may create basic blocks of its
own and counting within one block would miss a call placed in a new one.

Not every `__asm_*` call is a gap. The product declares two of them as intended
models -- a `rep stosq` modelled as `__asm_rep_stosq_memset` is a better answer
than a loop, and `__asm_hlt` is the honest model of a halt -- and that list is
read out of `isAsmIntrinsicName()` in `src/retdec/semantic_recovery_export.cpp`
rather than kept in the gate, for the reason IR2HLL-01 reads `InstVisitor.h`
rather than hard-coding its delegation chain. A gate with its own copy of the
product's list is a gate that will eventually excuse something the product no
longer excuses.

### What it found on the corpus COV-01 calls perfect

```
COV-01    x86_64 0.9931   arm 1.0000   arm64 1.0000   mips 1.0000   powerpc 1.0000
PSEUDO-01 x86_64 1.0000   arm 1.0000   arm64 0.9999   mips 0.9984   powerpc 0.9945
```

Both directions are informative.

x86-64 goes **up**: its 45 pseudo-assembly calls are the 42 `__asm_hlt` in
alignment padding and 3 `__asm_rep_stosq_memset`, and both are intended.
COV-01's 0.9931 counted the `HLT` against it.

Three of the four architectures COV-01 scores at 1.0000 go **down**:

```
arm64    __asm_movi                      1
mips     __asm_trunc.w.d                12   __asm_trunc.w.s 1   __asm_swc2 1
powerpc  __asm_rlwinm                   42   __asm_cror      6
```

Every one of those instructions has a function pointer in `_i2fm`. The function
emits a call.

### PowerPC's rotate-and-mask family

`rlwinm` at 42 occurrences was the most frequent unmodelled pseudo-assembly
call on any of the five architectures, and on the static corpus it is 28,341
with `rlwimi` at 7,795 and the record form `rlwinm.` at 2,206 -- 0.83% of
PowerPC. GCC emits `rlwinm` for every shift and every bitfield extract on
32-bit PowerPC: `rlwinm r3, r3, 0, 16, 31` is `(uint16_t) r3`, and it was
coming out of the decompiler as a call to an undefined function.

There is nothing in it that needs one:

```
rlwinm rA, rS, SH, MB, ME    rA = ROTL32(rS, SH) & MASK(MB, ME)
rlwnm  rA, rS, rB, MB, ME    the same, rotating by rB[27:31]
rlwimi rA, rS, SH, MB, ME    rA = (ROTL32(rS,SH) & MASK) | (rA & ~MASK)
```

Two things in that mask are easy to get backwards and neither fails loudly.
**PowerPC numbers bits from the most significant end**: bit 0 is `0x80000000`,
so `MB=0, ME=15` is the *high* half. And **`MB > ME` is not an error** -- the
mask wraps, covering bits MB..31 and 0..ME, which is how a rotate-and-mask
extracts a field that straddles the word boundary after rotation. A translation
that treated the wrap as empty would answer zero.

### The same word, at the wrong width

Fixing `rlwinm` made the PowerPC tests fail in `CS_MODE_64` only, which turned
out to be a second defect underneath. Capstone reports the extended mnemonics
in 64-bit mode and the raw five-operand form in 32-bit mode, so in 64-bit mode
`rlwinm 0, 1, 8, 0, 31` arrives as `rotlwi` and `rlwinm 0, 1, 0, 16, 31` as
`clrlwi` -- and the translators for those operate at the **register** width:

| instruction | means | did | with `r1 = 0x12345678` |
| --- | --- | --- | --- |
| `rotlwi r0, r1, 8` | rotate the low 32 bits | rotated all 64 | `0x1234567800`, not `0x34567812` |
| `clrlwi r0, r1, 16` | clear the top 16 of the word | cleared the top 16 of the register | `0x12345678`, not `0x5678` |
| `slwi` | shift left, drop past bit 31 | carried into bit 32 | |
| `srwi` | shift the word right | brought the upper 32 bits down | |

The bits leaving bit 31 have to come back at bit 0. At 64 bits they moved into
bit 32 and stayed there, which is not a rotate of anything. All four are word
operations now, written back zero-extended, which is what the ISA says and a
no-op on 32-bit PowerPC.

This was reachable only in 64-bit mode, and 64-bit mode was the only mode that
reached this code.

### MIPS: the rounding conversions, and a destination in the wrong file

`trunc`, `round`, `ceil` and `floor` were all dispatched to
`translatePseudoAsmOp0FncOp1()`. Two things were wrong and only one of them was
the missing semantics:

```
%0 = load double, ptr @fd2
%1 = call double @__asm_trunc.w.d(double %0)
store double %1, ptr @fd0        <-- the next instruction reads f0
```

The destination of a `.w` form holds a 32-bit integer and belongs in the
single-precision register. `loadRegister()`/`storeRegister()` map **every** FP
operand of a double-format instruction to the FDn file, which is right for the
source and wrong for the destination, and there is no per-operand way to ask
for it -- so the store goes through `getRegister()` directly, the way
`translateCvt()`'s `cvt.s.d` branch already does.

Rounding mode is the whole difference between the four and it is in the
mnemonic. MIPS `round` is to nearest **even**, so it is `llvm.roundeven` and
not `llvm.round`, which is half away from zero; the test uses 2.5 and -2.5,
the only inputs on which the two disagree.

Six of the twelve forms translate -- the four `.w.s`/`.w.d` pairs on MIPS32 and
`.l.d` on MIPS64 -- and six do not. On MIPS64 every FP register is 64 bits, so
a `.s` source is not the float the instruction reads and a `.w` destination is
not the 32-bit slot it writes; those fall back to the pseudo-assembly call
rather than answering at the wrong width. The rule is the loaded value's own
type against the width the mnemonic asks for, not the register id, which is the
same either way.

### Twenty-seven tests were asserting the gap

`tests/capstone2llvmir` had 24 MIPS and 3 PowerPC tests of this shape:

```cpp
EXPECT_JUST_VALUES_CALLED({
    {_module.getFunction("__asm_rlwinm"), {0x1234, 0x4, 0x2, 0x5}},
});
```

They pinned the pseudo-assembly call as if it were the specification. Fifteen
of them are rewritten to assert semantics; the twelve that remain are the
MIPS64 forms that still fall back, and they now say so. The old `rlwinm` test
also used operands for which the answer is zero, which is the weakest possible
assertion for a mask.

### Falsification

Ten mutations, each reverted alone, each rebuilt and run:

| mutation | result |
| --- | --- |
| PowerPC's mask numbered from the bottom | 2 tests fail |
| the wrapping mask treated as empty | 1 test fails |
| `rlwimi` drops the destination merge | 1 test fails |
| `rotlwi` back to register width | 2 tests fail |
| `clrlwi` back to register width | 1 test fails |
| the MIPS result stored through `storeRegister()` | 4 tests fail |
| MIPS `round` as half-away-from-zero | 3 tests fail |
| the probe never reports a pseudo-asm call | self-test fails, gate exits 1 |
| the intended-model list parses to nothing | self-test fails, gate exits 1 |
| the rotate family back to `translateRotateComplex5op` | **see below** |

That last one is the argument for the gate existing, run as an experiment
rather than asserted:

```
-- COV-01 (reads the dispatch table) --
arch         decoded   skipped     zeros    covered     rate  uncovered-kinds
powerpc         8904         0         0       8904   1.0000  0

-- PSEUDO-01 (asks the translator) --
arch       translated  modelled  unmodelled     rate  kinds
powerpc          8736         0          48   0.9945  2
    rlwinm             __asm_rlwinm                         42   0.48%
```

The dispatch table still has a function pointer either way, so COV-01 reports a
perfect score for a translator that has stopped translating 42 instructions.

### Where it leaves the parity corpus

```
arch       translated  modelled  unmodelled     rate
x86_64           5871        45           0   1.0000
arm              7837         0           0   1.0000
arm64            6733         0           1   0.9999
mips             8520         0           1   0.9999
powerpc          8736         0           6   0.9993
```

PSEUDO-01 goes into `standalone-check.yml` unfloored on its first run, for the
reason the static COV-01 step did, and into `check_push_gates.sh` as its
self-test -- which also proves the translator sources compile and link against
system LLVM without the test suite attached.

### The three that are left, and the backlog behind them

`__asm_swc2` (MIPS, 1) is a coprocessor-2 store: opaque by construction, like
`SVC` and `SYSCALL`.

`__asm_cror` (PowerPC, 6) is the condition-register bit family, and
`translateCrModifTernary()` emits its pseudo-assembly call **on purpose**,
taking all eleven CR fields back out of the return value. Translating it means
mapping a CR bit index to this translator's split model -- `CR0LT`/`GT`/`EQ`/`UN`
as four `i1`s, `CR1`..`CR7` as `i4` each -- and the bit order within those `i4`s
is not written down anywhere I could check. Getting it wrong is a silent wrong
branch, so it is recorded rather than guessed.

`__asm_movi` (ARM64, 1 here, 1,557 on the static corpus) is the vector
immediate. Capstone reports the raw `imm8` and a shift, not the expanded lane
value: `movi v0.2d, #0xff` comes back as `imm=0xff` where the architectural
value is `0xffffffffffffffff`. The family has about ten immediate encodings
(8-bit, 16- and 32-bit shifted, 32-bit MSL, the 64-bit byte-mask, and the
`fmov`-like form), and one wrong expansion is a wrong constant in the output.
Recorded.

The static corpus, measured with the same probe **before** the fixes in this
commit, is the real backlog, and it is not what COV-01's uncovered lists say:

```
arch       translated  unmodelled     rate    top entries
x86_64       5163159      213590   0.9586    vpcmpeqb 20010, vpmovmskb 19778, vmovdqu 13518
arm          3240326       26650   0.9918    mrc 11697, ubfx 1898, uqsub8 1554, tbb 1432
arm64        3559505       35514   0.9900    mrs 12400, st1b 4620, svc 4206, movi 1557
mips         4292792       45329   0.9894    rdhwr 19389, ext 4389, lwl 3586, lwr 3503, ins 2580
powerpc      4637012       79193   0.9829    rlwinm 28341, mfcr 9008, rlwimi 7795, tdi 4968
```

`ubfx` and `sxth` on ARM, `ext`, `ins` and `wsbh` on MIPS, and `mfcr` on
PowerPC are ordinary instructions with no register-model obstacle at all. They
are the next batch. `mrc` (ARM) and `rdhwr` (MIPS) are the TLS-pointer reads
that `mrs` is on ARM64, and they need the same synthetic register the previous
commit recorded.


## Batch F: the bitfield instructions PSEUDO-01 named

The previous section listed a backlog PSEUDO-01 found and COV-01 could not
see. This is the top of it that has no register-model obstacle: bitfield
reads and writes, sign-extension, and byte-order changes within a word. Twelve
instructions, all of them scored "covered" by COV-01 and all of them emitting a
call.

| instruction | occurrences | was | now |
| --- | --- | --- | --- |
| ARM `UBFX` | 1,898 | `__asm_ubfx` | `translateBitfield` |
| ARM `SXTH` | 805 | `__asm_sxth` | `translateSxt` |
| ARM `SBFX`, `BFI`, `BFC`, `SXTB` | below the cut | pseudo-asm | as above |
| ARM `REV16`, `REVSH`, `RBIT` | below the cut | pseudo-asm | `translateRev16` |
| MIPS `EXT` | 4,389 | `__asm_ext` | general path in `translateExt` |
| MIPS `INS` | 2,580 | `__asm_ins` | `translateIns` |
| MIPS `WSBH` | 1,890 | `__asm_wsbh` | `translateWsbh` |

### Three ways to get these wrong, all of them quiet

**Sign.** `UBFX` and `SBFX` differ in one letter and in what happens to a field
whose top bit is set; `SXTB`/`SXTH` differ from the `UXT` pair the same way.
The tests use fields and values whose top bit is set, because a field of
`0x56` cannot tell the two apart.

`SBFX` is also the one with an ordering trap: sign-extending from an arbitrary
bit is `(rn << (32 - lsb - width)) >>s (32 - width)`, and the arithmetic shift
has to come **second**. Masking first and then shifting loses the sign.

**Width.** `REV16` is not a 32-bit byte swap and neither is `WSBH`:
`0x12345678` becomes `0x34127856`, not `0x78563412`. Both are the plausible
`llvm.bswap` away from being wrong, and on MIPS the difference has a name --
`wsbh` followed by `rotr $2, $2, 16` is how the ISA spells a 32-bit swap, which
is why `wsbh` appears in every endian conversion in the corpus. `REVSH` is the
low halfword swapped and then **sign**-extended.

For the same reason, `EXT`, `INS` and `WSBH` are restricted to 32-bit registers
here. They are word instructions; MIPS64 spells the 64-bit ones `DEXT`, `DINS`
and `DSBH`, and a register-width implementation silently becomes that other
instruction. Out-of-range widths fall back to the pseudo-assembly call.

**The destination.** `BFI`, `BFC` and MIPS `INS` read their destination and
keep its bits outside the field. Dropping that is the difference between
`0xaabbeedd` and `0xee00`.

`translateExt` was the interesting one: it already had a translator, and that
translator handled exactly one idiom -- `ext rt, rs, 0, 31`, recognised as a
floating-point absolute value because clearing the sign bit is what it is --
and sent every other form to `__asm_ext`. The idiom stays; the general case is
four lines below it.

### A test that could not fail, caught by falsification

The first version of the three MIPS tests used `emulate()` and hit

```
error: instruction requires a CPU feature not currently enabled
ext $2, $3, 8, 8
```

because Keystone will not assemble MIPS32R2 instructions in plain MIPS32 mode.
Changing them to `ONLY_MODE_32R6` made the suite green -- and the falsification
run then showed why that was worse than the failure: reverting `ext`, `ins` and
**all three** left the suite passing, because

```cpp
::testing::Values(CS_MODE_MIPS32, CS_MODE_MIPS64),
```

is the whole instantiation. `ONLY_MODE_32R6` meant the test body never ran.
They use `emulate_bin()` with hand-assembled encodings now, which Capstone
decodes in MIPS32 mode without complaint, and the three mutations fail as they
should.

This is the value of reverting every fix separately, stated plainly: the tests
passed, the code was right, and the tests were still worthless.

### Falsification

Ten mutations, each reverted alone, each rebuilt and run; all ten fail:

| mutation | result |
| --- | --- |
| `SBFX` zero-extends | 1 test fails |
| `BFI` drops the destination | 1 test fails |
| the bitfield mask is not shifted into place | 2 tests fail |
| `REV16` is a word bswap | 1 test fails |
| `REVSH` zero-extends | 1 test fails |
| `RBIT` is a bswap | 1 test fails |
| `SXTB`/`SXTH` zero-extend | 2 tests fail |
| MIPS `EXT`'s general path removed | 1 test fails |
| MIPS `INS` drops the destination | 1 test fails |
| MIPS `WSBH` is a word bswap | 1 test fails |

### Where it leaves the static corpus

```
             before   after   uncovered kinds
arm          0.9918  0.9931       36 -> 28
mips         0.9894  0.9915       15 -> 12
```

PSEUDO-01 is floored from this commit, at what CI itself measured on run 194:

```
x86_64 1.0000   arm 1.0000   arm64 0.9999   mips 0.9999   powerpc 0.9993
```

x86-64 and ARM at 1.0000 means any instruction that stops being translated on
either fails the step -- including one that keeps its function pointer, which
is the case COV-01 cannot see.

### What is left, in order

```
arm      mrc 11697   uqsub8 1554   tbb 1432   stcl 924   ldcl 924   sel 840
mips     rdhwr 19389   syscall 3953   lwl 3586   lwr 3503   swl 2740   swr 2613
```

`mrc` (ARM) and `rdhwr` (MIPS) are the TLS-pointer reads that `mrs` is on
ARM64: 31,086 occurrences between them, and all three need the same synthetic
register id and the matching ABI-provider resize recorded two commits ago. That
is now the single largest item on any architecture and is worth doing properly.

`lwl`/`lwr`/`swl`/`swr` (12,442) are the MIPS unaligned load and store pair.
They are specifiable, and their specification is endianness-dependent in a way
that would need the corpus's big-endian binaries and a little-endian fixture to
test both halves -- a batch of its own rather than a line each.

`sel`, `uadd8` and `uqsub8` (3,234) are the ARMv6 GPR SIMD ops. `sel` reads the
`GE` flags, which this translator does not model, and `uadd8` writes them --
so the three come together with a register-model change or not at all.

`tbb`/`tbh` (2,066) are table branches: control flow, not data, and a different
kind of work from anything in this section.


## Batch G: the thread pointer

The largest single item PSEUDO-01 found, and it is the same instruction on
three architectures:

| | reads | occurrences | was |
| --- | --- | --- | --- |
| ARM | `mrc p15, 0, Rt, c13, c0, 3` | 11,697 | `__asm_mrc(15, 0, 13, 0, 3)` |
| ARM64 | `mrs Xd, tpidr_el0` | 12,400 | `__asm_mrs(...)` |
| MIPS | `rdhwr rt, $29` | 19,389 | `__asm_rdhwr(...)` |

43,486 occurrences between them. All three are how glibc finds thread-local
storage, and all three were coming out of the decompiler as calls to undefined
functions -- from which no later pass can recover that the result is a pointer,
let alone a stable one. Every TLS access in every ARM, ARM64 and MIPS binary
this decompiler has ever processed ended there.

### The blocker recorded two commits ago was not one

That commit said `TPIDR_EL0` needed a synthetic register id, and that
`src/bin2llvmir/providers/abi/arm64.cpp`'s
`_id2regs.resize(ARM64_REG_ENDING, nullptr)` made an id past the end "an
out-of-bounds write, not a missing entry". Both halves were wrong.

```cpp
void Abi::addRegister(uint32_t id, llvm::GlobalVariable* reg)
{
    if (id >= _id2regs.size())
    {
        _id2regs.resize(id+1, nullptr);
    }
    _id2regs[id] = reg;
}
```

The vector grows. The `resize(ARM64_REG_ENDING)` line is a reservation, not a
bound, and it is eleven lines above the function that writes into it in the
same file -- which I did not read.

And this translator has been doing exactly this since before the branch:

```cpp
enum arm_reg_cpsr_flags { ARM_REG_CPSR_N = ARM_REG_ENDING + 1, ... };
enum mips_reg_fpu_double { MIPS_REG_FD0 = MIPS_REG_ENDING + 1, ... };
```

The CPSR flags, ARM's SPSR/CPSR pair and MIPS's sixteen double-precision FP
pairs are all synthetic ids past `*_REG_ENDING`. The pattern was established,
working and three files away.

Recording a blocker after reading one line of it, and then acting on that
record two commits later, is a worse failure than not noticing the instruction
at all: the note would have kept the work from being done. The wrong paragraph
is annotated in place rather than deleted.

### ARM64 needed no new register

`arm64_init.cpp` already names and types **every** AArch64 system register
Capstone knows -- 200-odd of them, `tpidr_el0`, `fpsr`, `fpcr`, `midr_el1` and
the rest. What was missing was the instruction. `MRS` and `MSR` were `nullptr`
entries, so a complete system-register file sat there with nothing able to read
or write it.

`translateSysRegMove` handles both directions and all of them, which is why the
other eighteen `mrs` per binary translate too. The system-register operand
cannot go through `loadOp()`: Capstone's operand union aliases `sys` onto
`reg`, so `ARM64_OP_SYS` values collide numerically with ordinary register ids,
and `loadOp()` refuses them by design for that exact reason. The id is read out
of `.sys` and looked up directly; an id with no register falls back to the
pseudo-assembly call.

### MIPS: the selector that is also the stack pointer

```
rdhwr $v0, $29 | [0] type=REG reg=4(v0) | [1] type=REG reg=31(sp)
```

Capstone reports the hardware-register number as an ordinary GPR id, and
`MIPS_REG_29` **is** `MIPS_REG_SP` -- the same enumerator value. A translation
that loaded operand 1 would read the stack pointer, produce a plausible-looking
pointer, and be wrong in every binary. The number is a selector into a
different register file and is compared as a number here, never loaded. The
test sets `$sp` to `0x7fff0000` and the thread pointer to `0xdeadbeef` and
requires the second.

There is no companion test for a different selector: Capstone 5.0.9 does not
decode one. `$0`, `$1`, `$2` and `$3` all come back undecodable in MIPS32 mode
and only `$29` produces an instruction, so the selector check is unexercised by
this suite. Saying so is better than implying otherwise with a test that cannot
reach it.

### ARM: one encoding out of a coprocessor space

`mrc` is a coprocessor read and coprocessor reads are genuinely opaque -- but
`p15, 0, Rt, c13, c0, 3` is TPIDRURO, and in the static corpus it is *every*
`mrc`. The translation checks all six immediates and sends anything else to the
pseudo-assembly path, which the second test pins with `c0, c0, 0` (MIDR).

That test found something worth keeping: `translatePseudoAsmGeneric()` passes
every operand as an argument and returns void, so for `mrc` the destination
register is **read** rather than written. An instruction that produces a value
was producing nothing and consuming its own destination.

### Falsification

Six mutations, each reverted alone, each rebuilt and run; all six fail:

| mutation | result |
| --- | --- |
| ARM `MRC` translates every coprocessor read | 1 test fails |
| ARM `MRC` reads the stack pointer instead | 1 test fails |
| ARM64's system-register operand index fixed at 0 | 2 tests fail |
| ARM64 `MSR` writes the GPR instead of the system register | 1 test fails |
| MIPS `RDHWR` loads operand 1 (the `$sp` trap) | 1 test fails |
| the four dispatch entries back to `nullptr` | 5 tests fail |

### Where it leaves the static corpus

```
             before   after   unmodelled
arm          0.9931  0.9967    22387 -> 10690
arm64        0.9900  0.9935    35514 -> 23114
mips         0.9915  0.9960    36457 -> 17068
```

The gated dynamic corpus is unchanged -- it contains no `mrc`, `mrs` or
`rdhwr` at all, which is the same reason it took a static corpus to find any of
this.

### What is left

```
arm      uqsub8 1554   tbb 1432   stcl 924   ldcl 924   uadd8 840   sel 840
arm64    st1b 4620   svc 4206   ld1b 2688   movi 1557   ldg 1302   umaxp 882
mips     syscall 3953   lwl 3586   lwr 3503   swl 2740   swr 2613   break 294
powerpc  mfcr 9008   tdi 4968   sc 4292   tdgti 1331   lvx 1134   vperm 1134
```

PowerPC is now the worst of the five and `mfcr` its largest entry: move the
whole condition register into a GPR, which needs the CR bit-order question the
`cror` note above raises. `lvx`, `stvx` and `vperm` are AltiVec and need a
vector register file. MIPS's `lwl`/`lwr`/`swl`/`swr` and ARM64's SVE and MTE
are unchanged from the previous lists.


## Batch H: PowerPC's condition register was modelled twice, and one CR-bit
## instruction never reached its translator

Three findings, all in the same place, all found by following `__asm_mfcr`
(9,008 in the static corpus, the largest remaining entry on any architecture)
and `__asm_cror` (6 on the gated corpus).

### The condition register has two representations and one of them is dead

`powerpc_init.cpp` creates **both**:

* 32 `i1` globals -- `cr0_lt`, `cr0_gt`, `cr0_eq`, `cr0_so` through `cr7_so` --
  which `storeCr0()` and `storeCrX()` write on every comparison and every
  record-form instruction; and
* 8 `i4` globals, `PPC_REG_CR0`..`CR7`, one per field.

Nothing anywhere calls `loadRegister(PPC_REG_CR0..CR7)`. The `i4` set is
write-only: state that is produced, never consumed, and impossible to keep in
step with the `i1` set that the branches actually read.

### `cror` was clobbering CR0

`translateCrModifTernary()` -- the entry for all eight CR-bit logical
operations -- emitted a pseudo-assembly call returning an eleven-field struct
and then wrote, unconditionally:

```cpp
storeRegister(PPC_REG_CR0LT, ExtractValue(c, {0}), irb);
storeRegister(PPC_REG_CR0GT, ExtractValue(c, {1}), irb);
storeRegister(PPC_REG_CR0EQ, ExtractValue(c, {2}), irb);
storeRegister(PPC_REG_CR0UN, ExtractValue(c, {3}), irb);
storeRegister(PPC_REG_CR1,   ExtractValue(c, {4}), irb);   // ... CR7
```

`cror cr7lt, cr7gt, cr7lt` does not touch CR0. That code overwrote all four of
CR0's bits with the return value of an undefined function, so a comparison into
CR0, then CR-bit arithmetic on any other field, then a branch on CR0, branched
on whatever the call returned. A wrong branch, not a missing translation. The
seven `CR1`..`CR7` writes went into the dead `i4` set.

There is nothing to model here: Capstone reports all three operands as the
individual bit registers this translator already has, so `cror` is one `or`
between two `i1` loads.

### `cror` never reached that code either

Fixing it did not make the test pass, and the reason is the third finding:

```
4f9de382 -> cror cr7lt, cr7gt, cr7lt | id=227 ops=3
227 -> crmove     231 -> cror
```

Capstone 5.0.9 decodes `cror` with **`PPC_INS_CRMOVE`'s id** while printing
"cror". The dispatch table's `PPC_INS_CROR` entry was unreachable; every `cror`
went to `translateCrNotMove`, whose `EXPECT_IS_BINARY` rejected its three
operands and fell through to the generic pseudo-assembly path. That is the
shape of the call PSEUDO-01 saw -- `void @__asm_cror(i1, i1, i1)`, three
arguments and no return -- which is not what `translateCrModifTernary()`
emitted, and the discrepancy was visible in the IR all along.

Third instance of this shape in the branch, after ARM's `NOP`/`HINT` and
`VMRS`/`FMSTAT`: **a dispatch-table key that Capstone does not produce**. The
operation is taken from `i->mnemonic` now, which is what Capstone actually
prints, and `translateCrNotMove` delegates when it is handed three operands.

### `isCrBitRegister` was two registers short

```cpp
return PPC_REG_CR0EQ <= r && r <= PPC_REG_CR5UN;
```

Capstone groups the CR bit registers by bit **name**, not by field: EQ is
312..319 for CR0..CR7, GT is 320..327, LT 328..335, UN 336..343. So that range
is 312..341 and excludes exactly two registers, `CR6UN` and `CR7UN` -- the
summary-overflow bits of the two highest fields. It reads as "the CR bit
registers" and means "all but two of them". The predicate is used by
`translateCrNotMove` and `translateCrSetClr` as well, so `crnot cr7so, cr7so`
was falling to pseudo-assembly for the same reason.

A range over an enum whose order has not been checked is the same defect as the
`MIPS_REG_29 == MIPS_REG_SP` trap in the previous commit, one layer up.

### `mfcr`

The 32 bits assemble into a GPR at fixed positions, and PowerPC numbers them
from the **most** significant end: CR0's LT bit is bit 31 of the word, CR7's SO
bit is bit 0. The test sets one bit at each end and one in the middle, so a
translation that assembled the fields the other way round answers `0x80004001`
where the instruction answers `0x80020001`.

### Falsification

Six mutations, each reverted alone, each rebuilt and run; all six fail:

| mutation | result |
| --- | --- |
| the CR-bit operation keyed on the id again | 2 tests fail |
| `translateCrNotMove` does not delegate on three operands | 2 tests fail |
| `crand` implemented as `or` | 1 test fails |
| `mfcr`'s bit order reversed | 1 test fails |
| `isCrBitRegister` back to `CR5UN` | 1 test fails |
| `mfcr` back to the pseudo-assembly call | 1 test fails |

The `CR5UN` mutation needed a test written for it: the CR7 test added first
uses `CR7LT` and `CR7GT`, both inside the old range, so it could not have
caught the off-by-two. `cror cr7un, cr7eq, cr7un` is the one that can.

### Where it leaves PowerPC

```
                    static            gated corpus
before Batch E      0.9829            0.9945
after  Batch H      0.9932            1.0000
```

47,589 instructions on the static corpus between the two commits. PowerPC now
joins x86-64 and ARM at 1.0000 on the gated corpus; the floor moves to match
once CI has measured it.

What is left on PowerPC is mostly not translatable here: `tdi`, `tdgti`,
`tdlgti` and `twi` are conditional traps, `sc` is the system call, `attn` is a
checkstop, `lvx`, `stvx` and `vperm` are AltiVec and need a vector register
file, and `dcbst`/`icbi`/`dcbz` are cache maintenance -- `dcbz` in particular
*does* write memory (it zeroes a cache line) and is the one worth doing next.
`lwbrx`/`stwbrx` (byte-reversed load and store) and `stmw`/`lmw` (load and
store multiple) are both straightforwardly specifiable.

`mcrf` is still on the pseudo-assembly path, now visibly so: its function is
named `__asm_mcrf_cr0_read`, which is what it does -- it reads CR0 and writes
the dead `i4` registers. It is four `i1` copies and belongs in the next batch
with `mtcrf`, which has the same shape.

---

## Batch I — the condition-register moves, the byte-reversed accesses, the
## multiples, and an undefined address in five translators

Batch H's backlog, in its own order, plus one finding that was not on it and is
the most serious thing in the commit.

### `rA = 0` is the literal zero, and it was `undef`

Every X-form (register-indexed) memory access on PowerPC has the same special
case in its encoding: the rA field is a register number, except that **rA = 0
means the literal zero rather than r0**. `lwzx rD, 0, rB` addresses rB and
nothing else. GCC emits it whenever the address is already whole in one
register, which on a RISC with no base+index addressing mode is most of the
time.

Capstone reports that slot as `PPC_OP_REG` with `reg == PPC_REG_INVALID`, and
`loadOp()`'s register case is:

```cpp
case PPC_OP_REG:
{
    auto* r = loadRegister(op.reg, irb);
    return r ? r : llvm::UndefValue::get(ty ? ty : getDefaultType());
}
```

`loadRegister()` returns `nullptr` for `PPC_REG_INVALID`. So the effective
address of every rA = 0 indexed access was

```llvm
%2 = add i64 undef, %1
%3 = inttoptr i64 %2 to ptr
```

an undefined pointer, in five translators: `translateLoadIndexed`,
`translateStoreIndexed`, `translateLoadFloatIndexed`,
`translateStoreFloatIndexed` and `translateLhbrx`.

The `d(rA)` forms never had this. `loadOp()`'s `PPC_OP_MEM` case already reads
`mem.base == PPC_REG_INVALID` as "displacement alone" — the rule was known,
written down, and applied to one of the two addressing forms.

#### The test that could not fail

The first three tests written for this passed with the fix reverted. The
emulator evaluates `UndefValue` as zero, so `add undef, rB` and `rB` reach the
same address; and because `loadOp()` never emits a load for an invalid
register, the register-load set is identical either way. Nothing observable
through emulation distinguishes the two.

That is the fourth time in this branch that a mutation has come back green, and
the answer each time has been that the assertion was in the wrong place. The
defect here is in the IR, so the assertion is:

```cpp
for (auto& op : it->operands())
{
    EXPECT_FALSE(isa<UndefValue>(op.get())) << a << " produced an undef operand";
}
```

over all seven affected shapes. Reverting `generateIndexedAddress` now fails it.

### `mcrf`

`mcrf crD, crS` copies one condition register field to another: four bits. It
was three pseudo-assembly calls, and the shape of each says what was wrong:

| form | call | what was dead |
| --- | --- | --- |
| `mcrf crD, cr0` | `__asm_mcrf_cr0_read(i1,i1,i1,i1) -> i4` | the `i4` destination |
| `mcrf cr0, crS` | `__asm_mcrf_cr0_write(i4) -> {i1,i1,i1,i1}` | the `i4` source |
| `mcrf crD, crS` | `__asm_mcrf(i4) -> i4` | both |

Batch H established that the `i4` registers `PPC_REG_CR0..CR7` are written and
never read; the `i1` bits are what every branch reads. All three of these were
the two representations bridged by an opaque function. It is four `i1` copies.

### `mtcrf`, and a dispatch key with no encoding

`mtcrf CRM, rS` writes the CR fields selected by an 8-bit mask from the low
word of rS — the counterpart of `mfcr`, which Batch H fixed. It returned a
struct of four `i1`s and seven `i4`s and stored all eleven: the four that
matter came out of an undefined function, the seven with the field structure
were dead.

Two orderings, both easy to get backwards and neither loud:

* The bit positions are numbered from the **most** significant end, the same as
  `mfcr`: field *f* bit *b* is at `31 - (4f + b)`.
* **CRM is numbered the same way.** Mask bit `0x80` selects CR0, `0x01` selects
  CR7. Read as `1 << f` the mask selects the mirror-image set of fields, which
  for the common `0xff` and `0xf0` is invisible. The one-field test uses `0x08`,
  which is CR4 under the architecture's numbering and CR3 under the other.

`PPC_INS_MTCR` was dispatched to `translateMtcr`, which was doubly wrong —

```cpp
storeRegister(PPC_REG_CR0GT, irb.CreateAnd(op0, irb.getInt32(1 << 1)), irb);
```

an `i32` stored into an `i1` register, so `storeRegister`'s truncation takes bit
0 of `op0 & 2`, which is always zero; thirty-one of the thirty-two bits were
constant `false` and the one that was not was the wrong end of the word — and
**unreachable**. Capstone decodes `mtcr rS` as `PPC_INS_MTCRF` with mask
`0xff`. An exhaustive scan of the XFX `xo = 144` encoding space, which is the
only space LLVM's tables map to `MTCRF8`/`"mtcr"`, produces exactly two ids in
both 32- and 64-bit mode:

```
id=774   mtcrf     ops=2
id=796   mtocrf    ops=1
```

`PPC_INS_MTCR` (773) is in the instruction enum and comes out of the
disassembler for nothing. This is the fourth dispatch key in this branch that
Capstone does not produce, after ARM's `NOP`/`HINT` and `VMRS`/`FMSTAT` and
PowerPC's `CROR`/`CRMOVE`. The entry is `nullptr` now and the broken translator
is gone; `mtcr` reaches `translateMtcrf`, where it always went.

`mtocrf` stays unmodelled for a different reason: capstone reports **one**
operand for it, the source register, and drops the mask. The instruction cannot
be translated from the detail available.

### The byte-reversed accesses

`lhbrx`, `lwbrx`, `ldbrx`, `sthbrx`, `stwbrx`, `stdbrx` — load or store with
the byte order of the other endianness. A big-endian PowerPC reading a
little-endian file uses them, so they are what a decompiler meets in ELF
parsers, PE parsers and network code.

Four of the six were `nullptr` or a pseudo-assembly call. The two stores were
`translatePseudoAsmFncOp0Op1Op2`, which passes the value and the two address
registers to an opaque function and **writes no memory at all** — a store that
stores nothing is not an approximation of a store.

The one that was modelled, `lhbrx`, was two byte loads OR'd together, with

```
// TODO: Maybe model this as ASM pseudo call as PPC_INS_LWBRX.
```

on it and its test commented out under `TODO: Not working, maybe because of
little vs big endian?`. The suggestion ran the wrong way and the diagnosis was
wrong: the emulator stores a 16-bit value at one address rather than as two
bytes, so the second byte load read nothing. One `llvm.bswap` covers all six.

### `lmw` and `stmw`

`stmw rS, d(rA)` writes rS, rS+1, … r31 to consecutive words; `lmw` reads them
back. A compiler emits them to save and restore the callee-saved half of the
register file in one instruction, so they bracket whole functions. `stmw` was
668 occurrences of a call that wrote nothing; `lmw` was `nullptr`.

The count is 32 − rS and is encoded nowhere: `stmw r28, 8(r1)` is four
transfers and `stmw r31, 8(r1)` is one. The tests give each register a distinct
value and pin both ends of the written range.

The transfers are 32-bit words on 64-bit PowerPC too. The emulator cannot see
that either — its memory is a map from address to value and does not record the
width of an access, so a 64-bit store and a 32-bit store at the same address
are indistinguishable to it. That assertion is in the IR as well.

### Falsification

Nine mutations, each reverted alone, each rebuilt and run; all nine fail:

| mutation | result |
| --- | --- |
| `rA = 0` back to a plain add | 2 tests fail |
| `mcrf` back to the pseudo-assembly path | 8 tests fail |
| `mtcrf`'s mask read as `1 << f` | 4 tests fail |
| `mtcrf`'s bit order reversed | 6 tests fail |
| the byte-reversed store drops its `bswap` | 7 tests fail |
| the multiple counts from r0 | 6 tests fail |
| the multiple's offset is `r * 4` | 6 tests fail |
| `lwbrx` back to the pseudo-assembly call | 4 tests fail |
| the multiple transfers at register width | 1 test fails |

Two of the nine — the first and the last — came back **green** on their first
run, and both are recorded above. Neither was a fix that did not matter; both
were assertions written where the emulator cannot look.

### Where it leaves PowerPC

```
                    static            gated corpus
before Batch E      0.9829            0.9945
after  Batch H      0.9932            1.0000
after  Batch I      0.9960            1.0000
```

The PSEUDO-01 PowerPC floor moves from 0.9993 to 1.0, which is what
standalone-check 200 measured for itself.

What is left on PowerPC, in order:

```
tdi 4968   sc 4292   tdgti 1331   lvx 1134   vperm 1134   stvx 1050
tdlgti 558   attn 431   dcbst 378   icbi 378   dcbz 336   tdlti 317   twi 315
```

The traps (`tdi`, `tdgti`, `tdlgti`, `tdlti`, `twi`) are ~7,500 between them
and are the largest remaining group: `td`/`tw` are how GCC spells a
division-by-zero check and `__builtin_trap`. They are conditional control flow
into a handler, not a value computation, and belong with `sc` (the system call)
and `attn` (checkstop) in whatever model this decompiler grows for traps.
`lvx`, `stvx` and `vperm` are AltiVec and need a vector register file.

**`dcbz`, reconsidered.** Batch H's note said `dcbz` "*does* write memory (it
zeroes a cache line) and is the one worth doing next". Having looked at it: it
is not, and the reason is that the architecture does not say how much memory it
writes. The cache block size is implementation-defined — 32 bytes on the 32-bit
implementations this corpus is built for, 128 on POWER4 and later, 32 again on
the PPC970. Choosing one and emitting a fixed-size zeroing store would produce
C that is wrong on the machines that do not match, which is worse than the
opaque call: a call says "unknown", a wrong store says "known" and lies.
`dcbst` and `icbi` have no memory effect to model at all. Correcting this here
rather than deleting the note, for the same reason the `_id2regs.resize`
paragraph was annotated in place.

---

## Batch J — the MIPS unaligned pair, and the endianness no fixture could see

`lwl`, `lwr`, `swl` and `swr` are 12,442 occurrences in the static parity
corpus: the largest specifiable group on any architecture outside x86's AVX,
and the largest single item left on MIPS.

MIPS has no unaligned load. A compiler that must read a word from an address it
cannot prove aligned emits two instructions, each transferring the part of the
word on one side of the containing aligned word's boundary:

```
lwl $2, 0($3)      big-endian          lwl $2, 3($3)      little-endian
lwr $2, 3($3)                          lwr $2, 0($3)
```

They never appear apart. The loads were `translatePseudoAsmOp0FncOp1`, which at
least returned a value; the stores were `translatePseudoAsmFncOp0Op1`, which
**wrote no memory at all**.

### Written against the aligned word

Not as a run of byte accesses. That is both what the hardware does — one bus
transaction — and the form from which a later pass can recognise the pair: two
reads of the same aligned words merging into one value.

The shift the merge needs is `8 * (EA & 3)` on a big-endian MIPS and
`8 * (3 - (EA & 3))` on a little-endian one, and the other member of the pair
uses its complement. All eight forms are one of those two shifts:

```
LWL   rt = (W << s)  | (rt & lowMask(s))
LWR   rt = (W >> r)  | (rt & highMask(r))
SWL   W  = (rt >> s) | (W  & highMask(s))
SWR   W  = (rt << r) | (W  & lowMask(r))
```

with `r = (bytes - 1) * 8 - s`. `EA & 3` is a run-time value, so the shifts are
run-time and the case split cannot be done in C++.

`highMask(n)` keeps the **top** n bits and is therefore `~ones(width - n)`, not
`~ones(n)`. Writing the second is the one mistake here that still produces a
plausible answer, because for the middle alignments the two are the same size —
it was in the first version of this and only the `EA & 3 == 1` cases caught it.
Both masks are built one width up and truncated, because `ones(width)` is a
shift by the operand width, which is poison in LLVM.

### A second fixture, because one can only be one endianness

`Capstone2LlvmIrTranslator::createMips32()` defaults to
`CS_MODE_LITTLE_ENDIAN`, and Keystone's `KS_MODE_MIPS32` does the same, so the
existing MIPS suite is little-endian throughout. The parity corpus is
big-endian `mips-linux-gnu`. **The endianness the suite cannot see is the one
the measured binaries use.**

These four instructions are the only ones in this translator whose meaning
depends on it, so the suite gained a second fixture differing from the first in
exactly two lines — `KS_MODE_MIPS32 | KS_MODE_BIG_ENDIAN` and
`createMips32(&_module, CS_MODE_BIG_ENDIAN)` — with the mirror-image
expectations. Hard-coding either endianness now passes one suite and fails the
other; reverting the `getExtraMode()` check fails sixteen tests across both.

C2L-01 counts both fixtures towards the MIPS floor. A per-architecture floor
that counts one of two fixtures is a floor the other can be deleted under.

### The pair test

Neither half's own test can catch two errors that cancel. `unaligned_word_load
_pair` runs both instructions over memory holding `11 22 33 44 55 66 77 88` and
requires the unaligned word at `0x1001` — `0x22334455` big-endian,
`0x88112233` little-endian, from the same bytes.

### Falsification

Seven mutations, each reverted alone, each rebuilt and run; all seven fail:

| mutation | result |
| --- | --- |
| the endianness hard-coded to big | 16 tests fail |
| `highMask(n)` computed as `~ones(n)` | 5 tests fail |
| the merge drops the half it keeps | 3 tests fail |
| the store writes through instead of merging | 8 tests fail |
| the address is not aligned down | 21 tests fail |
| left and right swapped | 12 tests fail |
| the doubleword forms answered on MIPS32 | 1 test fails |

### Where it leaves MIPS

```
                 static
after Batch G    0.9960
after Batch J    0.9989     17,068 unmodelled -> 4,626
```

What is left is `syscall` 3953 (opaque by nature, like x86's SYSCALL), `break`
294, `cfc1` 168 (the FPU control word, which needs an FCSR register), `swc2`
85, and the `teqi`/`tnei`/`daddi` tail at 42 each.

### What this uncovered, and is not fixed here

`daddi` appearing at all sent me to the dispatch table, where
`MIPS_INS_DADD`, `DADDI`, `DADDIU`, `DADDU`, `DSUB`, `DSUBU`, `DMULT`,
`DMULTU`, `DDIV`, `DDIVU`, `DSLL`, `DSRL`, `DSRA`, `DSLLV`, `DSRLV`, `DSRAV`,
`DSLL32`, `DSRL32`, `DSRA32`, `DROTR`, `DEXT`, `DINS`, `DCLZ`, `DCLO`, `DSBH`,
`DSHD` — **the entire MIPS64 doubleword instruction set** — are every one of
them `nullptr`.

`createMips64()` exists, `CS_MODE_MIPS64` is half of this suite's
instantiation, and a genuine 64-bit MIPS binary would arrive with almost none
of its arithmetic translated. Neither COV-01 nor PSEUDO-01 can see it, because
both corpora are 32-bit MIPS and contain no doubleword instruction at all:
this is a gap in the *corpus*, not in either measurement's logic, and it is the
first one found in this branch that no amount of refining the metric would
have exposed. Next batch.

---

## Batch K — MIPS64 had no doubleword instructions

Thirty-two dispatch entries, every one of them `nullptr`:

```
DADD  DADDI DADDU DADDIU  DSUB  DSUBU  DMULT DMULTU  DDIV  DDIVU
DSLL  DSLLV DSRL  DSRLV   DSRA  DSRAV  DSLL32 DSRL32 DSRA32
DROTR DROTRV DROTR32  DCLZ DCLO
DEXT  DEXTM DEXTU  DINS  DINSM DINSU  DSBH  DSHD
```

That is the whole 64-bit arithmetic, shift, count and bitfield set.
`Capstone2LlvmIrTranslator::createMips64()` exists, `CS_MODE_MIPS64` is half of
the MIPS test suite's instantiation, and `decoder_init.cpp` line 127 selects it
for a 64-bit MIPS binary — so a real MIPS64 binary reached this translator with
almost none of its arithmetic translated.

**Neither COV-01 nor PSEUDO-01 could see it.** Both corpora are 32-bit MIPS and
contain no doubleword instruction at all, so the denominator never included
one. This is the first gap found in this branch that is in the *corpus* rather
than in a measurement's logic, and no amount of refining either metric would
have exposed it — it was found by reading the dispatch table after `daddi`
showed up at 42 occurrences in the Batch J measurement.

### Most of it is the existing translators at the register width

Capstone decodes **none** of these in `CS_MODE_MIPS32` — checked, all of them —
so a doubleword instruction can only arrive when the translator is in 64-bit
mode, where `getDefaultType()` is already `i64`. `DADD` and friends route to
`translateAdd`, `DSLL` to `translateSll`, `DMULT` to `translateMult` (two ids
added to its sign/zero-extend switch), `DDIV`/`DDIVU` to `translateDiv` and
`translateDivu`, `DCLZ`/`DCLO` to `translateClz`/`translateClo`. No mode guard
is needed and none is written.

### Four things that are not

**The `32` forms.** A MIPS64 shift immediate is five bits, so shifting a
doubleword by 32 or more needs a second opcode: `dsll32 rd, rt, sa` shifts by
`sa + 32`. The assembler hides this — you write `dsll $2, $3, 40` and get
`dsll32 $2, $3, 8` — and taking the reported immediate at face value is off by
exactly 32, which for a shift is the difference between a value and zero.

**The bitfield six, and capstone's missing `+32`.** `pos` and `size` are six
bits of encoding between them and cannot cover the whole 0..63 × 1..64 range,
so `DEXTM` adds 32 to the size, `DEXTU` adds 32 to the position, and `DINSM`
and `DINSU` do the same for the write. **Capstone does not apply those
offsets.** It reports the encoded fields with only the `msbd + 1` arithmetic
done:

```
dextm $2, $3, 0, 33     arrives as    pos 0, size 1
dextu $2, $3, 32, 8     arrives as    pos 0, size 8
```

Taking its numbers at face value reads a one-bit field where a 33-bit one was
meant, and reads bit 0 where bit 32 was meant. Neither is a crash and both look
like a plausible bitfield. The tests use source values whose bit 32 is set, so
the two readings differ in the answer's top bit as well as its width.

**`dsbh` and `dshd` are not a 64-bit byte swap.** `dsbh` swaps the two bytes
within each of the four halfwords; `dshd` reverses the four halfwords and
leaves the bytes inside them alone. One `llvm.bswap.i64` answers
`0x8877665544332211` for both — which is what `dsbh` *followed by* `dshd`
produces, and is how the ISA spells the full swap. Same relationship `wsbh`
then `rotr 16` has on the 32-bit side, and the same trap Batch F found there.

**Keystone will not assemble the R2 forms** (`dext`, `dins`, `dsbh`, `dshd`,
`drotr`) in `KS_MODE_MIPS64`, and it macro-expands a written `ddivu` into a
zero check, the divide, a `break` and an `mflo`. Those tests use hand-assembled
encodings through `emulate_bin()`, as Batch F's do.

### Two defects the doubleword work exposed in the 32-bit code

**`rotr rd, rt, 0` produced poison.** `translateRotr` was

```cpp
op2 = irb.CreateAnd(op2, ConstantInt::get(ty, 31));
auto* lshr = irb.CreateLShr(op1, op2);
auto* sub  = irb.CreateSub(ConstantInt::get(ty, 32), op2);
auto* shl  = irb.CreateShl(op1, sub);
```

For `op2 == 0` the left shift is by 32, the whole width, which is poison in
LLVM — and a rotation of zero is a legal encoding meaning "no rotation".
Masking the complement with `width - 1` fixes it and costs one `and`: for
`n == 0` the complement is 0, both halves are `x`, and the OR is `x`.

**The variable shifts did not mask their amount.** `sllv rd, rt, rs` shifts by
the low *five* bits of `rs` and `dsllv` by the low six; the hardware masks and
nothing here did. A computed shift amount above the register width — which is
every case where the amount is not a constant — shifted past the operand's
width, which is poison.

Neither is visible to the emulator. `getShiftAmount()` masks an over-wide shift
with exactly the same `& (width - 1)`, so it answers correctly whichever
implementation is underneath: the rotate mutation came back **green** on its
first run, and so would a shift-masking one. Both assertions are on the IR —
no shift may have a constant amount at or above its operand's width, and every
variable shift's amount must be an `and` with `width - 1`.

That is the third and fourth time in this branch an emulation test has been
unable to see a real defect, after PowerPC's `undef` address and `lmw`'s
transfer width. The pattern is now explicit: **the emulator normalises away
exactly the undefined behaviour that matters in the decompiler's output.**
Poison, undef and access width are all invisible to it, and assertions about
any of the three belong on the IR.

### Falsification

Eight mutations, each reverted alone, each rebuilt and run; all eight fail:

| mutation | result |
| --- | --- |
| `dsll32` does not add 32 | 4 tests fail |
| `DEXTM`/`DINSM`'s `+32` on size dropped | 2 tests fail |
| `DEXTU`/`DINSU`'s `+32` on position dropped | 2 tests fail |
| the insert forms drop the destination | 3 tests fail |
| `dsbh`/`dshd` as a 64-bit `bswap` | 2 tests fail |
| the rotate back to its poison-for-zero shape | 2 tests fail |
| the variable shifts stop masking | 2 tests fail |
| the 32 dispatch entries back to `nullptr` | 21 tests fail |

### What is still wrong on MIPS64, and is not fixed here

The mirror of this batch. On a 64-bit MIPS the instructions *without* the `D`
are 32-bit word operations whose results are sign-extended into the 64-bit
register — `add`, `addu`, `addi`, `addiu`, `sub`, `subu`, `sll`, `srl`, `sra`,
`sllv`, `srlv`, `srav`, `rotr`, `mult`, `multu`, `div`, `divu`, `clz`, `clo` —
and every one of them is done here at the register width, which silently makes
it the doubleword instruction that now exists alongside it. `and`, `or`, `xor`,
`nor`, `slt` and `sltu` genuinely are 64-bit and are right as they stand.

Batch F already applied this rule to `EXT`, `INS` and `WSBH`, which fall back
rather than answering at the wrong width. Extending it to the arithmetic and
shifts is the next batch, and it is a correction rather than an addition: it
changes what already-translated instructions mean.

C2L-01 floor: Mips 640 → 688.

---

## Batch L — on MIPS64 the word instructions were doublewords

The mirror of Batch K, and a correction rather than an addition: it changes what
already-translated instructions mean.

On a 64-bit MIPS the instructions **without** the `D` are 32-bit word
operations whose results are sign-extended into the 64-bit register. `addu $2,
$3, $4` adds the low halves and sign-extends; `daddu` adds the registers. They
are different instructions and both exist — so doing the word one at the
register width silently performs the other, which until the previous commit was
the only 64-bit arithmetic this translator had.

Thirteen translators were doing exactly that: `translateAdd`, `translateSub`,
`translateNegu`, `translateSll`, `translateSrl`, `translateSra`,
`translateRotr`, `translateMul`, `translateMult`, `translateDiv`,
`translateDivu`, `translateClz` and `translateClo`.

### The rule was already in this file

`translateMadd()` and `translateMsub()` both carry

```cpp
// We operate on 0..31 bits even if on MIPS64.
```

and truncate their operands to `i32` before doing anything. Batch F applied the
same rule again to `EXT`, `INS` and `WSBH` by restricting them to 32-bit
registers. It was correct, written down, and applied in two translators out of
a dozen.

Nothing needed inventing here: `isWordOperation()` is the list, and
`narrowToWord()` truncates. The sign-extension back into the 64-bit register
needs no code at all, because `storeOp()`'s MIPS default is already
`SEXT_TRUNC_OR_BITCAST`.

### What is deliberately not on the list

`AND`, `OR`, `XOR`, `NOR` and their immediate forms, `SLT`, `SLTU`, `MOVN` and
`MOVZ` genuinely operate on the whole 64-bit register on MIPS64. `SEB` and
`SEH` sign-extend from bit 7 and bit 15, which gives the same answer at either
width. `LUI` already carried its own note saying it behaves as a 32-bit
instruction. A test pins `and` as a doubleword operation so that adding it to
the list fails.

### `mult` needed more than narrowing

`mult` is 32 × 32 into a **64-bit** product whose two halves go into `LO` and
`HI` *sign-extended to the register width*; `dmult` is 64 × 64 into 128. The
product width was `getArchBitSize() * 2`, which is 128 on MIPS64 for both. It
is now twice the width of the narrowed operand, and the halves go back through
`storeRegister`, whose sign-extending default does the rest.

### None of the 688 existing MIPS tests could see any of this

Every one of them uses operands that fit in 32 bits and produce results that do
not set bit 31 — exactly the region where the two readings agree. Applying the
whole change left all 688 passing. The twenty-four tests added here use values
where they do not agree, and each states both answers in its comment:

```
addu  0x180000000 + 1     word 0xffffffff80000001    doubleword 0x180000001
sll   0x18000000 << 4     word 0xffffffff80000000    doubleword 0x180000000
srl   0xffffffff11223344  word 0x01122334            doubleword 0x0ffffffff1122334
sllv  by 36               word shifts by 4           doubleword shifts by 36
multu 0xffffffff * 2      word LO/HI = -2 / 1        doubleword LO = 0x1fffffffe
clz   0x100008000         word 16                    doubleword 31
```

### Falsification

Six mutations, each reverted alone, each rebuilt and run; all six fail:

| mutation | result |
| --- | --- |
| `narrowToWord` is a no-op | 10 tests fail |
| the shifts are not word operations | 5 tests fail |
| `mult`/`multu` are not word operations | 1 test fails |
| the doubleword instructions are narrowed too | 6 tests fail |
| `mult`'s halves at the full product width | 1 test fails |
| `clz`/`clo` are not word operations | 1 test fails |

The fourth is the over-correction, and it is the reason `isWordOperation()` is
an explicit list rather than "everything without a D in its name".

C2L-01 floor: Mips 688 → 712. 4,997 tests.

---

## Batch M — the ARM64 NEON lane operations, and a test harness that aborted
## instead of failing

ARM64 was the lowest of the four non-x86 architectures after Batch L.
Batch C's note said of what remained: *"UMAXP (882), SHRN (798), UMINP, ADDP,
ADDV, UZP1, SADDL, UADDW, XTN, SHL, UMOV, CNT and MVNI are ordinary lane
operations this register model can express. A next batch, not a limitation."*
This is that batch, restricted to the operations whose operands are all the
same width.

### `add`, `sub` and `mul` were not `nullptr` entries

They had translators, and those translators opened with

```cpp
if (ifVectorGeneratePseudo(i, ai, irb)) return;
```

so a vector operand meant "give up" and `add v0.4s, v1.4s, v2.4s` came out as
`__asm_add` — 588 occurrences in the static parity corpus, every one of them
scored **covered** by COV-01, because the dispatch table has a function pointer
for `ADD`. The WF-01 shape again, in the one place where the translator's own
author wrote the bail-out deliberately.

And it *was* deliberate, and right at the time. The comment above it says so:
the alternative then was `CreateAdd` on a 128-bit register, which is not a
lane-wise add of anything, and it dumped core on `.4s` operands in all ten
ARM64 binaries of ARCH-01. The guard was the honest answer until there was a
lane model. There is one now. What the guard protected against is still
checked by a test — `hasIntegerOpcodeOnFpType()` must stay false — and three
tests that asserted `__asm_add` and `__asm_sub` exist are rewritten to assert
lane arithmetic instead.

### Three families, one function

```
lane-wise    the operation between corresponding lanes
pairwise     between ADJACENT lanes of vn followed by vm
permute      no operation at all, only a choice of lanes
```

All three are one `shufflevector` apart. The pairwise forms become the
lane-wise case by splitting the concatenation `[vn, vm]` into its even and odd
lanes; the permutes are the mask alone.

Every permute has its own test with `v1 = 1,2,3,4` and `v2 = 5,6,7,8`, because
a mask that is backwards produces a vector with the right lanes in the wrong
order, and `zip1` and `trn1` agree on the first two lanes of a four-lane
arrangement — one test cannot tell them apart.

### The right shifts, and a legal shift by the whole width

`ushr v0.16b, v1.16b, #8` is a legal encoding — it is how a register is zeroed
lane by lane — and `sshr` by 8 broadcasts each byte's sign. A shift equal to
the operand's width is **poison** in LLVM, so both are case-split: the logical
one answers zero and the arithmetic one shifts by `laneBits - 1`, which is the
value the architecture defines.

### The harness aborted instead of reporting, and hid two mutations

Two of the nine falsification mutations came back **"0 failing tests"**. Both
were genuinely falsified: the tests failed, and then the process died:

```
c2l_tests: llvm/ADT/APInt.h:1523: uint64_t llvm::APInt::getZExtValue() const:
Assertion `getActiveBits() <= 64 && "Too many bits for uint64_t"' failed.
```

`dumpFunction()` prints every instruction's emulated value with
`APInt::getZExtValue()`, and it runs **inside a failing assertion's message**.
ARM64 V registers are `i128`. So any NEON test that fails takes the whole
process with it, every test after it in the run never executes, and
`--gtest_brief=1` reports nothing at all.

That is the most misleading way a check can fail: not a wrong answer, but no
answer, indistinguishable from success to anything counting `[  FAILED  ]`
lines. It is the same shape as everything else this branch has been finding —
a checker that is more permissive than the thing it stands in for — one level
further out, in the test harness rather than in the product.

`dumpFunction()` now prints through `APInt::print()`, and the two places that
compare a called argument against a 64-bit expectation report a wider argument
as a failure instead of asserting on it. With that in, the two mutations report
3 and 17 failing tests.

### Falsification

Nine mutations, each reverted alone, each rebuilt and run; all nine fail:

| mutation | result |
| --- | --- |
| the pairwise forms treated as lane-wise | 2 tests fail |
| `UZP1` and `UZP2` swapped | 2 tests fail |
| `ZIP1`'s mask becomes `TRN1`'s | 1 test fails |
| `UMAXP` becomes signed | 1 test fails |
| `SSHR` becomes logical | 2 tests fail |
| the shift-by-the-lane-width case split removed | 2 tests fail |
| `SMOV` zero-extends | 1 test fails |
| `add`/`sub`/`mul` back to the pseudo-assembly path | 3 tests fail |
| the 20 dispatch entries back to `nullptr` | 17 tests fail |

The last two are the ones that read "0 failing tests" before the harness fix.

### Where it leaves ARM64

```
                 static
after Batch G    0.9935
after Batch M    0.9941     23,114 unmodelled -> 20,930
```

What is left, and why:

```
st1b 4620   ld1b 2688   whilelo 588   cntb/cntd     SVE: needs Z and P registers
svc 4206    brk 339                                 opaque by nature
movi 1557                                           Capstone reports the raw
                                                    imm8 and a shift rather
                                                    than the expanded lane
                                                    value, across ~10 encodings
ldg 1302  st2g/stz2g 924  gmi/irg 756               MTE: needs an allocation-tag
                                                    model
shrn 798    xtn   saddl  uaddw  addv  cnt           modellable, different
                                                    operand widths
```

The last row is the next batch. `shrn` and `xtn` narrow to half-width lanes,
`saddl`/`uaddw` widen, and `addv` reduces — none of them satisfies
`neonSameWidthRegs()`, which is why they are not in this one. `cnt` needs
`llvm.ctpop` on a vector, which the emulator's intrinsic handler does not
implement (it calls `getIntegerBitWidth()` on the result type), so it needs
either an emulator change or a manual expansion.

C2L-01 floor: Arm64 501 → 519. 5,015 tests.

---

## Batch N — three NEON instructions were not missing, they were wrong

The rest of Batch M's backlog — the operations whose source and destination
arrangements differ in width — plus three that had translators and produced
wrong answers.

### `neg v0.4s` negated the register

`translateNeg` has **no vector guard at all**. Every other translator that can
meet a vector operand opens with `ifVectorGeneratePseudo`; this one does not,
so `neg v0.4s, v1.4s` reached `CreateSub` on the 128-bit register.

```
v1 = 1, 1, 1, 1

the instruction   ffffffff ffffffff ffffffff ffffffff
this code         fffffffe fffffffe fffffffe ffffffff
```

Every lane but the lowest is off by one, because the borrow crossed. That is
worse than `__asm_neg` would have been: a call to an undefined function is
visibly unknown, and this produced a plausible answer and was scored **covered**
by COV-01.

`mvn v0.16b, v1.16b` reaches `translateMov`, which negates at the register
width — and that one is *right*, because a bitwise NOT does not cross lane
boundaries. It stays where it is. The difference between the two is the whole
point: `ABS` and `NEG` carry, `NOT` does not.

### `dup` moved instead of broadcasting

Both forms reached `translateMov`:

```
dup v0.4s, w1        lane 0 got w1, lanes 1..3 got zero
dup v0.4s, v1.s[2]   the whole source register was copied
```

Neither broadcast anything.

### One capstone id, two instructions, told apart by the destination

Fixing that broke three passing tests, and the reason is the fourth instance in
this branch of a capstone id not meaning what its name says — this time in the
other direction. `mov s0, v1.s[0]` is reported as **`ARM64_INS_DUP`**, the same
id as `dup v0.4s, w1`. It is the same encoding. The two are distinguishable
only by whether the *destination* has a vector arrangement:

```
dup v0.4s, w1      [0 v0 vas=4S  vi=-1]     broadcast
mov s0, v1.s[0]    [0 s0 vas=0   vi=-1]     extract one lane
```

Diverting on the id alone sends the scalar form — which `translateMov` already
handled correctly — to a translator that cannot express it. The diversion tests
the destination, and a test pins the scalar form so that the id-only version
fails.

### The `2` suffix is a destination half, not an operation

`xtn v0.8b, v1.8h` writes 64 bits and zeroes the top half of the register, the
way every D-form write does. `xtn2 v0.16b, v1.8h` writes the **top** 64 bits
and leaves the bottom alone. A compiler emits the pair back to back to narrow
two full registers into one, so translating `xtn2` as `xtn` destroys the half
the previous instruction just produced — and the test for it sets the
destination's low half to a sentinel and requires it to survive.

### The widening adds exist for the bit a narrow implementation discards

`uaddl v0.8h, v1.8b, v2.8b` extends each byte to a halfword *before* adding, so
the sum of two full-range lanes cannot overflow. That is the entire reason the
instruction exists. `0xff + 1` is `0x0100` here and `0x00` at eight bits, and
`saddl` on the same operands is `0`, because `0xff` is `-1`.

### `cnt` and the emulator

`llvm.ctpop` on a vector is the obvious expression and the emulator's intrinsic
handler cannot execute it — it calls `getIntegerBitWidth()` on the result type,
which asserts for a vector. So `cnt` is the classic SWAR sequence instead:
vector adds, ands and shifts, all of which the emulator already does. The
translation is a little longer and it is testable, which the intrinsic would
not have been.

### Falsification

Nine mutations, each reverted alone, each rebuilt and run; all nine fail:

| mutation | result |
| --- | --- |
| `neg` back to a 128-bit register negate | 1 test fails |
| `DUP` back to `translateMov` | 2 tests fail |
| `DUP` diverted on the id alone | 4 tests fail |
| `xtn2` writes the low half | 1 test fails |
| the widening operations all unsigned | 1 test fails |
| `rev16`/`rev32`/`rev64` as a whole-register byte swap | 3 tests fail |
| `CNT`'s SWAR loses its last fold | 1 test fails |
| `shrn` drops its shift | 1 test fails |
| the 18 dispatch entries back to `nullptr` | 11 tests fail |

### Where it leaves ARM64

```
                 static
after Batch G    0.9935
after Batch M    0.9941
after Batch N    0.9945     20,930 unmodelled -> 19,712
```

What remains needs registers this translator does not have, or is opaque:

```
st1b 4620  ld1b 2688  whilelo 588  cnt{b,d}     SVE: Z and P registers
svc 4206   brk 339                              opaque by nature
movi 1557                                       Capstone reports a raw imm8
                                                and a shift, not the expanded
                                                lane value, across ~10 encodings
ldg 1302  st2g/stz2g 924  gmi/irg 756           MTE: an allocation-tag model
```

ARM64 is at the limit of what the current register model can express. The next
real gain there is a Z/P register file for SVE, which is the same size of job
as YMM/ZMM for x86-64's AVX — the largest remaining item on any architecture,
at ~150k occurrences.

C2L-01 floor: Arm64 519 → 534. 5,030 tests.

---

## Batch O — the ARMv6 parallel instructions, and the flags that were the point

`uqsub8` (1,554), `uadd8` (840) and `sel` (840) were what remained on ARM after
the bitfield batch, and Batch F's note said they "come together with a GE-flag
model or not at all". That turns out to be exactly right, and the reason is
worth stating: **without the flags these are an ordinary wrapping add done four
times, and `sel` — which reads nothing else — cannot be translated at all.**

### Four byte lanes in a general-purpose register

ARM needs no NEON register for these. The lanes are the bytes of `r0`..`r14`,
which is why hand-written ARMv6 string routines are built out of them. Three
variants of every operation, differing only in what happens at the lane
boundary:

```
plain        wraps, and RECORDS what happened in the GE flags
saturating   clamps to the lane's range, sets no flags
halving      keeps the bit that would have been lost, sets no flags
```

and two lane orders: the straight forms operate lane against lane, the exchange
forms (`ASX`, `SAX`) take the *other* half of the second operand and then add
one lane and subtract the other. Thirty-seven instructions, one function.

### The GE flags

Four `i1` registers at ids past `ARM_REG_ENDING`, the same way `ARM_REG_CPSR_N`
and `ARM_REG_TPIDRURO` already are. Four separate bits rather than a nibble of
a wider register, for the reason PowerPC's condition register needed: a
four-bit register that nothing reads is a second representation to keep in step
with, and the one this translator can act on is the bit.

What sets them is not one rule but three, and the mnemonics name them:

```
uadd8   GE[n] = lane n carried out
usub8   GE[n] = lane n did NOT borrow, i.e. unsigned a >= b
sadd8   GE[n] = the lane's SIGNED result is non-negative
```

The unsigned-add and unsigned-subtract rules are different questions with the
same shape, and a test where one lane is `0x80 + 0x80` separates the signed
rule from the unsigned one — it carries out *and* is negative.

The halfword forms set the flags **in pairs**: four flags, two lanes.

### Two bits wider, not one

The arithmetic is done wider than the lane so the carry, the borrow and the
halving bit are all still there to be read. One extra bit is not enough, and
the reason is the saturating forms: `0xff + 0xff` is 510, which does not fit a
**signed** nine-bit value, so every comparison against the saturation bound
would read it as −2 and clamp upward instead of downward. Two bits hold every
case — an unsigned sum reaches 510 of a signed 511, an unsigned difference
reaches −255, and the signed forms are narrower than both.

That was in the first version of this, found by working the ranges out on
paper rather than by a test, and there is now a mutation for it: reverting the
width to `laneBits + 1` fails two tests.

### Eight tests were asserting the gap

The suite had tests for `uqadd8`, `uqsub8`, `uqadd16`, `uqsub16`, `uqasx`,
`uqsax`, `uhadd8` and `sel` that all read

```cpp
EXPECT_JUST_VALUES_CALLED({
    {_module.getFunction("__asm_uqadd8"), {0x1234, 0x5678}},
});
```

with `{ARM_REG_R0, ANY}` for the result. They pinned the pseudo-assembly call
as if it were the specification. All eight are rewritten to assert arithmetic.

Two of the replacements were wrong on their first run — the exchange pair, where
I wrote the expected values out by hand and took the wrong half of the second
operand. The code was right and the tests were wrong, which the falsification
step would not have caught, because a wrong test that fails is simply a failing
test. The values are computed now, which is what the rest of this branch does
and what I should have done there.

### Falsification

Nine mutations, each reverted alone, each rebuilt and run; all nine fail:

| mutation | result |
| --- | --- |
| the GE flags are never written | 14 tests fail |
| `usub8`'s GE becomes the carry test | 6 tests fail |
| the halfword forms set one flag each instead of a pair | 6 tests fail |
| the exchange forms stop exchanging | 8 tests fail |
| the saturating forms wrap | 14 tests fail |
| the halving forms do not halve | 2 tests fail |
| the wide type back to `laneBits + 1` | 2 tests fail |
| `SEL` ignores the GE flags | 4 tests fail |
| the 37 dispatch entries back to `nullptr` | 32 tests fail |

### Where it leaves ARM

```
                 static
after Batch F    0.9931
after Batch G    0.9967
after Batch O    0.9977     10,690 unmodelled -> 7,456
```

What is left:

```
tbb 1432  tbh 634            table branches -- control flow, a different kind
                             of work from anything in this batch
stcl 924  ldcl 924           coprocessor load and store, opaque
svc 554   udf 256            opaque by nature
vld1.8 546  vst1.8 294       ARM NEON, which needs the D/Q register model
vpadd.i8 336  vldmia 169     ARM64 has (arm64_init.cpp names V0..V31; the
                             32-bit translator does not)
```

### Where all five stand

```
x86_64   0.9586     AVX and AVX-512, needing YMM/ZMM
arm      0.9977     table branches, coprocessor, NEON
arm64    0.9945     SVE, MTE, movi
mips     0.9989     syscall, break, the FPU control word
powerpc  0.9960     traps, AltiVec, cache maintenance
```

Every remaining item on the four non-x86 architectures is either opaque by
nature or needs a register file that does not exist yet. The four are now
within 0.004 of each other, and all four are above where x86-64 sits.

C2L-01 floor: Arm 608 → 624. 5,046 tests.

---

## Batch P — the sub-register mask was a hard-coded table, and it is why AVX
## cannot be modelled

x86-64 is the lowest of the five now that the other four are done, at 0.9586,
and what is left of it is AVX. The recorded reason has been "needs YMM/ZMM in
the register file" since the first x86 batch. **That is not what is missing.**

`x86_init.cpp` already creates `X86_REG_YMM0..31` as `i256` globals and
`X86_REG_ZMM0..31` as `i512`. They have been there all along. What is missing
is that XMM, YMM and ZMM are three **independent** globals: nothing maps one to
another, so an SSE write to `xmm0` and an AVX read of `ymm0` do not see each
other, and `vzeroupper` has nothing to zero.

This is the second time in this branch a recorded blocker turned out to be a
line I had not read — the first was `_id2regs.resize`, corrected in Batch G.
The lesson from that one was to read the function rather than the declaration;
this one needed reading a different file entirely.

### What actually blocks it

`initializeRegistersParentMapToOther` exists and would take the mapping in one
line. The obstruction is one level down, in `storeRegister`:

```cpp
else if (rt->isIntegerTy(16))
{
    if (l->getType()->isIntegerTy(32))  { andC = irb.getInt32(0xffff0000); }
    else if (l->getType()->isIntegerTy(64)) { andC = irb.getInt64(0xffffffffffff0000); }
}
...
if (andC == nullptr)
{
    throw GenericError("Mask not initialized in storeRegister().");
}
```

A hard-coded three-by-four table — `i8`, `i16` and `i32` children against
`i16`, `i32` and `i64` parents, plus the row for the four high-byte registers —
and **any pair outside it throws**. That covers every general-purpose register
and nothing else. Mapping XMM's parent to YMM would have made every SSE store
throw before it wrote anything.

Every entry in that table is `~((2^childBits - 1) << offset)` at the parent's
width, with `offset` 8 for exactly `AH`/`CH`/`DH`/`BH` and 0 for everything
else. Written that way it is four lines, it is correct for any pair of widths,
and it says what the rule is instead of listing its instances.

Four tests pin the shapes the table had, including the one case that is *not* a
merge: x86-64 zero-extends a 32-bit write to the whole 64-bit register, and
`storeRegister` handles that above the masking path. A generalisation that
reached the mask there would answer `0x11223344deadbeef` for
`mov eax, 0xdeadbeef`.

Falsified by four mutations: dropping the high-byte offset fails 35 tests,
taking the parent's width as the child's fails 37, forgetting to invert the
mask fails 177, and removing the 32-bit zero-extension fails 13.

### What is measured, and what is left

The corpus says which AVX widths matter. Disassembling the static x86-64
corpus and classifying every `v*` instruction by its widest register operand:

```
xmm     2064    13%
ymm    12912    79%
zmm     1456     9%
```

So the VEX 128-bit forms alone would address an eighth of it; the bulk is YMM.

With the mask generalised, the remaining work is in three parts, and they have
to go together or not at all:

1. **The parent map.** Three lines. Verified locally: it applies cleanly and
   the translator builds.
2. **The test fixture.** Only *parent* registers get globals in this register
   file — in 64-bit mode `RAX` has one and `EAX` does not — so aliasing XMM to
   ZMM means `getRegister(X86_REG_XMM0)` stops resolving, and
   `getRegisterValueUnsigned` asserts on an `i512` besides. The ARM64 suite hit
   the same wall at 128 bits and answered it with `vLow`/`vHigh`; the x86 suite
   needs the same for every SSE test.
3. **The instructions.** `vpcmpeqb`, `vpmovmskb`, `vmovdqu`, `vpaddb`,
   `vpandn`, `vpminub` and the rest, at 128 and 256 bits.

**Why (1) is not committed on its own.** It would alias the registers without
the VEX upper-zeroing rule, and a legacy SSE write would then be visible to an
AVX read that should have seen zeros — turning "unknown" into "wrong", which is
the argument this branch already made about `dcbz`. The mask generalisation is
committed because it is correct on its own terms and is falsifiable on its own
terms; the aliasing is not, until the instructions that depend on it arrive
with it.

C2L-01 floor: X86 2217 → 2229. 5,058 tests.

---

## Batch Q — AVX, and YMM = YMMH:XMM

The previous batch established that the recorded blocker was wrong: YMM and ZMM
globals already exist, and what was missing was any relationship between them
and XMM. It also removed the thing that made the obvious repair impossible —
the hard-coded sub-register mask table.

The obvious repair is still not the one taken here, and the reason is the test
suite rather than the translator: only **parent** registers get globals in this
register file, so aliasing XMM to ZMM stops `getRegister(X86_REG_XMM0)`
resolving, and all 159 XMM references in the x86 suite would have to be
rewritten against ZMM through accessors that do not assert past 64 bits.

The decomposition the hardware manual uses costs none of that:

```
YMM = YMMH : XMM
```

XMM keeps its global, so every SSE translator and every existing test is
untouched. The upper half becomes sixteen new `i128` registers,
`X86_REG_YMM0_HI..YMM15_HI`, at ids past the flag registers — the same pattern
`X86_REG_CF` and `X87_REG_IE` already use. And the three rules that make AVX
and SSE coexist become three things you can write down and test:

```
a legacy SSE write            leaves YMMH alone
a VEX-encoded 128-bit write   zeroes YMMH
vzeroupper                    zeroes every YMMH
```

### The rule that is quiet when you get it wrong

The middle one. A VEX 128-bit write zeroes the upper half of its destination
and a legacy SSE write does not — that difference is the *entire reason*
`vzeroupper` exists, and it is the one thing here that can be wrong without
producing a visibly odd value: the low half is right either way, and the upper
half only matters to the next 256-bit read. Two tests do nothing but separate
them, with the same operands and the same destination:

```
vmovdqu xmm0, xmm1    ->  YMM0_HI becomes zero
movdqu  xmm0, xmm1    ->  YMM0_HI keeps 0xdeadbeef...
```

### `vzeroupper` had nothing to do

9,336 occurrences, and until the upper halves were registers there was nothing
for it to zero. It was a call to an undefined function, which left every
subsequent 256-bit read of those registers wrong rather than unknown.

### What went in

```
vzeroupper, vzeroall
vmovdqu, vmovdqa, vmovups, vmovupd, vmovntdq
vpand, vpor, vpxor, vpandn
vpaddb/w/d/q, vpsubb/w/d/q
vpminub, vpmaxub, vpminsb, vpmaxsb
vpcmpeqb/w/d/q, vpcmpgtb/w/d/q
vpmovmskb
```

Thirty-two dispatch entries, at 128 and 256 bits, with the width taken from the
register operands and anything that does not agree falling back rather than
being answered at the wrong one.

Two lane traps, both with a test shaped around them:

* `vpaddb` and `vpaddd` differ **only** in whether a carry crosses a byte
  boundary, so any operand whose lanes do not carry gives the same answer for
  both. Every lane in the test carries.
* `vpmovmskb` collects the **top** bit of each byte lane. Taking the low bit
  instead agrees exactly on the all-ones and all-zeroes lanes a compare
  produces — which is every lane it normally sees — so the test uses `0x01` and
  `0x80` alternating, where the two readings answer `0x55555555` and
  `0xaaaaaaaa`.

### Falsification

Eight mutations, each reverted alone, each rebuilt and run; all eight fail:

| mutation | result |
| --- | --- |
| the VEX 128-bit write stops zeroing the upper half | 1 test fails |
| `vzeroupper` zeroes the whole register | 1 test fails |
| the YMM halves swapped on load | 4 tests fail |
| `VPANDN` inverts the second operand | 1 test fails |
| `vpaddb` as a whole-register add | 1 test fails |
| `vpmovmskb` takes the low bit of each lane | 2 tests fail |
| `vpminub` becomes signed | 1 test fails |
| the 32 dispatch entries back to `nullptr` | 10 tests fail |

### Where it leaves x86-64

```
                 static     unmodelled
before           0.9586     213,590
after Batch Q    0.9822      92,140
```

121,450 instructions, and the largest single movement of this branch.

What is left is dominated by **AVX-512**, which is a different problem again:

```
kmovd 10435   vmovdqu64 15566   vmovdqa64 3536   vptestmb 2452   vptestnmb 2178
```

Every one of those is EVEX-encoded and takes a **mask register** — `k0`..`k7`,
a register file this translator does not have, and one whose whole purpose is
per-lane predication that the value model would have to carry through every
operation. That is the same shape of job as SVE's Z and P registers on ARM64,
and it is now the largest remaining item on any architecture.

Below it: `pcmpistri` 6,393 (SSE4.2 string compare, which has its own
mini-language in an immediate), `syscall` 4,627 (opaque by nature), and the
`vmovups`/`vpcmpeqb` remainders, which are the ZMM-width forms falling back
correctly rather than being answered at 256 bits.

### Where all five stand

```
x86_64   0.9822     AVX-512 mask registers, pcmpistri, syscall
arm      0.9977     table branches, coprocessor, NEON
arm64    0.9945     SVE, MTE, movi
mips     0.9989     syscall, break, the FPU control word
powerpc  0.9960     traps, AltiVec, cache maintenance
```

C2L-01 floor: X86 2229 → 2262. 5,091 tests.

## Batch R — the opmask registers were already there, the instructions were not

### The recorded blocker was wrong, again

Batch Q ended by writing that AVX-512 needed "a **mask register** — `k0`..`k7`,
a register file this translator does not have." That is false. `x86_init.cpp`
has carried `{X86_REG_K0, i64}` through `{X86_REG_K7, i64}` in the type map,
and `x86.cpp` has carried eight `createRegister(X86_REG_K0..K7, _regLt)` calls
under the comment `// Opmask registers (AVX-512).`, for as long as this branch
has existed.

This is the second time in three batches that a blocker I recorded turned out
to be a thing that was already present. Batch P made the same correction about
the YMM globals. The shape is identical both times: **the note says "missing"
where the truth is "present but unreached"**, because the evidence for it was
a pseudo-assembly call in the output rather than a reading of the register
file. A `__asm_kmovd` call proves that nothing translated `kmovd`. It does not
say why, and I twice guessed the most expensive possible reason.

What was actually missing was every instruction. All 41 opmask ids present in
the dispatch table were `nullptr`; ten more that capstone emits — `KADDB/W/D/Q`,
`KTESTB/W/D/Q`, `KUNPCKWD`, `KUNPCKDQ` — were **not in the table at all**,
which reaches the same pseudo-assembly path by a different route and is
therefore invisible to any check that reads the table for `nullptr`.

### R-1: the width is in the mnemonic, and capstone will not tell you it

Capstone reports `size = 2` for every opmask operand, for every instruction:

```
kmovq k1, rax   ops=2: REG(k1,sz=2) REG(rax,sz=8)
kmovb k1, eax   ops=2: REG(k1,sz=2) REG(eax,sz=4)
kmovd k1, k2    ops=2: REG(k1,sz=2) REG(k2,sz=2)
```

`KMOVB`, `KMOVW`, `KMOVD` and `KMOVQ` are four different instructions that
differ only in how many bits they move, and the operand carries the same `2`
for all four. A translator that took the width from the operand — which is
what every other x86 translator in this file does, correctly — would make them
one instruction. `maskWidth()` therefore switches on the id, across all 51.

The tests pin this down per width rather than trusting one: `kmovb k1, eax`
with `eax = 0x12345678` must answer `0x78`, where `KMOVW` answers `0x5678` and
`KMOVD` `0x12345678`.

### R-2: these instructions zero the destination's upper bits

`k0`..`k7` are i64 globals. Every opmask instruction writes its own width and
**clears everything above it** — `kmovw k1, k2` clears `k1[63:16]`. That is the
opposite of the sub-register rule the rest of x86 uses, where a narrow write
merges into the parent, and it is the rule a reasonable translator would reach
for by habit.

Every test that writes a mask register pre-loads it with `0xffffffffffffffff`,
so a merging implementation answers `0xffffffffffff1234` where the correct
answer is `0x1234`. That one mutation fails ten tests.

### R-3: `KANDN` and `KUNPCK` both depend on which operand is `VEX.vvvv`

Intel writes these three-operand forms as `DEST, SRC1, SRC2` where SRC1 is the
`VEX.vvvv` operand — the **second** one listed — and SRC2 is the r/m operand.
For the commutative ones (`KAND`, `KOR`, `KXOR`, `KXNOR`, `KADD`) the order
does not matter and no test could detect it. For two of them it does:

* `KANDN k1, k2, k3` is `~k2 & k3`, not `k2 & ~k3`. With `k2 = 0xf0f0` and
  `k3 = 0xff00` the two readings answer `0x0f00` and `0x00f0`.
* `KUNPCKBW k1, k2, k3` puts **k2 in the high byte**. With `k2 = 0xaa` and
  `k3 = 0x55` the two readings answer `0xaa55` and `0x55aa`. A test using the
  same value for both operands would pass either way.

### R-4: `KSHIFTL`/`KSHIFTR` do not mask the count

The count is the full `imm8` and is not taken modulo the width: `kshiftlw` by
16 or more produces **zero**, where every general-purpose x86 shift would mask
the count to `count & 15` and shift by nothing. Two consequences:

* An implementation that reused the GPR shift path would answer `0x1234` for
  `kshiftlw k1, k2, 16` instead of `0`.
* An LLVM `shl i16 %x, 16` is **poison**, so the zero has to be produced
  rather than computed. The count is a literal, so this is a decision at
  translation time and not a `select`.

### R-5: `KORTEST` compares against all-ones *at the instruction's width*

`KORTEST` sets ZF when the OR is zero and CF when it is all ones — `0xff` for
`KORTESTB`, `0xffff` for `KORTESTW`, `0xffffffffffffffff` for `KORTESTQ`. The
test uses `k1 = 0x10f0, k2 = 0x000f`, where the byte-wide OR is `0xff` (CF set)
and the word-wide OR is `0x10ff` (CF clear), so a width-confused implementation
is visible in the flag rather than only in the value.

`KTEST` is the pair that is easiest to get subtly wrong, because ZF and CF come
from two different expressions over the same two operands:

```
ZF = ((SRC2 & SRC1) == 0)
CF = ((SRC2 & ~SRC1) == 0)
```

Three mistakes are available — swapping the flags, negating the second operand
instead of the first, and both — and a single test case cannot separate them.
Two cases do: `k1 = 0xff00, k2 = 0x00ff` gives `ZF=1, CF=0`, and
`k1 = 0xffff, k2 = 0x00ff` gives `ZF=0, CF=1`. The second is the one that rules
out negating the wrong operand.

Both clear OF, SF, AF and PF, which is a separate mutation and fails seven
tests on its own.

### Falsification

Eleven mutations, each reverted alone, each rebuilt and run. All eleven fail
(the counts below are distinct tests, not gtest's doubled `[ FAILED ]` lines):

| mutation | result |
| --- | --- |
| a mask-register write merges instead of zeroing | 10 tests fail |
| the width comes from capstone's operand size | 17 tests fail |
| `KANDN` negates the r/m operand instead of `vvvv` | 1 test fails |
| `KUNPCK` halves swapped | 3 tests fail |
| the shift count masked like a GPR shift | 2 tests fail |
| `KORTEST` ZF and CF swapped | 4 tests fail |
| `KTEST` ZF and CF swapped | 2 tests fail |
| `KTEST` CF negates the second operand | 1 test fails |
| the test flags leave OF/SF/AF/PF alone | 7 tests fail |
| `KSHIFTR` is arithmetic | 1 test fails |
| the `KMOV` dispatch entries back to `nullptr` | 12 tests fail |

Keystone 0.9.2 does not assemble any of these, so every test uses the encoding
`llvm-mc` produces, each one checked against capstone's decode before it was
written into a test.

### Where it leaves x86-64

```
                 static     unmodelled
before           0.9822      92,140
after Batch R    0.9845      79,935
```

`kmovd` (10,435), `kortestd` (804) and `kmovq` (588) have left the list
entirely. The largest remaining entry is now `vmovdqu64` at 6,674.

### What this does not do

It models the **mask registers**, not **masking**. An EVEX-encoded
`vpaddb zmm1{k1}{z}, zmm2, zmm3` still falls back, and will until the value
model carries per-lane predication through every vector operation. What Batch R
buys is that the masks those instructions consume are now real values with real
provenance rather than the return value of an opaque `__asm_kmovd` call — which
is the difference between a decompiled loop whose bound is visibly derived from
a comparison and one whose bound comes from nowhere.

The remaining x86-64 list is now led by the 512-bit-wide moves and compares
(`vmovdqu64`, `vmovdqa64`, `vpcmpeqb`, `vptestmb`), which need ZMM =
ZMMH:YMMH:XMM — the same decomposition Batch Q did one level down — and by
`pcmpistri` at 6,393, which needs no new register at all.

### Where all five stand

```
x86_64   0.9845     512-bit moves and compares, pcmpistri, syscall
arm      0.9977     table branches, coprocessor, NEON
arm64    0.9945     SVE, MTE, movi
mips     0.9989     syscall, break, the FPU control word
powerpc  0.9960     traps, AltiVec, cache maintenance
```

C2L-01 floor: X86 2262 → 2370. 5,199 tests.

## Batch S — ZMM = ZMMH:YMMH:XMM, and the sixteen registers EVEX added

### What the remainder actually was

Batch R left x86-64 at 0.9845 and I wrote that what remained was led by
`vmovdqu64` at 6,674 and the other 512-bit moves. That was half right. Running
objdump over the corpus and grouping the fallbacks by *shape* rather than by
mnemonic gave a different picture:

```
a  operands on registers 16..31 only   549   40%
b  512-bit operands                    351   26%
c  an opmask register as destination   335   24%
d  a write mask or {z}                 139   10%
```

The largest group was not the width at all. It was `%ymm16`, `%ymm17`,
`%ymm19` — 256-bit operations, already fully modelled, on the sixteen
registers only EVEX can name. `isYmmRegister()` read
`X86_REG_YMM0 <= r && r <= X86_REG_YMM15`, which is correct for AVX2 and wrong
the moment a glibc string routine uses `ymm16`.

### S-1: the decomposition, one level up and across all thirty-two

Batch Q established YMM = YMMH:XMM so that a legacy SSE write and an AVX read
of the same register see the same bits. Batch S extends that:

```
ZMMn = ZMMn_HI(511:256) : YMMn_HI(255:128) : XMMn(127:0)      n = 0..31
```

48 new synthetic globals — `YMM16_HI`..`YMM31_HI` and `ZMM0_HI`..`ZMM31_HI`.
The standalone i256 `X86_REG_YMMn` and i512 `X86_REG_ZMMn` globals were
already in the register file and remain there; nothing in the translator reads
or writes them any more, which is the point. Holding `zmm3` in its own i512
global would put `xmm3` and `zmm3` in different storage — the exact bug the
decomposition exists to prevent, and the one mutation that fails 20 tests.

For registers 16..31 there is no legacy alias to reconcile, so the
decomposition buys nothing there except that it is the same rule.

### S-2: the fallback was reading a register nothing writes

This one was found by a test I wrote for something else, and it is the
sharpest finding of the batch.

`translatePseudoAsmGeneric` loads each operand so it can pass it to the
`__asm_*` call. For a `zmm1` operand that is `loadRegister(X86_REG_ZMM1)`,
which reads the i512 global — while every *translated* instruction now writes
the three slices. So an instruction this translator declines to model would
read a register that is permanently zero, and the output would look exactly
like one that had been read correctly: a call with an argument, no warning,
no pseudo-assembly marker on the value itself.

The fix is in `loadRegister`/`storeRegister` rather than in the vector helpers,
so that every path — modelled or not — sees the same storage. Two new methods,
`loadWideVectorRegister` and `storeWideVectorRegister`, assemble and split.

### S-3: capstone does not tell you an operand is a broadcast

```
vpaddd zmm2, zmm1, dword ptr [rdi]{1to16}
  ops=3  R(zmm2,bcast=0)  R(zmm1,bcast=0)  M(sz=4,bcast=0)
```

`avx_bcast` is **zero**. The `{1to16}` appears in `op_str` and nowhere in the
structured detail. The only evidence is the operand's `size`: four bytes,
where the instruction operates at sixty-four.

The vector path took its width from the *register* operands and then read that
many bits of memory — so this was a live mistranslation before Batch S, not
one it introduced: an EVEX `vpaddd ymm2, ymm1, dword ptr [rdi]{1to8}` on
registers 0..15 reached it. The fix is to require every memory operand to be
the width the registers agreed on, and otherwise decline.

Two checks now enforce that, one in `avxWidth` and one in `loadVectorOp`.
They are redundant: removing either alone changes nothing, and the test only
fails when both go. That is worth stating rather than hiding — the falsification
run reported two green mutations until I removed both together.

### S-4: the EVEX modifiers, and which guard is actually load-bearing

A write mask reaches capstone as an **extra operand**, not a flag:

```
vmovdqu8 zmm1{k2}, [rdi]      ops=3   R(zmm1) R(k2) M(sz=64)
vmovdqu8 zmm1{k2}{z}, [rdi]   ops=3   R(zmm1) R(k2,ZMASK) M(sz=64)
vpaddb   zmm3{k4}, zmm2, zmm1 ops=4   R(zmm3) R(k4) R(zmm2) R(zmm1)
```

So the operand-count checks the translators already had reject every masked
form on their own. `hasEvexModifier()`'s opmask branch is therefore a second
lock on a door that is already shut — the mutation that disables it fails
nothing, and I am not going to claim otherwise.

Its `{sae}` and embedded-rounding branch is different:

```
vaddps zmm3, zmm2, zmm1, {rn-sae}   ops=3   R R R   sae=1 rm=1
```

Three plain register operands, no mask, nothing about the instruction's shape
to distinguish it from the ordinary form. Only `avx_sae`/`avx_rm` do, and
suppressing exceptions while pinning the rounding mode is not what an IEEE
`fadd` computes. That branch was dead too, because no floating-point AVX
instruction was wired — so this batch wires `VADDPS`/`VSUBPS`/`VMULPS`/`VDIVPS`
and their double forms, which makes the guard load-bearing and gives the
packed-float arithmetic at 128, 256 and 512 bits along with it.

### S-5: the scalar moves are not narrow whole-register moves

`VMOVD` and `VMOVQ` write one element and **clear everything above it**,
including the register-to-register `vmovq xmm2, xmm1` whose entire purpose is
to take the low 64 bits and zero the rest. Routing them through the
whole-register mover would copy 128 bits for that form and leave stale upper
halves for the others. `VMOVD` moves 32 bits and `VMOVQ` 64, and the test that
separates them has to use a *memory* operand — with a `%esi` source both
widths answer the same, because esi is 32 bits either way.

### Falsification

Fifteen mutations, each reverted alone, rebuilt and run:

| mutation | result |
| --- | --- |
| zmm3 held in its own global, not xmm3's | 20 tests fail |
| the YMM predicate stops at fifteen | 1 test fails |
| ZMM not recognised as a vector register | 4 tests fail |
| a 512-bit load drops the top 256 | 4 tests fail |
| a 256-bit write leaves 511:256 alone | 2 tests fail |
| a 128-bit write leaves everything above alone | 1 test fails |
| the broadcast width check, **both** guards | 1 test fails |
| the EVEX modifier guard off | 1 test fails |
| `vzeroupper` leaves the ZMM quarter | 1 test fails |
| `vmovq` reg-to-reg copies the whole register | 1 test fails |
| `vmovd` writes sixty-four bits | 1 test fails |
| a wide vector register reads its own dead global | 1 test fails |
| the EVEX move ids back to `nullptr` | 2 tests fail |
| `vmovd`/`vmovq` back to `nullptr` | 5 tests fail |

Four came back green on the first run and each was a real gap in the tests,
not a fix that did not matter:

* the broadcast check is duplicated, so removing one copy proves nothing —
  fixed by mutating both;
* the opmask guard is shadowed by the operand-count check — stated above
  rather than papered over;
* `vmovd`'s width was invisible with a register source — fixed by adding a
  memory-operand test;
* the dead-global read had no test at all — fixed by adding one that reads the
  IR directly, since the emulator would show a plausible zero either way.

### Where it leaves x86-64

```
                 static     unmodelled
after Batch Q    0.9822      92,140
after Batch R    0.9845      79,935
after Batch S    0.9922      40,369
```

Every `vmov*` entry has left the top twenty-five. What is left is one shape
and one instruction:

```
vpcmpeqb/vptestmb/vptestnmb/vpcmpltub ...  ~13,000   compare INTO a mask register
pcmpistri                                    6,393   SSE4.2 string compare
syscall                                      4,627   opaque by nature
```

The compares are category (c) from the table at the top: a vector comparison
whose destination is `k1` rather than a vector register. Every piece needed
for those now exists — the mask registers from Batch R, the vector values from
Batch S — and it is the next batch.

### Where all five stand

```
x86_64   0.9922     compare-into-mask, pcmpistri, syscall
arm      0.9977     table branches, coprocessor, NEON
arm64    0.9945     SVE, MTE, movi
mips     0.9989     syscall, break, the FPU control word
powerpc  0.9960     traps, AltiVec, cache maintenance
```

C2L-01 floor: X86 2370 → 2433. 5,262 tests.

### A note on the format gate

Batch R was committed with `x86_avx512.cpp` unformatted, and the push gates
caught it. `scripts/check_format.sh --fix` with no `--base` did not cover the
file, because it was still untracked when I ran it. The gate uses
`--base "$(git rev-parse --verify -q '@{upstream}' || git rev-parse HEAD^)"`,
which is the invocation that matters for a commit that adds a file.

## Batch T — capstone returns the wrong instruction id for every EVEX compare

### The finding

This one is not about a missing translator. It is about a dispatch key that
lies, and it is the sharpest instance of WF-01's inverse — one key covering
many instructions — that this branch has found.

Capstone 5.0.9 decodes the EVEX `VPCMP`/`VPCMPU` predicate aliases and returns
`X86_INS_VPCMPB + predicate`. The element width and the signedness do not
enter into it. Decoding all 48 encodings and grouping by the id capstone
returned:

```
1111  X86_INS_VPCMPB       vpcmpequb vpcmpequd vpcmpequq vpcmpequw
1112  X86_INS_VPCMPD       vpcmplt{b,d,q,w} vpcmplt{ub,ud,uq,uw}
1113  X86_INS_VPCMPEQB     vpcmpeqb vpcmple{b,d,q,w} vpcmple{ub,ud,uq,uw}
1115  X86_INS_VPCMPEQQ     vpcmpeqq vpcmpneq{b,d,q,w} vpcmpneq{ub,ud,uq,uw}
1116  X86_INS_VPCMPEQW     vpcmpeqw vpcmpnlt{b,d,q,w} vpcmpnlt{ub,ud,uq,uw}
1117  X86_INS_VPCMPESTRI   vpcmpnle{b,d,q,w} vpcmpnle{ub,ud,uq,uw}
```

Read the last row again. A 512-bit vector comparison comes back as
**`X86_INS_VPCMPESTRI`**, the id of an SSE4.2 packed string compare. Nothing
about the two instructions is related. If `VPCMPESTRI` had ever been wired to
a translator, capstone would have handed it `vpcmpnleb` and it would have been
translated as a string search.

The mnemonic string is right in all 48 cases. So the translator parses
`vpcmp<pred>[u]<size>` out of `cs_insn::mnemonic` and uses the id only to
reach the function. Anything that does not parse — a genuine `vpcmpestri`,
say — falls back, which is how the two stay separated while sharing a key.

Parsing runs back to front: the last character is the element size, a `u`
before it makes the comparison unsigned, and the remainder is the predicate.
No predicate name ends in `u`, so the split is unambiguous.

`VPTESTM`/`VPTESTNM` are the part capstone gets right — eight distinct,
correct ids — so those dispatch normally. Four of them (`VPTESTMB`,
`VPTESTMW`, `VPTESTNMB`, `VPTESTNMW`) were **absent from the dispatch table
entirely**, the same gap Batch R found for `KADD`/`KTEST`.

### T-1: predicates 3 and 7 have no mnemonic

```
vpcmpb k1, zmm2, zmm1, 3    ops=4   R(k1) R(zmm2) R(zmm1) I(3)
vpcmpb k1, zmm2, zmm1, 7    ops=4   R(k1) R(zmm2) R(zmm1) I(7)
```

FALSE and TRUE render as the **base** mnemonic with the predicate in a fourth
operand, and the two are indistinguishable by mnemonic. So the parser has a
third answer besides "predicate P" and "not a compare": "read operand 3".

### T-2: my own guard rejected the destination

`hasEvexModifier()` from Batch S answers "does this instruction carry a write
mask", and it does so by looking for an opmask register among the operands.
For a comparison, operand 0 **is** an opmask register — the destination — so
the guard rejected every instruction in this batch and all sixteen tests came
back reading zero from a pseudo-assembly call.

A compare into `k1` is not a predicated instruction. The guard now takes the
first operand to consider, and the compares pass 1. That is a small fix and it
is recorded here because the symptom — every new test failing identically with
the right code — reads like the translator was never wired, and it was.

### T-3: the mask is one bit per lane, bit 0 for lane 0

`icmp` on an `<N x iW>` gives `<N x i1>`; bitcasting that to `iN` puts lane 0
in bit 0, and the zero-extension into the i64 opmask register clears
everything above the lane count. The test uses `0xfe` repeating rather than a
symmetric pattern, so a reversed lane order answers `0x7f7f...` and is visible.

Lane counts run from 2 (quadwords at 128 bits) to 64 (bytes at 512), and the
128-bit tests pre-load `k1` with all ones so a mask that merged instead of
zero-extending shows up.

### Falsification

Fourteen mutations, each reverted alone, rebuilt and run:

| mutation | result |
| --- | --- |
| the predicate taken from the capstone id | 2 tests fail |
| signedness ignored | 3 tests fail |
| the lane width always bytes | 3 tests fail |
| `le` read as `eq` | 1 test fails |
| `nle` read as `eq` | 1 test fails |
| the base form's predicate not read from the immediate | 2 tests fail |
| the two source operands swapped | 5 tests fail |
| the mask merged instead of zero-extended | 10 tests fail |
| `VPTESTNM` not negated | 1 test fails |
| `VPTESTM` lane width always bytes | 1 test fails |
| the destination mask treated as a write mask | 13 tests fail |
| the compare ids back to `nullptr` | 1 test fails |
| the `VPTESTM` ids back to `nullptr` | 1 test fails |
| a vector destination no longer routed to the AVX2 compare | 2 tests fail |

The first one is the point of the batch: it implements exactly what "trust the
id" computes, and the tests that catch it are the ones named after the
instruction the id claims — `VPCMPLEUB_is_not_the_VPCMPEQB_its_id_claims` and
`VPCMPNLEB_is_not_the_VPCMPESTRI_its_id_claims`.

One mutation came back green on the first run: the one for the mask
zero-extension re-read the register it had just written in the same basic
block, so `old | new` was `new` and the mutation computed the correct answer.
Rewritten to merge with the value loaded beforehand, it fails 10 tests.

### Where it leaves x86-64

```
                 static     unmodelled   kinds
after Batch Q    0.9822      92,140       -
after Batch R    0.9845      79,935      165
after Batch S    0.9922      40,369      133
after Batch T    0.9947      27,572      113
```

```
pcmpistri     6,393   SSE4.2 string compare -- needs no new register
syscall       4,627   opaque by nature
vpxorq        1,722   write-masked
vpaddb        1,954   write-masked
vpcmpeqb      2,123   write-masked
```

Everything with a `vpcmp`/`vptest` name still in the list is the **predicated**
form — `{k2}` or `{z}` — which this declines on purpose. That is category (d)
from Batch S's table, the last one, and the only one that needs the value
model to carry per-lane predication rather than just to produce and consume
masks.

The largest single remaining item on x86-64 is now `pcmpistri`.

### Where all five stand

```
x86_64   0.9947     pcmpistri, syscall, write-masked vector ops
arm      0.9977     table branches, coprocessor, NEON
arm64    0.9945     SVE, MTE, movi
mips     0.9989     syscall, break, the FPU control word
powerpc  0.9960     traps, AltiVec, cache maintenance
```

C2L-01 floor: X86 2433 → 2484. 5,313 tests.

## Batch U — PCMPISTRI, and reading the semantics off the hardware

### Why this one is different

`pcmpistri` was the largest single unmodelled instruction left on x86-64 —
6,393 sites, because glibc's `strlen`, `strcmp`, `strchr` and `strstr` are all
built on it. It also has more behaviour packed into it than anything else in
this branch: a four-field language in the imm8, four aggregation modes, two
element formats, two signednesses, four polarities, two output selections, and
five flags.

Everything in this branch so far has been implemented from the manual and then
falsified with mutations. That is a good method for an instruction whose
behaviour fits in a paragraph. It is not a good method here, because the
failure mode is not "I wrote the wrong operator" — it is "I misremembered
which operand the validity mask applies to in aggregation mode 3", and a
mutation test cannot tell me that my *baseline* is wrong. A test written from
the same misunderstanding as the code passes.

This container is x86-64 and the CPU reports `sse4_2`. So the semantics came
off the hardware instead:

1. A C model of the instruction was written from the manual.
2. It was run against a real `pcmpistri` over **1,760,000** `(operand,
   operand, imm8)` triples — 40,000 random operand pairs across 44 control
   bytes, covering all four aggregations, both formats, both polarities that
   do anything, both output selections, and every flag.
3. **Zero mismatches.** What is implemented is that model.
4. The translator's own IR, run in RetDec's emulator, was then compared
   against the verified model over **88,000** more triples. Zero mismatches.

The expected values in every unit test are the hardware's answers, emitted by
a generator rather than typed.

### The imm8

```
imm8[1:0]  format      00 unsigned bytes  01 unsigned words
                       10 signed bytes    11 signed words
imm8[3:2]  aggregation 00 EqualAny  01 Ranges  10 EqualEach  11 EqualOrdered
imm8[5:4]  polarity    00 positive  01 negative
                       10 masked positive  11 masked negative
imm8[6]    output      0 least significant index  1 most significant
```

Every field is a literal, so all of it is decided at translation time and only
the data is left to run time. The corpus uses four control bytes — `0x1a`,
`0x3a`, `0x12`, `0x02` — but all of them are implemented, because a partial
implementation that answered the wrong aggregation would be worse than the
pseudo-assembly call it replaced.

### U-1: the validity mask has no branch in it

"Implicit length" means each operand ends at its first zero element. Writing
that as a length and comparing against it would need a loop or a `cttz`; it is
cheaper and clearer as a bit trick on the element-is-zero mask `Z`:

```
valid = (Z & -Z) - 1
```

the bits strictly below the lowest set bit. When no element is zero, `Z` is 0,
`0 & -0` is 0, and `0 - 1` is all ones — the no-terminator case needs no
special path. The flags fall out too: `ZF` is `Z2 != 0` and `SF` is `Z1 != 0`,
so the numeric length is never computed at all.

### U-2: one aggregation can zero its invalid elements and three cannot

For **EqualAny**, the invalid elements of the first operand can simply be
zeroed: every *valid* element of the second operand is non-zero by
construction, so a zero can never match one. That turns sixteen per-element
validity tests into one masked AND.

For **Ranges** the same trick is wrong, and the reason is worth recording. A
signed pair whose high element was zeroed becomes the range `[low, 0]`, which
still matches every negative value. So each pair carries its own validity bit.
This is the kind of thing the differential oracle catches and a hand-written
test does not: the mutation that removes the pair validity failed **4,066** of
88,000 comparisons and **none** of the fourteen unit tests written before it.

For **EqualEach**, two elements that are both past the end count as **equal**.
That forced true is what makes a negative-polarity `EqualEach` — which is
`strcmp` — answer "the strings match" with every bit clear.

For **EqualOrdered**, an invalid element of the first operand ends the
comparison: everything from there on counts as matching, which is what turns a
prefix comparison into a substring search.

### U-3: the tests that had to be added afterwards

The mutation driver found fourteen ways to get this wrong. Nine were caught by
the fourteen unit tests generated from the oracle. **Five were not**, and every
one of the five was a real defect the differential test failed thousands of
times on:

| mutation the unit tests missed | differential failures |
| --- | --- |
| EqualAny does not exclude the set past its terminator | 7,362 / 88,000 |
| Ranges ignores pair validity | 4,066 / 88,000 |
| masked negative inverts everything | 8,518 / 88,000 |
| ZF and SF swapped | 10,222 / 88,000 |
| Ranges always unsigned | 725 / 88,000 |

`ZF` and `SF` is the instructive one. All fourteen tests had operands that
either both carried a terminator or neither did, so the two flags always
agreed and swapping them was invisible. Seven more cases were generated — from
the oracle, again — to pin each of the five down, and the driver now catches
all fourteen mutations with the committed suite alone.

That is the finding under the finding: **a falsification suite is only as good
as the inputs it was written with**, and an independent oracle is what tells
you which inputs you failed to think of.

### Falsification

Fourteen mutations, each reverted alone, rebuilt and run against the committed
tests: all fourteen fail. Separately, the five listed above were each run
against the 88,000-case differential oracle, which failed on every one.

### Where it leaves x86-64

```
                 static     unmodelled   kinds
after Batch Q    0.9822      92,140       -
after Batch R    0.9845      79,935      165
after Batch S    0.9922      40,369      133
after Batch T    0.9947      27,572      113
after Batch U    0.9959      21,179      112
```

`pcmpistri` has left the list entirely. What is left is led by `syscall`
(4,627, opaque by nature — the number in a register decides what it does) and
then, as after Batch T, the **write-masked** vector operations: `vpxorq`,
`vpaddb`, `vpcmpeqb` and `vptestnmb` with a `{k}` or `{z}` modifier, which
this translator declines on purpose because the value model does not carry
per-lane predication.

### Where all five stand

```
x86_64   0.9959     syscall, write-masked vector ops, vpternlog
arm      0.9977     table branches, coprocessor, NEON
arm64    0.9945     SVE, MTE, movi
mips     0.9989     syscall, break, the FPU control word
powerpc  0.9960     traps, AltiVec, cache maintenance
```

x86-64 is no longer the worst of the five on the static corpus. ARM64 is.

C2L-01 floor: X86 2484 → 2547. 5,376 tests.

## Batch V — on ARM64, b0, h0, s0, d0, q0 and v0 were six registers

### How this was found

x86-64 finished Batch U at 0.9959, which made ARM64 the worst of the five on
the static corpus at 0.9945. Measuring it gave 19,712 unmodelled instructions
across 38 kinds — and most of the list was what I expected: SVE (`st1b` 4,620,
`ld1b` 2,688, `whilelo` 588), `svc` 4,206, MTE (`ldg` 1,302, `st2g` 462).

But `movi` was third at 1,557, and `rev16` (338), `fmov` (210) and `saddl2`
(168) were on it too — and **all four already had a function pointer in the
dispatch table**. COV-01 counts those as covered. They were emitting
pseudo-assembly anyway, because each translator declined the particular
operand shape those forms use. That is the gap PSEUDO-01 exists to see, and it
is the third time on this branch it has paid for itself.

Fixing `addv` then turned up something much larger.

### V-1: the FP/NEON register file was fragmented

`addv b0, v1.8b` writes `b0`. Writing `b0` did not change `v0`. Checking why:

```cpp
void Capstone2LlvmIrTranslatorArm64_impl::initializeRegistersParentMap()
{
	std::vector<std::vector<arm64_reg>> rss =
	{
		{ARM64_REG_W0,  ARM64_REG_X0},
		...
		{ARM64_REG_WSP, ARM64_REG_SP},
		{ARM64_REG_WZR, ARM64_REG_XZR},
	};
```

That is the whole map. W→X, WSP→SP, WZR→XZR. **The floating-point and vector
registers are not in it at all**, so `b0` (i8), `h0` (i16), `s0` (f32),
`d0` (f64), `q0` (f128) and `v0` (i128) were six independent globals for one
hardware register.

Two tests, one in each direction, confirmed it before anything was changed:

```
fmov d0, x1                    left v0 untouched
movi v0.2d, #0xff00ff00ff00ff00  left d0 at zero
```

This is the same bug as the x86 XMM/YMM aliasing that Batches Q and S fixed,
and it is worse in one respect: on x86 it needed AVX to show up, while here it
affects **ordinary compiled floating-point code**. Any sequence that computes
in `d` registers and then moves through a `v` register — which is what every
vectorised loop prologue and epilogue does — was reading storage nothing had
written.

The fix is the parent map plus two changes to `loadRegister`/`storeRegister`:

* A narrow view has to be truncated on read on an INTEGER of the right width
  first. There is no `trunc i128 to double`.
* A narrow view write **zeroes the rest of the register** — the rule for every
  ARM64 sub-register, unlike x86 where an 8- or 16-bit write merges. The
  existing code already zero-extended into the parent, which turns out to be
  exactly right; it simply never had a wide parent to do it to.

### V-2: which exposed a real `ucvtf` bug

The first version of the store path converted the value straight to the
parent's type. `ucvtf d0, w1` then produced

```llvm
%1 = trunc i64 %0 to i32
%2 = bitcast i32 %1 to float     ; reinterprets 123 as 1.7e-43
%3 = fpext float %2 to double
```

— a bitcast where the instruction means a conversion, because the caller's
`eOpConv::UITOFP_OR_FPCAST` was being ignored. Nine tests caught it. The value
is now converted to the **view's** type using whatever the caller asked for,
and only then widened as bits.

### V-3: the emulator will run invalid IR and give a plausible answer

Falsifying the narrow-view read produced a green run. Removing the truncation
entirely leaves `bitcast i128 to double` — a bitcast between types of
different sizes, which is not valid LLVM IR — and **every test still passed**.
Instrumenting the path showed it is reached hundreds of times per run, so this
was not dead code: the emulator evaluated the malformed cast and produced a
number that happened to satisfy the assertions.

So the harness now runs `llvm::verifyFunction` on every translated function
and fails the test if it does not verify. With that in place the same mutation
fails **150 tests**.

That is worth stating on its own: until this commit, *no test in this suite
could tell well-formed IR from malformed IR*. Every value assertion went
through an emulator that does not type-check. A translator that emitted
nonsense of the right approximate shape would have passed.

### V-4: the five forms that had a pointer and fell back

| instruction | sites | why it fell back |
| --- | --- | --- |
| `movi` / `mvni` | 1,599 | `translateMovi` handed any vector arrangement to pseudo-assembly |
| `rev16` / `rev32` | 338 | wired to the NEON lane translator, which declines a scalar |
| `fmov` lane form | 210 | `translateFMov` declined anything touching a vector register |
| `saddl2` and friends | 168 | not wired at all |
| `addv` / `smaxv` / `umaxv` | 42 | not wired at all |

Three details worth keeping:

* Capstone reports `movi`'s RAW imm8 and the shift separately, not the element
  value — except for the `.2d` arrangement, where it reports the expanded 64
  bits and there is no shift. `MSL` shifts **ones** in, not zeroes.
* `rev16` reverses bytes within each halfword: `0x11223344` becomes
  `0x22114433`, not `0x44332211`. Implemented as masked swaps rather than a
  vector `llvm.bswap`, because the emulator reads that intrinsic's operand as
  a plain integer and asserts on a vector type — the wrong IR shape shows up
  as a crash rather than a wrong answer.
* The `2` widening forms differ from the plain ones only in reading the UPPER
  half of their sources, which is invisible unless the two halves differ.

### Falsification

Sixteen mutations, each reverted alone, rebuilt and run. All sixteen fail:

| mutation | result |
| --- | --- |
| a narrow-view write merges instead of zeroing | 314 tests fail |
| a narrow-view read does not truncate | 150 tests fail |
| a narrow view converts to the parent's type | 20 tests fail |
| the `movi` dispatch back to `nullptr` | 16 tests fail |
| `movi` does not replicate across the arrangement | 12 tests fail |
| `rev16` reverses the whole register | 6 tests fail |
| the widening `2` forms read the lower half | 4 tests fail |
| the `addv` dispatch back to `nullptr` | 4 tests fail |
| eight more, one or two tests each | |

The narrow-view read mutation is the one that mattered: it was green until the
harness started verifying the IR.

### Where it leaves ARM64

```
                 static     unmodelled   kinds
before           0.9945      19,712       38
after            0.9951      17,355       32
```

2,357 removed, which is exactly what the five wired forms account for:
`movi`/`mvni` 1,599, `rev16` 338, `fmov` 210, `saddl2` 168, `addv` 42. All six
mnemonics have left the list.

None of that is what makes this batch worth having. The register aliasing was
silently wrong on every instruction that touched a floating-point register,
translated or not, and **PSEUDO-01 cannot see that at all**: it counts
pseudo-assembly calls, and a wrong answer is not a pseudo-assembly call. The
rate moved by 0.0006; the thing that was fixed does not show up in it.

What remains, and why:

```
SVE          8,568   st1b 4,620  ld1b 2,688  whilelo 588  cntd 294  cntb 210  ptrue 168
svc          4,206   opaque by nature
MTE          3,234   ldg 1,302  stz2g 462  st2g 462  gmi 378  irg 378  stzg 126  stg 126
brk, dc        633   a trap, and implementation-defined cache maintenance
cpy/set        294   the ARMv8.8 memcpy and memset instructions
```

What remains is dominated by two things this deliberately does not model:

* **SVE** (`st1b`, `ld1b`, `whilelo`, `cntb`, `cntd`, `ptrue` — about 8,500).
  Not a register-file job like AVX-512 was. ZMM is exactly 512 bits; an SVE
  Z register's width is **implementation-defined and not knowable from the
  instruction stream**, and `cntb`/`whilelo` return values that depend on it.
  Modelling it at a guessed width would be wrong rather than incomplete.
* **MTE** (`ldg`, `stg`, `st2g`, `irg`, `gmi` — about 3,200). `irg` produces
  an architecturally random tag and `stg` writes a tag memory this model does
  not have.

plus `svc` (4,206), which is opaque by nature for the same reason `syscall` is
on x86.

### Unfixed: ARM 32-bit has the same fragmentation, in a harder shape

`src/capstone2llvmir/arm/` has **no parent map at all** — no
`getParentRegister` override — and its type map carries `{ARM_REG_S0, f32}`,
`{ARM_REG_D0, f64}` and `{ARM_REG_Q0, f128}` as independent globals. The same
two-line probe that found the ARM64 bug finds this one:

```
vmov s0, r0        with r0 = 0x12345678
  s0 -> 0x12345678
  d0 -> 0                      (both CS_MODE_ARM and CS_MODE_THUMB)
```

It is not the same fix. ARM64's views nest one-to-one — `b0`, `h0`, `s0`,
`d0`, `q0` are all the low end of `v0` — so a parent map plus a
truncate-and-zero-extend does it. ARM's VFP registers **pair**: `d0` is
`s1:s0`, `d1` is `s3:s2`, and `q0` is `d1:d0`. So `s1` is the *high* half of
its parent, not the low one, and the machinery has to carry an offset as well
as a width. `d16`..`d31` have no `s` views at all.

That is a batch of its own and is not bolted onto this one. Recorded here with
the reproduction so it does not have to be rediscovered.

*Fixed in Batch W, immediately below.*

## Batch W — on ARM, s0 and s1 are d0

### What it is

The finding recorded at the end of Batch V, fixed. `src/capstone2llvmir/arm/`
had no parent-register concept at all -- `loadRegister` loaded the named
global directly -- so `s0`, `s1` and `d0` were three separate globals for two
halves of one register.

It is heavily exercised. Across the ARM half of the static corpus:

```
vldr d, [mem]     9,323      writes a D register
vstr d, [mem]     9,215
vmov gpr, s         684      reads an S register
vldr s, [mem]       467
vstr s, [mem]        47
```

A `vldr d0, [r0]` followed by a `vmov r1, s0` -- the ordinary way a compiler
gets the low word of a double into a general-purpose register -- read a global
nothing had written.

### The mapping, not from memory

`Dn[31:0]` is `S2n` and `Dn[63:32]` is `S2n+1`. That is the ARM ARM's
definition, and since there is no qemu in this container to build an oracle
with, it was confirmed a second way -- from gcc's own register allocation:

```c
float low(double d)  { float f[2]; memcpy(f, &d, 8); return f[0]; }
float high(double d) { float f[2]; memcpy(f, &d, 8); return f[1]; }
```

```
low:   bx lr                      // the answer is already in s0
high:  vmov.f32 s0, s1            // the high half IS s1
       bx lr
```

`low` compiles to nothing at all. The double arrives in `d0` and the float
returns in `s0`, so `s0` is `d0[31:0]` -- the toolchain says so by emitting no
instruction.

### Two things that make this NOT the ARM64 fix

* The views **pair** rather than nest. `s1` sits at bit 32 of its parent, so
  the machinery needs an offset as well as a width. ARM64's `b0`, `h0`, `s0`,
  `d0` and `q0` are all the low end of `v0` and need only a width.
* An ARM sub-register write **merges**. Writing `s0` leaves `s1` alone.
  Every ARM64 sub-register write zeroes the rest. Applying ARM64's rule here
  would silently destroy the other half of every D register on each scalar
  float store -- and the mutation that does exactly that fails 8 tests.

The merge has a visible cost: writing half a register requires reading the
other half, so `vmov s0, r1` now loads `d0` as well as storing it, and two
existing tests had to gain that load in their expectations. That is the
instruction's actual data flow rather than an artefact.

### What is left alone

Q registers. Every translator in `arm.cpp` already sends an operand on a Q
register to pseudo-assembly -- `isScalarVfp()` rejects them explicitly -- so
nothing reads those globals today. Composing `qn` from `d2n`/`d2n+1` belongs
with whatever first models a NEON instruction, and inventing it now would add
a second unexercised mechanism.

`d16`..`d31` have no S views at all and need nothing.

### Falsification

Six mutations, each reverted alone; all six fail:

| mutation | result |
| --- | --- |
| S registers back to their own globals | 26 tests fail |
| `s1` treated as the low half | 12 tests fail |
| the pairing off by one (`sN` maps to `dN`) | 12 tests fail |
| a narrow write zeroes the other half | 8 tests fail |
| the read does not shift down | 6 tests fail |
| the write does not shift up | 6 tests fail |

### Where it leaves ARM

PSEUDO-01 does not move, and cannot: this fixes wrong answers, not
pseudo-assembly calls. ARM stays at 0.9977 on the static corpus. The 23,438
VFP and NEON instructions in that corpus are the measure of what was affected,
and 10,521 of them touch a register through a view.

C2L-01 floor: ARM 624 -> 636. 5,410 tests.

### Did the other three have it too?

Having found the same bug on x86, ARM64 and ARM, the question is whether it is
everywhere. It is not, and the reasons are worth recording so the check does
not have to be repeated:

| arch | overlapping views | status |
| --- | --- | --- |
| x86-64 | XMM ⊂ YMM ⊂ ZMM | fixed in Batches Q and S |
| ARM64 | b/h/s/d/q ⊂ v | fixed in Batch V |
| ARM | s pairs into d, d pairs into q | s/d fixed in Batch W; q not reachable |
| PowerPC | f0..f31 are the high halves of vs0..vs31; v0..v31 are vs32..vs63 | **not reachable** |
| MIPS | f2n/f2n+1 pair into a double when FR=0 | **not reachable** |

PowerPC: all **143** VSX instruction ids in the dispatch table are `nullptr`.
Nothing reads or writes the `VS` globals, so the overlap cannot be observed.
Whoever wires the first VSX instruction inherits this problem and should read
Batch S before starting — `vs0` is `f0` in its high half, which is the same
decomposition x86 needed.

MIPS: the mapping already exists as
`singlePrecisionToDoublePrecisionFpRegister()`, and it is gated on
`cs_insn_group(MIPS_GRP_NOTFP64BIT)` — the FR=0 case, which is the only one
where the pairing applies. Within that it maps an odd register to the whole
even one rather than to its upper half, which would be wrong for a
single-precision write to `f1`. Counting the corpus: **zero** single-precision
operations (`*.s`, `lwc1`, `swc1`, `mtc1`, `mfc1`) name an odd `f` register,
which is what the O32 ABI requires. Not exercised, and left alone rather than
changed on the strength of an argument with no test behind it.

The general shape, across all five: **an overlapping-view bug is only
observable once two different views are both modelled.** x86's went unnoticed
until AVX; ARM64's and ARM's were live the whole time because scalar
floating-point and vector code both were. PowerPC's and MIPS's are latent
because one side of each overlap is not modelled at all.

## Batch X — every x87 FADD popped the stack

### How this was found

Having been caught twice by a capstone id that covers more than one
instruction -- ARM64's `DUP`/`MOV` and x86's whole EVEX `VPCMP` family -- the
question is how many more there are. So: decode 1.5 million random words per
architecture, build the id → mnemonic map, discard the ones that differ only
by a condition-code or hint suffix (which the translators read from
`cs_arm.cc`, not from the mnemonic), and keep only ids that are **wired to a
translator**. That is the precise danger set: a dispatch key that reaches real
code and covers more than one operation.

Most of what came back was benign -- ARM's `vector_data` suffixes, which
`isScalarVfp()` already reads. Two x86 entries were not:

```
WIRED id 15    X86_INS_FADD    fadd faddp
WIRED id 377   X86_INS_MOVD    movd movq
```

### X-1: FADDP has no id of its own

Every other popping x87 instruction gets one:

```
fmulp  -> X86_INS_FMULP (506)      fmul  -> X86_INS_FMUL (504)
fsubp  -> X86_INS_FSUBP (725)      fsub  -> X86_INS_FSUB (723)
fdivp  -> X86_INS_FDIVP (158)      fdiv  -> X86_INS_FDIV (156)
fstp   -> X86_INS_FSTP  (714)      fst   -> X86_INS_FST  (713)
faddp  -> X86_INS_FADD  (15)       fadd  -> X86_INS_FADD (15)
```

`translateFadd` already knew: it detects FADDP by reading the opcode byte,
because that is the only way to tell. And then it gated the pop on the id
instead:

```cpp
bool isFADDP = xi->opcode[0] == 0xDE && ...;   // used for the destination
...
if (i->id == X86_INS_FADD)                     // true for EVERY form
{
    x87IncTop(irb, top);                       // the pop
}
```

Its five siblings all read `i->id == X86_INS_FMULP`, `X86_INS_FSUBP`,
`X86_INS_FDIVP`, `X86_INS_FDIVRP`, `X86_INS_FSUBRP`. FADD is the only one
written against the non-popping id, and it is the only one where that id is
what capstone returns for the popping form.

So `fadd m32fp`, `fadd m64fp`, `fadd st(0), st(i)` and `fadd st(i), st(0)`
**all popped the x87 stack**, and every instruction after one of them was
reading a different register than it thought.

### X-2: asked the hardware, not the manual

This container is x86-64, so the behaviour was measured rather than quoted --
`fnstsw` either side of each encoding, reading TOP out of bits 13:11:

```
fadds m32       TOP 6 -> 6   no pop
faddl m64       TOP 6 -> 6   no pop
fadd st(1),st   TOP 6 -> 6   no pop
fadd st,st(1)   TOP 6 -> 6   no pop
faddp           TOP 6 -> 7   POPS
fmul st(1),st   TOP 6 -> 6   no pop
fmulp           TOP 6 -> 7   POPS
fsub st(1),st   TOP 6 -> 6   no pop
fsubp           TOP 6 -> 7   POPS
```

Only DE pops, and the siblings were already right.

### X-3: the opcode byte alone is not enough either

Gating the pop on `opcode[0] == 0xDE` on its own is wrong for a second reason:
**FIADD m16int is also DE** (`DE /0`). It is a different instruction, it does
not pop, and capstone does give it its own id. So the test is "the id is FADD
**and** the opcode is DE", which is exactly FADDP and nothing else. The
existing `X86_INS_FIADD_m16` test is what caught this -- it had the right
expectation all along and started failing the moment the pop moved onto the
opcode byte alone.

### X-4: four tests encoded the bug

`X86_INS_FADD_d8`, `X86_INS_FADD_dc`, `X86_INS_FADD_d8_c0` and
`X86_INS_FADD_dc_c0` all asserted `{X87_REG_TOP, 0x3}` after starting at 2 --
a pop. One of them carries the SDM line as a comment directly above it:

```
// DC C0+i	FADD ST(i), ST(0)	Add ST(i) to ST(0) and store result in ST(i).
```

which says nothing about popping. The tests were written from the code rather
than from the manual, which is the failure mode falsification cannot catch:
reverting the fix makes them pass again, because they *are* the fix's
mirror image.

### Falsification

Four mutations, each reverted alone; all four fail:

| mutation | result |
| --- | --- |
| the pop gated on the id (the original bug) | 14 tests fail |
| never pops | 6 tests fail |
| `isFADDP` without the id check (catches `fiadd m16`) | 2 tests fail |
| `isFADDP` without the opcode check | 14 tests fail |

The new tests include two consecutive `fadd st, st(1)` instructions, because
the cost of the bug was not the first instruction's answer -- that was right --
but the second one's operands.

### What it does not change

The x86-64 static corpus contains **no x87 FADD at all**: modern glibc is
compiled to SSE, so PSEUDO-01 does not move and could not have found this.
It is live for 32-bit binaries and for anything using `long double`.

`X86_INS_MOVD` covering `movd` and `movq` is the other entry from the scan and
is **not** a bug: capstone reports the MMX forms with distinct ids
(`movd` 377, `movq` 378), and the collision in the random-decode scan is the
64-bit `movd` with REX.W, which Intel itself names `movq` and which is the
same instruction.

C2L-01 floor: X86 2547 → 2559. 5,422 tests.

## Batch Y — MSA reached the scalar translator, and two misnamed types

### Y-1: the same collision, on MIPS

The random-decode scan that found the x87 FADD bug also flagged thirty wired
MIPS ids covering more than one mnemonic. Most were the floating-point
variants — `add`/`add.d`, `movn`/`movn.d`, `div`/`div.d` — and those are fine,
because the translators distinguish them by **operand type** rather than by
id: loading an `F` register yields a double, and `translateAdd` picks
`CreateFAdd` over `CreateAdd` on that basis. It works, and it is worth naming
as the pattern that saved MIPS where the id could not.

MSA is different. `ld.b $w0, 0($a0)` is `MIPS_INS_LD`, the same id as the
scalar doubleword load; `and.v` is `MIPS_INS_AND`; `sll.b` is `MIPS_INS_SLL`.
The only thing separating them is that the operands are `W` registers, and
those carry no type the translator can key on.

What happened before: they reached the scalar translator. `ori.b $w31, $w31,
0xb7` reached `translateOr`, which OR'd the immediate into the **low byte** of
the register where ORI.B ORs it into all sixteen byte lanes. There was a test
for that instruction — `issue_633` — and it passed, because its expectation
for the value was `ANY`.

MSA is not modelled, so the answer is the pseudo-assembly call every other
unmodelled instruction gets. One guard at dispatch, on the presence of a W
operand, does it for the whole extension. `and.v` would in fact have come out
right as a 128-bit AND; `ld.b` would not, and deciding which per id is exactly
how the EVEX compare family went wrong on x86.

### Y-2: `i128` was `getInt64Ty` and `i1` was `getInt32Ty`

Chasing why `@w31` printed as `i64` when the type map says `i128`:

```cpp
auto* i1   = llvm::IntegerType::getInt32Ty(_module->getContext());
auto* i32  = llvm::IntegerType::getInt32Ty(_module->getContext());
auto* i64  = llvm::IntegerType::getInt64Ty(_module->getContext());
auto* i128 = llvm::IntegerType::getInt64Ty(_module->getContext());
```

Two of the four locals are misnamed rather than mistyped. Every register
declared `i128` — all 32 MSA vector registers — was **64 bits**, half its
width. Every register declared `i1` — the nine DSP condition, carry and
overflow flags — was **32 bits**.

There was a second, separate typo in the same block: `{MIPS_REG_W31, f128}`
where `W0`..`W30` are `i128`, so the last MSA register was a different type
from the other thirty-one.

Neither is reachable today, which is why none of it was noticed: no DSP
instruction is wired, nothing reads the DSP flags, and MSA now goes to
pseudo-assembly. All three are fixed because they are wrong, and because
whoever models either extension would inherit them.

### Falsification

Four mutations, each reverted alone; all four fail:

| mutation | result |
| --- | --- |
| MSA operands reach the scalar translator again | 1 test fails |
| the MSA register range off by one (drops W31) | 1 test fails |
| MSA registers back to 64 bits | 1 test fails |
| the DSP flags back to 32 bits | 1 test fails |

The register-width test asserts the widths directly rather than through
behaviour, because there is no behaviour to assert: nothing reads these
registers yet, and a test that went through an instruction would be testing
the guard instead.

C2L-01 floor: MIPS 712 → 714. 5,424 tests.

### The id-collision sweep, finished

Decoding 1.5 million random words per architecture, keeping only ids that are
**wired to a translator** and that cover mnemonics differing by more than a
condition-code or hint suffix, gives the complete danger set. All five are now
accounted for:

| arch | wired collisions | how they are separated |
| --- | --- | --- |
| x86-64 | `FADD`/`FADDP`, `MOVD`/`MOVQ` | FADD fixed in Batch X; MOVD/MOVQ is the REX.W form of one instruction, not two |
| x86-64 | the whole EVEX `VPCMP` family | fixed in Batch T, by parsing the mnemonic |
| ARM64 | `DUP`/`MOV` | already handled: the diversion is scoped to a destination with a vector arrangement |
| ARM | 30 ids, all `vector_data` suffixes | handled: every VFP translator guards on `isScalarVfp()`, which reads `cs_arm.vector_data` |
| MIPS | 30 ids: FP variants and MSA | FP by operand type; MSA fixed in Batch Y |
| PowerPC | none | the collisions its scan found are branch-hint suffixes on unwired ids |

Three distinct mechanisms separate a colliding pair correctly, and it is worth
naming which is which, because picking the wrong one is how these go wrong:

* **a structured detail field** — `cs_arm.vector_data`, `cs_arm64.vas`. Best
  when capstone provides one.
* **the operand's type** — MIPS's `add` vs `add.d`, which works because
  loading an `F` register yields a double. Robust and free, when the operands
  differ.
* **the mnemonic string** — x86's EVEX compares, where the id is wrong, no
  detail field distinguishes them, and the operands are identical. Ugly, and
  the only thing that works.

The failure mode in every case found was reaching for the id when none of the
three applies.

## Batch Z — SBB applied its carry twice, on two architectures

### How this was found

Every bug in the last three batches came from a dispatch key that lies. This
one came from a different question: **is the arithmetic right?** Nothing on
this branch had checked, because every test for it was written by the same
person who wrote the code.

This container is x86-64. So: execute each instruction on the CPU with random
operands, capture the result and all six flags, then run the translator's IR
in RetDec's emulator over the same inputs and compare. 26 instructions × 400
random operand pairs = 10,400 comparisons.

The first run reported 127 mismatches. 126 were `sbb64` and one was `neg64`.
`adc64` — the same operation with the carry added rather than subtracted —
had **none**.

### Z-1: the flag helpers apply the carry, and SBB applied it first

```cpp
// translateSbb, before
op1 = irb.CreateAdd(op1, cf);          // fold the carry into the operand
auto* sub = irb.CreateSub(op0, op1);
storeRegistersPlusSflags(irb, sub, {
    {X86_REG_AF, generateBorrowSubCInt4(op0, op1, irb)},   // no cf argument:
    {X86_REG_CF, generateBorrowSubC(sub, op0, op1, irb)},  // these LOAD it
    {X86_REG_OF, generateOverflowSubC(sub, op0, op1, irb)}});
```

The helpers take an optional `cf`; omitted, they load CF from the register
themselves. So the carry went in twice — once folded into `op1`, once inside
each helper.

`translateAdc` next door has always done it correctly: it keeps `op0` and
`op1`, computes `op0 + op1 + cf` separately, and passes `cf` **explicitly** to
every helper. That is why ADC was clean and SBB was not.

### Z-2: and the helper itself was wrong anyway

```cpp
// generateBorrowSubC, before
cfSub  = sub - cf
CF = cf ? ((op0 < cfSub) || (op1 < -1))
        : (op0 < op1)
```

`op1 < -1` unsigned is true for **every** `op1` except all-ones. So with a
borrow in, the answer was 1 almost regardless of the operands. The correct
borrow out of `op0 - op1 - cf` is `op0 < op1 + cf` without wrapping, which
for a carry of 0 or 1 is:

```
(op0 < op1) || (op0 == op1 && cf)
```

written that way precisely because `op1 + 1` wraps to zero when `op1` is all
ones — and that wrap is a real case: `sbb eax, 0xffffffff` with CF set
borrows, and computing it as `op0 < op1 + 1` says it does not.

`generateOverflowSubC` had the matching mistake: it asked the sign question
about `sub - cf`, a value the instruction never produces.

### Z-3: the same bug on ARM

`generateBorrowSubC` had exactly one other caller — ARM's SBC — and that one
uses it **correctly**, keeping its operands and passing the carry explicitly.
It inherited the broken formula anyway:

```
sbcs r0, r1, r2   with r1 = 5, r2 = 3, C clear (borrow in)
  5 - 3 - 1 = 1, which does not borrow, so ARM's C (NOT borrow) is 1
  translator answered 0
```

ARM's SBC is the same ALU operation as x86's SBB with the carry named the
other way round, so the expectations for the new ARM tests come from executing
the equivalent SBB on this CPU and inverting C. The arithmetic is identical;
only the convention differs.

Two existing ARM tests asserted the wrong carry, both with the same comment:

```cpp
{ARM_REG_CPSR_C, false}, // TODO: check, somehow (emul) is it ok?
```

The author did not know, guessed, and wrote the guess down as an expectation.

### Z-4: NEG's overflow was hardcoded to zero

```cpp
{X86_REG_OF, zero}
```

NEG is `0 - op0`, and that overflows for exactly one input: the minimum signed
value, whose negation is itself. `neg rax` with `rax = 0x8000000000000000`
sets OF on the hardware and cleared it here.

### Z-5: which flags the comparison may ask about

The first version of the differential test reported dozens of `imul`
"mismatches" that were nothing of the kind: IMUL leaves SF, ZF, AF and PF
**architecturally undefined**, and comparing against an undefined flag is
comparing against whatever this particular CPU happened to leave behind. The
driver now carries a per-instruction mask — AF undefined for the logicals, OF
undefined for shifts by other than one, only CF for the bit tests, nothing at
all for NOT — and 955 of the 10,400 comparisons fall outside it.

Getting that mask wrong in the other direction would hide real bugs, so it is
written from the SDM's own "undefined" wording rather than from what happened
to differ.

### Falsification

Six mutations, each reverted alone; all six fail:

| mutation | result |
| --- | --- |
| the shared borrow helper back to the old formula | 9 tests fail |
| SBB ignores the carry in for CF | 6 tests fail |
| SBB folds the carry into the operand again | 4 tests fail |
| SBB's AF ignores the carry in | 4 tests fail |
| the shared overflow helper uses `result - carry` | 2 tests fail |
| NEG's overflow back to zero | 1 test fails |

Two of them came back green on the first run and needed inputs the existing
tests did not have: the operand-folding one needs `op1 + carry` to actually
wrap, and the overflow one needs a borrow in **and** a result sitting exactly
on the sign boundary. Both are now pinned from either side.

### After

```
10,400 comparisons against the hardware, 0 mismatches (955 undefined flags skipped)
```

across `add`, `sub`, `and`, `or`, `xor`, `cmp`, `test`, `imul`, `neg`, `inc`,
`dec`, `not`, `shl`, `shr`, `sar`, `rol`, `ror`, `adc`, `sbb`, `bt`, `bts`,
`btr`, `btc` at 32 and 64 bits.

PSEUDO-01 does not move: every one of these was already translated. It was
translated wrongly.

C2L-01 floors: X86 2559 → 2580, ARM 636 → 650. 5,459 tests.

### FLAG-01, so this does not have to be a one-off

The harness that found the SBB and NEG bugs is now
`scripts/ci/check_x86_flags.sh`, run by `standalone-check` on every push. Two
comparisons:

```
arithmetic  34 instruction forms x N random operand pairs
            add sub and or xor cmp test imul neg inc dec not
            shl shr sar rol ror rcl rcr adc sbb bt bts btr btc bsf bsr
            at 8, 16, 32 and 64 bits
condition   every SETcc against every one of the 64 flag combinations.
            Exhaustive, not sampled: 16 x 64 = 1024 cases.
```

The condition-code half is worth more than its size suggests: `Jcc`, `SETcc`
and `CMOVcc` all go through the same sixteen `generateCc*` helpers, so 1,024
cheap comparisons verify **every conditional in x86 output**. It reports zero
mismatches, which is a result rather than an assumption.

Three things make it honest rather than decorative:

* It **skips** on a non-x86-64 host rather than passing. It has to execute the
  instructions.
* It carries a per-instruction mask of the flags the architecture leaves
  **undefined** — IMUL's SF/ZF/AF/PF, the logicals' AF, OF for shifts by other
  than one, everything but CF for the bit tests and the rotates, nothing at
  all for NOT. Without it the first run reported dozens of "mismatches" that
  were only this CPU's leftovers. The mask is written from the SDM's own
  wording, not from what happened to differ, because getting it wrong in the
  other direction hides real bugs.
* `--self-test` corrupts one expected flag and requires **exactly one**
  mismatch back. A comparison that cannot report a difference proves nothing,
  which is the same reason every mutation driver on this branch carries an md5
  guard.

Reverting either fix makes it fail with the exact flag named:

```
flags differ: of   got cf1 pf1 af0 zf0 sf1 of0  want cf1 pf1 af0 zf0 sf1 of1
6800 comparisons, 1 mismatches (805 unmodelled)
FLAG-01: FAIL arithmetic flags disagree with the hardware
```

What it does not cover: MUL/DIV/IDIV (two-register results), SHLD/SHRD, the
x87 and SSE flag-setting comparisons, and every architecture other than x86.
The other four cannot be done this way in this container — there is no qemu —
which is why ARM's SBC had to be derived by mapping x86's SBB rather than
measured directly.

## Batch AA — the instructions whose answer does not fit in one register

### Why FLAG-01 could not see these

FLAG-01 compares one destination register and six flags. That shape is what
made it cheap, and it is also what it cannot express:

```
MUL, IMUL    write a result twice as wide as their operand, split across two
             registers -- and at 8 and 16 bits they write only PART of those
             registers.
DIV, IDIV    read a dividend twice as wide as their operand and write both a
             quotient and a remainder.
SHLD, SHRD   have three operands.
the shifts   are covered at 64 bits and nowhere else, and 64 bits is the one
             width where the count mask and the destination width are both
             the uninteresting case.
```

The last section of the FLAG-01 entry above lists exactly these as what it
does not cover. This is that list, done.

`scripts/ci/x86_wide_oracle.c` and `scripts/ci/x86_wide_compare.cpp` carry the
full 64-bit RAX and RDX through every row, before and after. A translator that
zeroes what it should merge, or merges what it should zero, is a mismatch
rather than something nobody looked at. Four bugs came out of the first run.

### AA-0: the generator could not produce a negative operand

Before any of them, the instrument's own bug. Both oracles drew their random
operands like this:

```c
a = ((uint64_t)random() << 32) ^ random();
```

`random()` returns 31 bits. Bit 31 and bit 63 are therefore clear in *every*
draw — confirmed by OR-ing 200,000 of them together:

```
OR of 200000 draws: 7fffffff7fffffff
bit63 ever set: 0   bit31 ever set: 0
```

A generator that cannot produce a negative operand cannot find a sign bug, and
this one was hiding AA-1: the first sweep of MUL, DIV and the shifts came back
**clean** on an instruction that gets `10 / -3` wrong, because the divisor was
never negative. The fix is five 13-bit chunks, and it is applied to
`x86_flag_oracle.c` as well — the shipped one had the same flaw.

Not the first time on this branch that a green run turned out to be the test
not looking — Batch V needed `verifyFunction` before a mutation would fail at
all, and two Batch Z mutations needed operands that actually wrapped. It is
why the rule is that a falsification run which passes is a result to check,
not a result to report.

### AA-1: IDIV zero-extended its divisor

```c
op1 = irb.CreateZExt(op1, op0->getType());   // for DIV *and* IDIV
```

The dividend is assembled from RDX:RAX as an exact bit pattern, so it needs no
care. The divisor is widened, and widening it with a *zero* extension turns
every negative divisor into a huge positive one:

```
idiv rcx   rax=10  rdx=0  rcx=-3
  hardware   rax=-3  rdx=1
  translator rax=0   rdx=10
```

All four widths. The fix is one conditional, matching the `SDiv`/`UDiv` and
`SRem`/`URem` choices three lines below that were already written this way.

Every IDIV test in the tree divides by a positive number, which is why this
survived: the tests were written from the same reading of the manual as the
code.

### AA-2: one-operand IMUL's overflow test

```c
auto* f = irb.CreateICmpNE(h, 0);
if (i->id == X86_INS_IMUL) { f = f && irb.CreateICmpNE(h, -1); }
```

"The high half is neither zero nor all ones" is not the rule. The rule is that
the high half must be the **sign extension of the low half**, and the two
differ in both directions:

```
imul cl   al=16   cl=8    ->  ax=0x0080   high=0, low is negative    cf=1 of=1
imul cl   al=-43  cl=3    ->  ax=0xff7f   high=-1, low is positive   cf=1 of=1
```

The old test reported neither flag for either. `imul` by a small constant is
ordinary compiler output, so this is not an exotic path — 16 times 8 is enough
to reach it. MUL is unaffected: for the unsigned form "the high half is not
zero" *is* the rule, and a test asserting exactly that now sits next to the
IMUL ones.

### AA-3: SHLD and SHRD masked the count by the processor mode

```c
if (getBasicMode() == CS_MODE_32)      { op2 = SRem(op2, 32); }
else if (getBasicMode() == CS_MODE_64) { op2 = SRem(op2, 64); }
```

The count is masked by the **operand** size: five bits for a 16- or 32-bit
operand, six for a 64-bit one. Reading the mode instead means that in 64-bit
mode `shld eax, ecx, cl` with cl=0xe0 reduces 224 to 32 rather than to 0, and
then shifts an i32 by 32 — which is poison, and poison does not stay where it
is put once the optimiser sees it.

The plain shifts, fifty lines up in the same file, already had it right:

```c
unsigned maskC = op0BitW == 64 ? 0x3f : 0x1f;
```

so the fix is to say the same thing here. These two were the only calls to
`getBasicMode()` in `x86.cpp`; there are now none.

At 16 bits a count above 15 is architecturally undefined, and the mask alone
still leaves a shift of an i16 by 20. That is reduced as well — not to match
the hardware, which is entitled to do anything there, but so that RetDec does
not put poison into IR it then optimises.

### AA-4: none of the nine shift forms wrote its destination at a zero count

The manual says that when the masked count is zero the **flags** are not
affected. It says nothing about the destination, and every one of these
translators reads that silence as "write nothing":

```c
llvm::IRBuilder<> bodyIrb(generateIfNotThen(op1Zero, irb));
```

On x86-64 that is wrong, because a write to a 32-bit register zeroes bits
63:32 whether or not the value changed. Measured, not assumed:

```
shld cl=0   (literal zero)  rax aaaaaaaa11112222 -> 0000000011112222
shld cl=32  (masks to 0)    rax aaaaaaaa11112222 -> 0000000011112222
shld cl=96  (masks to 0)    rax aaaaaaaa11112222 -> 0000000011112222
shl  cl=0                   rax aaaaaaaa11112222 -> 0000000011112222
shr  cl=0                   rax aaaaaaaa11112222 -> 0000000011112222
sar  cl=0                   rax aaaaaaaa11112222 -> 0000000011112222
rol  cl=0                   rax aaaaaaaa11112222 -> 0000000011112222
ror  cl=0                   rax aaaaaaaa11112222 -> 0000000011112222
rcl  cl=0                   rax aaaaaaaa11112222 -> 0000000011112222
rcr  cl=0                   rax aaaaaaaa11112222 -> 0000000011112222
```

All nine, every count that masks to zero. `generateShiftDestinationWrite()`
stores the loaded value back before the branch: a no-op in value terms, which
gets the destination's width right for free. It is skipped for a memory
destination, where there is nothing to widen and a redundant store would be a
new memory write for later analyses to explain.

This one was found by looking, not by the sweep — the sweep only covered SHLD
and SHRD at first, so the other seven were carried by a fix nothing measured.
The oracle now covers shl, shr, sar, rol, ror, rcl and rcr at 8, 16 and 32
bits as well, and reverting the helper fails all nine:

```
by instruction:  shld32=6 shrd32=9 shl32=12 shr32=10 sar32=8
                 rol32=10 ror32=7 rcl32=8 rcr32=8
```

### Two things the harness got wrong about itself

Both were caught by the same habit: read a green result as a claim to check.

**The wide self-test removed its own evidence.** It corrupted a row with
`awk '{$6 = $6 + 1}'`. awk keeps numbers in doubles, so a 64-bit result became
`1.47884e+19`, which does not parse back as an integer — the row was silently
*dropped*, and the comparison reported one fewer comparison and no mismatch. A
self-test that cannot fail is the thing self-tests exist to prevent. It now
rewrites the last digit as text.

**Eight rol/ror "mismatches" were the harness disagreeing with itself.** The
oracle seeds the incoming carry for every single-operand shift; the comparator
seeded it only for RCL and RCR. For a count that masks to zero CF is left
alone, and "left alone" can only be checked if both sides started from the
same value.

### And one test that was right for the wrong reason

`shld eax, ecx, cl` with cl=0x21 gives the correct answer even with AA-3 put
back, because RetDec's emulator reduces a shift amount modulo the width
exactly as the hardware does — so a shift by 33 of an i32 lands on the same
value as a shift by 1. The bug is real in the IR, where `shl i32 x, 33` is
poison, but a test has to *fail*, not merely be right. A second test at
cl=0x20 — five bits of which is zero and six bits of which is 32 — is the one
that fails.

### Falsification

Each fix reverted on its own, with an md5 guard, against both instruments.

Against the hardware comparison (14,400 rows):

```
A_idiv_zext          361   idiv8=123 idiv16=112 idiv32=83 idiv64=43
B_imul_of            152   imul8=49 imul16=48 imul32=37 imul64=18
C_shxd_mask           22   shld32=8 shrd32=14
E_zero_count_write    78   shld32=6 shrd32=9 shl32=12 shr32=10 sar32=8
                           rol32=10 ror32=7 rcl32=8 rcr32=8
```

Every width of IDIV and of IMUL, and every one of the nine shift forms. The
per-instruction tally is printed rather than the first forty failing rows,
because the printed list is capped and a capped list read as a summary is how
AA-4 nearly shipped covering two instructions instead of nine.

Against the gtest suite, which is what runs on a machine that is not x86-64:

```
A_idiv_zext          3 new tests fail   IDIV_r8/r32/r64_negative_divisor
B_imul_of            3 new tests fail   IMUL_r8/r32_high_all_ones_low_positive,
                                        IMUL_r8_overflows_into_the_sign_bit
C_shxd_mask          2 new tests fail   SHLD_r32_count_of_32_is_a_count_of_zero,
                                        SHRD_r32_count_masks_to_zero
E_zero_count_write   4 new tests fail   the two above plus
                                        SHL_r32_zero_count_still_clears_the_top_half,
                                        ROL_r32_count_masks_to_zero_still_writes
```

Twelve tests in all, and the driver checks after restoring that every one of
them passes again — a mutation run that leaves the tree broken would otherwise
report the same "caught" for the wrong reason.

### After

```
14,400 wide comparisons against the hardware, 0 mismatches
20,400 arithmetic comparisons, 0 mismatches
 1,024 condition-code comparisons, 0 mismatches
```

PSEUDO-01 does not move: all of these were already translated. They were
translated wrongly, which is the one thing a coverage number cannot report.

C2L-01 floor: X86 2580 → 2616. 5,495 tests.

### AB-4: the emulator was dropping every vector float intrinsic

Found by the packed test above, which aborted the whole suite:

```
Assertion `Src2.AggregateVal.size() == Src3.AggregateVal.size()' failed.
```

`CVTTPS2DQ` passed and `CVTPS2DQ` did not, and the only difference between
them is that the rounding form asks for `llvm.roundeven.v4f32` first. The
interpreter's floating-point intrinsic block is guarded on

```c
if (I.getType()->isFloatTy() || I.getType()->isDoubleTy())
```

which is scalar-only, so every VECTOR intrinsic fell through to the
unhandled-external path — and that path leaves `GenericValue::AggregateVal`
**empty**. An empty aggregate propagates quietly through the next
instruction, so nothing complained until a vector `select` met one and
compared operand sizes.

Until then, every packed conversion this interpreter ran was reading nothing
and answering with it. Nothing caught it because `CVTPS2DQ` had no test at
all: the translation was written, and the only thing that could have executed
it could not.

The guard is now on the *scalar* type and the computation runs per lane. This
is the Batch V lesson from the other side: there the emulator ran invalid IR
and returned a plausible number, here it declined to run valid IR and returned
an absence. Both look like an answer.

### Still not covered

x87 and SSE comparisons (COMISS, UCOMISS, FCOMI and their double forms) set
EFLAGS and are not in any of the three comparisons. Neither is SHLD/SHRD with
a memory destination, nor BSWAP/XADD/CMPXCHG. And still no architecture other
than x86 — there is no qemu in this container, so ARM, ARM64, MIPS and PowerPC
remain checked by reading rather than by execution.

## Batch AB — the AVX comparisons, and what a conversion answers off the end

### How this was found

The Batch AA entry above closes with a list of what its instruments still
cannot reach, and the first item is the x87 and SSE comparisons. Those read an
XMM register and write flags or a general-purpose register, which is a shape
neither of the other two oracles can express: both pass their operands in
general-purpose registers.

`scripts/ci/x86_sse_oracle.c` and `x86_sse_compare.cpp` are that third shape.
Operands are float BIT PATTERNS from a fixed pool — NaN quiet and signalling,
both zeroes, both infinities, denormals, the integer-conversion boundaries and
ordinary numbers — paired exhaustively, because the row of the comparison
table that matters is the one only a NaN reaches.

The first run reported two things.

### AB-1: the AVX comparison forms were not translated at all

```
vucomisd    NOT TRANSLATED -- falls through to pseudo-assembly
vucomiss    NOT TRANSLATED
vcomisd     NOT TRANSLATED
vcomiss     NOT TRANSLATED
vmovmskps   NOT TRANSLATED
vmovmskpd   NOT TRANSLATED
vcvtsd2si   NOT TRANSLATED
vcvtss2si   NOT TRANSLATED
vcvttsd2si  NOT TRANSLATED
vcvttss2si  NOT TRANSLATED
```

`vucomisd` is what a compiler emits for `a < b` on doubles on any machine
built this decade. RetDec dispatched it, and the nine forms beside it, to
`nullptr` — so an AVX binary got a pseudo-assembly call where an SSE binary
got a comparison.

They are not alone. Of 1,357 dispatch entries, 792 are `nullptr`, and 93 of
those are VEX forms whose SSE twin IS translated. Most of that 93 cannot be
wired up in one line, because the VEX encoding of an arithmetic instruction
takes THREE operands where the SSE one takes two — measured, not assumed:

```
vucomisd xmm0, xmm1        ops=2      <- same shape as ucomisd
vaddsd   xmm0, xmm1, xmm2  ops=3      <- not the same shape as addsd
```

These ten are the subset that is genuinely a drop-in: two operands, and a
destination that is a general-purpose register or nothing at all, so there is
no 256-bit upper half for the VEX form to zero.

### AB-2: and wiring them up exposes the trap

Three of the shared translators branch on the instruction id:

```c
bool isDouble = i->id == X86_INS_UCOMISD || i->id == X86_INS_COMISD;
```

Point `VUCOMISD` at that function and leave the line alone, and every AVX
double comparison silently takes the single-precision path. Nothing crashes.
No coverage number moves, because the instruction IS translated. It is
translated as the wrong operation — 913 wrong answers out of 9,610 when the
mutation was measured.

This is the same bug class as the EVEX compares in Batch T and MIPS MSA in
Batch Y: **one dispatch key covering more than one operation**. It has now
appeared three times, so it gets a check rather than a note. C2L-01 fails if
any V-form shares a translator whose body tests for the SSE id and not the VEX
one. Fourteen shared translators pass it today.

### AB-3: a conversion off the end of the range

The larger of the two. `cvttsd2si` and its seven relatives were a bare
`CreateFPToSI`:

```
cvtsd2si  +inf     hardware 0x80000000   translator 0
cvtsd2si  NaN      hardware 0x80000000   translator 0
cvtsd2si  1.0e24   hardware 0x80000000   translator 0
cvtsd2si  -2^31-ε  hardware 0x80000000   translator 0x7fffffff
```

652 of 4,805 rows. x86 defines the answer for an input that does not fit — a
NaN, an infinity, or a magnitude past the destination's range — as the
**integer indefinite** value, the destination's minimum signed value. LLVM's
`fptosi` calls that case poison, which is a different thing and a worse one:
poison is not a value the decompiler can print, and it does not stay where it
is put once the optimiser sees it.

`generateFpToSiDefined()` range-checks first, feeds the conversion a value
that is in range on every path — so the IR carries no poison at all rather
than poison that happens not to be selected — and selects the indefinite value
back in. Two details are load-bearing:

* The bounds are `-2^(N-1)` and `2^(N-1)`, with a **strict** upper comparison.
  `2^(N-1)-1` is the real limit but it is not exactly representable in binary
  floating point, and 2147483647.0 as a float is 2147483648.0. The strict
  form against the power of two is exact for both widths and both source
  types, and it is the correct boundary for a value that has already been
  rounded.
* Both comparisons are **ordered**, so a NaN fails them and takes the
  indefinite path without needing a test of its own.

It works elementwise on a vector, so `CVTPS2DQ` and `CVTTPS2DQ` get it too.

### Falsification

```
AB1_unwired          vucomisd reported NOT TRANSLATED, 500 fewer comparisons
AB1_body_untaught    913 mismatches   vucomisd=457 vcomisd=456
AB2_bare_fptosi     1295 mismatches   cvtsd2si=185 cvtss2si=146 cvttsd2si=179
                                      cvttss2si=142 vcvtsd2si=174 vcvtss2si=150
                                      vcvttsd2si=175 vcvttss2si=144
```

The middle one is the one worth having. It is the mutation that produces a
translator which runs, terminates, writes plausible flags, and is wrong — the
failure the new C2L-01 check exists to prevent.

### After

```
9,610 comparisons against the hardware, 0 mismatches, 0 untranslated forms
```

C2L-01 floor: X86 2616 → 2664. 5,543 tests.

The comparison fails on an untranslated form as well as on a wrong one,
because an absence read as a pass is how these ten sat unnoticed.

### Still not covered

263 of the 792 `nullptr` entries are not AVX at all, and some are ordinary:
the packed shifts (`PSLLD`, `PSRLQ`, `PSRAW` and the rest), `PSHUFB`, the
saturating packed adds, `PMADDWD`, the `PMOVSX`/`PMOVZX` widening family,
`PACKSSWB`, `PTEST` and `CMPPS`/`CMPPD`. Those are in every vectorised loop
and every optimised string routine. They are a larger batch than this one and
have not been started.

The remaining 83 VEX forms with a translated SSE twin need a three-operand
path and the VEX zeroing rule, not a dispatch entry.

## Batch AC — the packed shifts and the widening moves

### What it is

The Batch AB entry closes with a list of ordinary instructions that are not
translated at all, and the first two groups on it are these twenty. All of
them fell through to pseudo-assembly:

```
psllw pslld psllq psrlw psrld psrlq psraw psrad
pmovsxbw pmovsxbd pmovsxbq pmovsxwd pmovsxwq pmovsxdq
pmovzxbw pmovzxbd pmovzxbq pmovzxwd pmovzxwq pmovzxdq
```

The packed shifts are in every vectorised loop that touches integers; the
widenings are how a compiler turns a `char[]` into arithmetic.

`scripts/ci/x86_vec_oracle.c` and `x86_vec_compare.cpp` are a fourth oracle
shape — XMM in, XMM out — carrying both 64-bit halves of the destination
before and after, so a translator that writes the right value to the wrong
half is a mismatch rather than something nobody looked at.

### AC-1: a shift past the element width is defined, and defined two ways

This is the part that is not transcription. x86 does not leave a count at or
past the element width undefined:

```
psllw by 16   0005000600070008_0001000200030004 -> 0000000000000000_0000000000000000
psllw by 2^40 0005000600070008_0001000200030004 -> 0000000000000000_0000000000000000
psraw by 20   ffff000180007fff_8000000700018000 -> ffff0000ffff0000_ffff00000000ffff
psrad by 32   7fffffff00000001_80000001ffffffff -> 0000000000000000_ffffffffffffffff
```

A logical shift gives zero. An **arithmetic** right shift gives each element's
sign bit broadcast across it. LLVM's `shl` and `lshr` are poison at or past
the width, so the count is clamped to width-1 — which for the arithmetic case
*is* the answer — and the logical cases are selected back to zero.

Two more things the count is not:

* It is **one value for the whole register**, taken from the low quadword of
  the second operand, not one per lane. The upper quadword is not read at all;
  the oracle fills it with noise, and reading it costs 1,871 mismatches.
* It is a full 64 bits. Truncating it to the element width before asking
  "is this too big" reads a count of 2^40 as a shift by zero.

### AC-2: the widenings read the low elements, and how many follows the destination

`pmovsxbq` writes two quadwords, so it reads two **bytes**; everything above
the low sixteen bits of the source is untouched input. Reading the high lanes
instead is 6,000 mismatches out of 10,000 — every row of every widening form,
which is what a wrong shuffle mask looks like. Getting the sign wrong is
quieter: 2,559, only on the PMOVZX half and only where a byte had its top bit
set.

### AC-3: a mutation came back green, and the thing it mutated was dead code

The sixth mutation removed a guard I had added to the emulator's `select`, on
the theory that a scalar condition with vector arms — `select i1 %c, <8 x i16>
%a, <8 x i16> %b`, which is exactly what the shift guard produces — would trip
the per-lane assertion. Removing it changed nothing.

It changed nothing because `visitSelectInst` passes the **condition's** type
to `executeSelectInst`, not the result's:

```c
Type* ty = I.getOperand(0)->getType();
```

so a scalar condition already takes the scalar branch, which is correct. The
guard was unreachable. It has been reverted rather than shipped with a comment
claiming it fixed something.

This is the rule doing its job in the direction it is usually not needed for.
A falsification run that passes is a result to check: usually the check finds
the test is not looking, and this time it found that the code was not doing
anything.

### Falsification

Each decision reverted on its own, with an md5 guard, against 10,000 rows:

```
AC1_no_clamp             381   psraw=190 psrad=191
AC2_no_zero_select      1298   all six logical shift forms
AC3_arith_clamp_zero     515   psraw=255 psrad=260
AC4_widen_high_lanes    6000   all twelve widening forms, every row
AC5_zext_becomes_sext   2559   the six PMOVZX forms only
AC6_count_whole_reg     1871   all eight shift forms
```

### After

```
10,000 comparisons against the hardware, 0 mismatches,
0 untranslated forms, 0 misencoded
```

"0 misencoded" is its own line because the encodings are hand-written. Each is
disassembled and checked against the mnemonic it is supposed to be — a wrong
ModRM would otherwise test one instruction against another's expected answers
and report a translator bug that is really a typo in the test.

C2L-01 floor: X86 2664 → 2709. 5,588 tests.

### Still not covered

45 of the 65 packed-integer instructions grouped in the Batch AB entry: the
saturating adds and subtracts, `PMADDWD`, the multiply family, `PSHUFB` and
the blends, the insert/extract pairs, the horizontal adds, `PACKSSWB` and
friends, and `PTEST`. The oracle shape they need now exists.

## Batch AD — saturation, packing, the packed multiplies and two reductions

### What it is

The twenty instructions the Batch AC entry listed as still not covered, minus
the shuffles and the insert/extract pairs:

```
paddsb paddsw paddusb paddusw psubsb psubsw psubusb psubusw
packsswb packssdw packuswb packusdw
pmullw pmulhw pmulhuw pmulld pmuludq pmuldq
pmaddwd psadbw
```

All twenty fell through to pseudo-assembly. They reuse the Batch AC oracle,
which already had the right shape — XMM in, XMM out, both halves carried.

### AD-1: the whole point of these is the edge, so the generator goes there

Uniform noise almost never saturates. Two random bytes sum past 127 about a
quarter of the time and past 255 almost never, so a random-operand sweep of
`paddusb` would run thousands of rows without once exercising the clamp and
report zero mismatches on a translation that does not have one.

Half of each operand's elements are therefore drawn from the edges of the
element's range — zero, one, the signed and unsigned extremes and their
neighbours. Removing the saturation entirely is then 3,738 mismatches out of
20,000 rather than a handful.

### AD-2: the unsigned forms need a SIGNED comparison

`psubusb` of 1 and 2 is zero, and the way to get there is to widen, subtract,
and notice the intermediate went below zero. Widening with a zero extension
and then clamping with an *unsigned* comparison cannot notice: the negative
intermediate reads as enormous and clamps upward instead.

Both the signed and the unsigned forms therefore clamp with signed
comparisons on the widened value. Widening to twice the element gives room for
every sum and difference of two N-bit values, so the arithmetic itself can
never overflow and only the clamp decides. Using unsigned comparisons instead
costs 4,994 mismatches.

### AD-3: PACKUSWB reads a signed source

The US forms saturate a **signed** word into an **unsigned** byte: -1 becomes
0, 300 becomes 255. Reading the source as unsigned turns -1 into 255 — the
same bit pattern, arrived at backwards, and wrong.

### AD-4: three different answers to "what do you keep"

```
PMULLW PMULLD     the low half, where the product wraps into the element and
                  the signedness cannot be observed at all
PMULHW PMULHUW    the high half, where it is the only thing kept and the
                  signedness is the entire difference between the two
PMULUDQ PMULDQ    the whole product of the EVEN dwords only -- lanes 1 and 3
                  are not multiplied at all
```

Making PMULHUW signed is 500 mismatches and touches nothing else. Reading the
odd dwords instead of the even ones is 991, across both quadword forms.

### AD-5: the two reductions

`PMADDWD` multiplies signed words and adds **adjacent** pairs, so eight
products become four sums. Its one overflowing input is -32768 squared twice,
which wraps to 0x80000000; that is the defined answer, so the addition is left
to wrap rather than saturated.

`PSADBW` sums eight absolute differences per group into the low word of that
group's quadword. Eight byte differences reach 2040, so the accumulator is
sixteen bits and cannot overflow. Dropping the absolute value is 500
mismatches — every row, because a signed difference is almost never the
unsigned one.

### Falsification

Each decision reverted on its own, with an md5 guard, against 20,000 rows:

```
AD1_no_saturation        3738   all eight saturating forms
AD2_unsigned_low_clamp   4994   the saturating forms and all four packs
AD3_pack_src_unsigned    1979   all four packs
AD4_pack_halves_swapped  1970   all four packs
AD5_mulh_always_signed    500   pmulhuw only
AD6_mul_odd_lanes         991   pmuludq pmuldq
AD7_madd_wrong_pairs      500   pmaddwd
AD8_sad_no_abs            500   psadbw
```

AD5 is the one worth having: it touches exactly one instruction, which is what
a signedness bug looks like when the two forms differ only in that.

### After

```
20,000 comparisons against the hardware, 0 mismatches,
0 untranslated forms, 0 misencoded
```

C2L-01 floor: X86 2709 → 2748. 5,627 tests.

### Still not covered

Of the 65 packed-integer instructions grouped in the Batch AB entry, 25
remain: `PSHUFB`, `PSHUFHW`/`PSHUFLW` and the blends; the `PINSR`/`PEXTR`
pairs; the horizontal adds and subtracts; `PTEST`; `PHMINPOSUW`;
`PMULHRSW`; `PMADDUBSW`; `PABSB`/`W`/`D` and the `PSIGN` family.

## Batch AE — PSHUFB, the horizontal forms, and three odd ones out

### What it is

Sixteen more, all falling through to pseudo-assembly:

```
pshufb
phaddw phaddd phaddsw phsubw phsubd phsubsw
pabsb pabsw pabsd  psignb psignw psignd
pmulhrsw pmaddubsw phminposuw
```

Each carries a rule that a plausible-looking translation gets wrong, and each
of those rules is a separate mutation below.

### AE-1: PSHUFB's control is data, and it has a branch in it

The control is the second XMM operand, not an immediate, so the index is only
known at run time and `shufflevector` cannot express it — its mask has to be
constant. `extractelement` with a dynamic index can.

The rule is not a permutation. A control byte with its **top bit set writes
zero** rather than selecting a lane, and only the low four bits of the rest
are an index. Masking with `0x0f` and forgetting the top bit answers with a
byte from the source everywhere the hardware answers zero: 491 mismatches out
of 500 rows.

### AE-2: three outcomes, not two

`PSIGN` negates on a negative control, keeps on a positive one, and writes
**zero** on a control of exactly zero. A `negative ? -a : a` translation is
right two-thirds of the time and silently wrong the rest: 1,009 mismatches
across the three widths.

### AE-3: the minimum signed value is its own absolute value

`pabsb` of -128 is -128. x86 does not saturate it to 127, and neither does
negating and letting it wrap — so the naive translation is the correct one
here, and a translation that saturated "to be safe" is the wrong one. Adding
that safety is 965 mismatches.

### AE-4: the horizontal forms add within, not across

`PHADDW` adds **adjacent pairs inside each operand** — the first operand's
four sums fill the low half of the destination and the second's fill the high
half. It is two independent reductions written side by side, not a lane-wise
operation. `PHSUB` subtracts the second element of each pair from the first,
and getting that backwards is 1,000 mismatches.

### AE-5: rounding, asymmetry, and a tie

```
PMULHRSW    (a*b >> 14) + 1 >> 1. The +1 is the rounding; dropping it is off
            by one on every product whose bit 14 is set -- 408 mismatches.
PMADDUBSW   the FIRST operand's bytes are unsigned and the SECOND's are
            signed. Widening both the same way is wrong whichever way is
            picked -- 500 mismatches, every row.
PHMINPOSUW  unsigned comparison (496 mismatches if signed), and ties take the
            LOWEST index, which falls out of scanning upward with a strict
            comparison. Using `<=` answers with the last of a tie instead:
            245 mismatches.
```

The tie-breaking one is the finding that would be easiest to write and never
notice. It needed a generator that produces equal words often enough to hit
it, which the edge-drawing one from Batch AD does.

### Falsification

Each rule reverted on its own, with an md5 guard, against 28,000 rows:

```
AE1_pshufb_no_top_bit        491   pshufb
AE2_pshufb_index_unmasked    500   pshufb
AE3_phsub_operands_swapped  1000   phsubw phsubd
AE4_phadds_no_saturate       796   phaddsw phsubsw
AE5_pabs_saturates_intmin    965   pabsb pabsw pabsd
AE6_psign_no_zero_case      1009   psignb psignw psignd
AE7_pmulhrsw_no_rounding     408   pmulhrsw
AE8_pmaddubsw_symmetric      500   pmaddubsw
AE9_phminpos_tie_last        245   phminposuw
AE10_phminpos_signed         496   phminposuw
```

Every one lands on exactly the instructions whose rule it breaks and on no
others, which is what a per-instruction tally is for.

### After

```
28,000 comparisons against the hardware, 0 mismatches,
0 untranslated forms, 0 misencoded
```

C2L-01 floor: X86 2748 → 2781. 5,660 tests.

### Still not covered

Of the 65 packed-integer instructions, nine remain, and they are the ones
that do not fit this harness: `PSHUFD`/`PSHUFHW`/`PSHUFLW`, `PBLENDW`,
`BLENDPS`/`BLENDPD` take an immediate, and `PINSR`/`PEXTR` read or write a
general-purpose register. `PTEST` and `PBLENDVB` need the flags and an
implicit XMM0 respectively. None needs a new instrument, only a row format
with an immediate in it.

## Batch AF — the immediate-controlled shuffles and blends

Six more: `PSHUFHW`, `PSHUFLW`, `PBLENDW`, `BLENDPS`, `BLENDPD`. The control is
an imm8, which is part of the **encoding** rather than of the data, so it
cannot vary per row: each (instruction, immediate) pair is its own oracle
entry, named with the immediate appended, and the comparator matches
capstone's mnemonic against the name up to the underscore.

The immediates are chosen to be asymmetric — `0x1b` reverses, `0x4e` swaps
halves, `0xa5` alternates. A control of `0xe4` is the identity and would pass
against a translation that ignored the immediate entirely.

### AF-1: the half forms permute a half and copy the other

`PSHUFHW` permutes the high four words and copies the low quadword through;
`PSHUFLW` does the reverse. Both take two bits per lane out of the same imm8,
indexed from the start of the half they act on. Treating either as a
whole-register permutation scrambles the half that is supposed to be left
alone — 800 mismatches — and indexing the permuted half from lane 0 rather
than from its own base is 400, on `PSHUFHW` only, because for `PSHUFLW` the
base is zero and the two readings coincide.

### AF-2: PSHUFD was already there

It has had `translateSsePshufd` since before this batch. The oracle covers it
at three control bytes and it is correct, so it keeps its own translator and
the new one has no case for it. Adding an unreachable case would be the dead
code Batch AC caught itself shipping.

### AF-3: a set immediate bit selects the SECOND operand

For all three blends. Inverting that is 1,200 mismatches, every row of every
form. `BLENDPS` and `BLENDPD` are floating point only in name — nothing is
interpreted — so they are one operation at two widths and share a translator
with `PBLENDW`.

### Falsification

```
AF1_shuf_whole_register    800   pshufhw_1b/c6 pshuflw_1b/c6
AF2_shuf_index_from_zero   400   pshufhw_1b/c6 only, and that is correct
AF3_blend_bit_inverted    1200   all six blend entries
```

### After

```
13,800 comparisons against the hardware, 0 mismatches,
0 untranslated forms, 0 misencoded
```

C2L-01 floor: X86 2781 → 2796. 5,675 tests.

### A test that was wrong where the translator was right

All five gtest cases written for this batch failed on their first run while
the same instructions passed 13,800 hardware comparisons. The cause was in the
tests: `setXmm()` takes `(reg, hi, lo)` and the oracle supplies `(lo, hi)`, so
every input was reversed. Worth recording because the instinct on a red test
is to look at the code it tests, and here the code was the only part that had
already been measured.

## Batch AG — three ARM64 bugs, found by looking for the x86 ones

### How this was found

Every batch from AA to AF was x86, because x86 is the only architecture this
container can execute. The five bug classes those batches established are not
x86-specific, though, so ARM, ARM64, MIPS and PowerPC were searched for each
of them by reading — with the rule that a finding which cannot be demonstrated
from the code is worse than no finding.

Three of the ARM64 results are in this batch. The rest, and the MIPS/PowerPC
ones, are recorded below as still open.

### AG-1: FCVTZS and FCVTZU had their signedness transposed

```c
case ARM64_INS_FCVTZU:
    op1 = irb.CreateFPToSI(op1, ...);   // the UNSIGNED convert, signed
case ARM64_INS_FCVTZS:
    op1 = irb.CreateFPToUI(op1, ...);   // the SIGNED convert, unsigned
```

Straight transposition, and both ids are dispatched here. `fptosi` and
`fptoui` agree wherever the value fits, which is why it survived: they differ
exactly at the inputs that do not, and there the answer is poison rather than
merely different. `fcvtzs x0, d0` with `d0 = -1.0` was `fptoui double -1.0`.

### AG-2: the emulator cannot see AG-1, and that is why it lasted

The test for this one asserts on the **IR**, not on a value, and the reason is
worth recording. `src/llvmir-emul/llvmir_emul.cpp` implements both
conversions with the same call:

```
executeFPToUIInst  ->  APIntOps::RoundDoubleToAPInt(Src.DoubleVal, DBitWidth)
executeFPToSIInst  ->  APIntOps::RoundDoubleToAPInt(Src.DoubleVal, DBitWidth)
```

The two instructions are indistinguishable to the interpreter. **No value test
running through it can catch a translator emitting the wrong one of the
pair**, on any architecture. The first version of these tests asserted on the
returned register and passed with the transposition put back.

That also means the float-to-integer *saturation* class — bare `CreateFPToSI`
at eight sites across ARM, ARM64, MIPS, PowerPC and the x87 path — cannot be
value-tested here either. It is listed as open below rather than fixed
unfalsifiably.

### AG-3: UXTX and SXTX threw away the top half

```c
case ARM64_EXT_UXTX:
    trunc = irb.CreateTrunc(val, i32);   // copied from the UXTW case above
    return irb.CreateZExt(trunc, ty);
```

"Extend from X" means use all sixty-four bits, so both of these are no-ops.
Both were byte-identical copies of the W forms above them.
`add x0, x1, x2, uxtx` with `x2 = 0x123456789abcdef0` answered
`0x000000009abcdef0`.

Reachability is limited: capstone does not set `ext` when UXTX is paired with
SP, so the common `add x0, sp, x1, lsl #3` never reaches this code. Wrong from
the code without qualification; uncommon in compiler output.

### AG-4: EXTR with a rotate of zero, and a test that hid it

`extr Xd, Xn, Xm, #0` is a legal encoding meaning `Xd = Xm`. The translator
computed `width - 0 = 64` and emitted `shl i64 %Xn, 64` — poison. The
emulator reduces a shift modulo the width, turning it into `shl %Xn, 0`, so
the answer came out as `Xm | Xn`: a wrong **value**, not only bad IR.

There was already a test for exactly this encoding,
`ARM64_INS_EXTR_r_r_r_i_5`, and it passed. It chose `x1 = 0x1111111111111111`
against `x2 = 0x9999999999999999`, and every set bit of the first is also set
in the second — so the spurious OR was invisible. Changing that one constant
to `0x2222222222222222` fails it, and that is the change this batch makes.

`translateNeonExt`, a few hundred lines away in the same file, case-splits
this identical hazard and its comment says why. EXTR was missed.

### Falsification

```
AG1_fcvtz_transposed   2 tests  FCVTZS_is_the_signed_convert, FCVTZU_...
AG2_uxtx_truncates     1 test
AG3_sxtx_truncates     1 test
AG4_extr_no_zero_case  1 test   the existing test, with its constant replaced
```

One decision is deliberately not in that list. The EXTR fix also clamps the
shift amount, not only selects the result, so the IR carries no poison on
either path. That clamp is **not value-observable** — the select already
decides the answer — so a mutation removing it comes back green. It is kept
rather than dropped because poison does not stay where it is put once the
optimiser sees it, and said here rather than left as an unfalsified change.

C2L-01 floor: ARM64 556 → 560. 5,679 tests.

### Still open on the other architectures

Confirmed from the code, not yet fixed:

```
PowerPC  SLW/SRW mask the count to six bits and then shift an i32 by it,
         which is poison for 32..63 where the ISA defines ZERO. translateRotlw
         in the same file handles this and says so in a comment.
PowerPC  SUBFE computes its carry from RA where the value uses ~RA. The three
         sibling translators all pass the complemented operand.
PowerPC  SRAW/SRAWI place the carry at XER bit 29 and store that into an i1
         global, so CA is unconditionally false. Both its tests are commented
         out.
PowerPC  cntlzw, mulhw, mullw, divw and sraw are done at register width, so
         they are wrong on PPC64, which createPpc64() enables.
ARM      the register-controlled shifts mask neither to eight bits nor at the
         operand width; ARM is not modulo, so this is a wrong value and not
         only poison.
ARM      LDRD writes the two loaded words to the wrong registers.
ARM64    LSLV/LSRV/ASRV/RORV do not mask the shift amount.
ARM64    a memory operand's extender is dropped and its index zero-extended,
         so `ldr w0, [x1, w2, sxtw #2]` with a negative index addresses 4 GiB
         away.
all      bare CreateFPToSI at eight sites, per AG-2.
```

## Batch AH — three PowerPC bugs, two of them in the carry

### AH-1: SLW and SRW shift an i32 by up to 63

```c
op2 = irb.CreateAnd(op2, ConstantInt::get(op2->getType(), 0x3f)); // low 6 bits
auto* val = irb.CreateShl(op1, op2);
```

Six bits is the right amount to **read**, and the sixth does not participate
in the shift: `slw` with a count of 32 or more gives zero. Masking to six bits
and then shifting leaves `shl i32 %v, 32` through `shl i32 %v, 63` — poison,
and the poison then feeds `storeCr0()`, so the condition register goes with
it.

`translateRotlw`, forty lines up in the same file, handles this and says so:

```c
// `lshr i32 %w, 32` is poison, and n is not always a constant here -- the
// register form exists -- so the zero case is selected rather than decided.
```

`translateRotateWordMask` does the same. SLW and SRW were the two that did
not. Both of their tests use a count of 16.

### AH-2: SUBFE's carry came out of the wrong sum

`subfe RT,RA,RB` is `~RA + RB + CA`, and CA is the carry out of **that** sum.

```c
auto* op1Neg = generateValueNegate(irb, op1);
auto* val = irb.CreateAdd(op1Neg, op2);          // correct: ~RA
...
storeRegister(PPC_REG_CARRY, generateCarryAddC(op1, op2, irb, carry), irb);
                                              // ^^^ not complemented
```

So the value was right and the carry answered `carryout(RA + RB + CA)`.
`subfe r0, r1, r2` with `r1=1`, `r2=5`, `CA=1` gives `r0=4` either way, and
CA=1 on the hardware against CA=0 here.

The three sibling translators — `subfc`, `subfme`, `subfze` — all pass the
complemented operand. All three existing SUBFE tests use `r1 = 0x2222` and
`r2 = 0x1111`, where both readings give CA = 0.

This is close to the Batch Z shape but not the same: the carry is not applied
twice, it is computed from the wrong operand.

### AH-3: SRAW and SRAWI could not set the carry at all

```c
auto* shl31 = irb.CreateShl(and29, ConstantInt::get(and29->getType(), 29));
storeRegister(PPC_REG_CARRY, shl31, irb);
```

`and29` is 0 or 1, so `shl31` is 0 or `0x20000000` — the carry placed at XER's
bit position 29, which is what the source this was transcribed from was doing.
But RetDec does not model XER as a word here: `PPC_REG_CARRY` is a separate
**i1** global, and `storeRegister` truncates. Bit 0 of `0x20000000` is zero, so
CA was unconditionally false.

Every other carry store in the file passes an i1 directly. This was the only
one that shifted first. Both of its tests are commented out, which is why
nothing caught it.

### A mutation that came back green, and the test that was missing

The first version of these tests covered "negative, bits lost" (CA set) and
"negative, nothing lost" (CA clear). A mutation that dropped the **sign**
condition entirely — making CA mean only "bits were lost" — passed both.

The missing case is positive-with-bits-lost: `srawi 0, 1, 4` with
`r1 = 0x0000000f` shifts four 1-bits out of a positive value, and CA must
still be zero. Added, and the mutation now fails.

### Falsification

```
AH1_slw_no_zero_select      SLW_count_past_the_width_is_zero
AH2_srw_no_zero_select      SRW_count_past_the_width_is_zero
AH3_subfe_carry_from_ra     SUBFE_carry_is_out_of_the_complemented_sum
AH4_sraw_carry_shifted      SRAWI_sets_the_carry
AH5_sraw_ignores_the_sign   SRAWI_positive_never_sets_the_carry
```

C2L-01 floor: PowerPC 926 → 938. 5,691 tests.

### Still open on PowerPC

`cntlzw`, `mulhw`, `mullw`, `divw`/`divwu` and `sraw` are computed at register
width, so they are wrong on PPC64 — which `createPpc64()` enables and the
gtest suite instantiates. `cntlzw r0, r1` with `r1 = 1` answers 63 where the
architecture says 31. MIPS solves this centrally with `isWordOperation()` and
`narrowToWord()`; PowerPC has no equivalent and the narrowing was applied by
hand to the rotate and shift family only.

Also open: `div`/`divw` emit `sdiv` with an unconstrained divisor, which is
undefined behaviour in LLVM rather than poison — strictly worse, because it
licenses deleting the surrounding code. `mips.cpp` already has
`storeRegisterUnpredictable()` for exactly this and does not use it on the
division path.

## Batch AI — ARM's register-controlled shifts are not modulo

### What the architecture says

`shift_n = UInt(R[s]<7:0>)` — the low **eight** bits of the register, 0 to
255 — and every one of those values is defined:

```
n == 0        the value is unchanged AND the carry is unchanged
1 <= n < 32   the ordinary shift; the carry is the last bit shifted out
n == 32       LSL gives 0 with carry = bit 0
              LSR gives 0 with carry = bit 31
              ASR gives the sign broadcast with carry = bit 31
n > 32        LSL and LSR give 0 with carry 0
              ASR gives the sign broadcast with carry = bit 31
```

LLVM's shifts are poison at or past the width, and RetDec's emulator reduces
them modulo it. Both of those disagree with ARM, so shifting by a raw count
was a wrong **value** and not only bad IR:

```
lsl r0, r1, r2   r1 = 1, r2 = 32     hardware r0 = 0        translator r0 = 1
lsr r0, r1, r2   r1 = 0x80000000, r2 = 32
                                     hardware r0 = 0        translator unchanged
asr r0, r1, r2   r1 = 0x80000000, r2 = 32
                                     hardware r0 = 0xffffffff  translator unchanged
```

The carry was worse. It was computed from `n - 1`, so the commonest runtime
count of all — zero — gave `shl i32 %val, 0xffffffff`.

### The immediate forms keep their old shape

Most shifts carry an immediate count, and there the whole rule is known at
translation time. That case is written out separately: it emits one shift and
one carry with no selects, and — for a non-zero count — never reads CPSR_C.

That matters beyond tidiness. The general path **must** read CPSR_C, because a
count of zero has to leave the carry exactly as it was and a flag cannot be
preserved without being read. Seven existing tests assert the set of registers
a shift loads, and the register forms now load one more. Those expectations are
updated rather than worked around; the immediate forms are unchanged.

### A select that could never fire

The first version also selected the VALUE on a zero count. A mutation removing
it changed nothing, and the reason is that shifting by zero is the identity for
all three kinds — `res` already equals `val` there. Removed. The carry select
is not redundant and stays.

### Two test values that agreed by accident

`ARM_INS_LSL_reg_by_zero_changes_nothing` used `r1 = 0x0000abcd` with the
carry preset to 1. Bit 0 of `0xabcd` is 1, so the carry that the buggy path
computes and the carry that the correct path preserves are both 1, and the
mutation passed. Changed to `0x0000abcc`, whose bit 0 is clear.

### Falsification

```
AI1_no_eight_bit_mask          LSL_reg_reads_only_eight_bits_of_the_count
AI2_no_out_of_range_value      LSL_reg_by_the_width_is_zero, ..._past_the_width
AI4_zero_count_clobbers_carry  LSL_reg_by_zero_changes_nothing, ..._eight_bits
AI5_no_carry_clear_past_width  LSL_reg_past_the_width_clears_the_carry
AI6_asr_no_sign_broadcast      ASR_reg_past_the_width_broadcasts_the_sign, ...
```

C2L-01 floor: ARM 650 → 664. 5,705 tests.

### Noted while here

`ARM_INS_ADD_ror_reg` assembles `add r0, r1, r2, ASR r3`. The test is
misnamed, and the consequence is that ROR with a register count has no test at
all. Left as it is rather than renamed, because the fix is a new test and not a
new name.

Also unchanged: these helpers write CPSR_C whether or not the instruction sets
flags, so a non-S `lsl` clobbers the carry. `ARM_INS_LSL` (`arm_tests.cpp`)
currently pins that as expected. It is a separate question from the count, and
this batch does not answer it.

## Batch AJ — LDRD put the two loaded words in the wrong registers

`LDRD <Rt>, <Rt2>, [<Rn>]` is `Rt <- MemA[Rn,4]; Rt2 <- MemA[Rn+4,4]`. The
translator loads a little-endian i64 from the address, so the **low** half is
the word at `[Rn]` and belongs in `operands[0]`. It stored the high half
there:

```c
storeOp(ai->operands[0], hi, irb);
storeOp(ai->operands[1], lo, irb);
```

```
ldrd r0, r1, [r2]   r2 = 0x1000, [0x1000] = 0x90abcdef, [0x1004] = 0x12345678
  hardware    r0 = 0x90abcdef   r1 = 0x12345678
  translator  r0 = 0x12345678   r1 = 0x90abcdef
```

Three places in the same file use the opposite convention, and they are the
ones that are right: `STRD` a hundred lines down stores `operands[0]` at the
base address and `operands[1]` at base+4, and `UMULL`, `SMULL`, `UMLAL` and
`SMLAL` all put `lo` in `operands[0]`. `LDRD` contradicted all four.
`LDREXD` and `LDAEXD` share the translator and had it too.

### Three tests asserted the swapped answer

`ARM_INS_LDRD`, `ARM_INS_LDREXD` and `ARM_INS_LDAEXD` all set a quadword at
`0x1000` and expected `r0` to take its high half. They were written from the
same reading of the manual as the code, which is the Batch AA-1 shape and the
fourth time it has appeared on this branch:

```
AA-1  every IDIV test divided by a positive number
AG-4  the EXTR test chose a constant whose bits hid the bug
AH    both SRAW tests were commented out; the SUBFE ones used operands
      where the right and wrong readings agree
AJ    all three LDRD tests asserted the swap
```

All three are corrected here, and reverting the fix fails all three.

C2L-01 floor unchanged: the tests were corrected, not added.

## Batch AK — the ARM64 addressing mode dropped its extender

`ldr w0, [x1, w2, sxtw #2]` is how a compiler indexes an array with a signed
`int`. The offset is `SignExtend(Wm,64) << 2`. Two independent errors sat in
one path:

```c
auto* idxR = loadRegister(op.mem.index, irb);
if (idxR) { idxR = generateOperandShift(irb, op, idxR); }   // at 32 bits
...
idxR = irb.CreateZExtOrTrunc(idxR, addr->getType());        // always ZERO
```

**The extender was never read.** `op.ext` does not appear on this path at all,
so a signed index was zero-extended:

```
ldr w0, [x1, w2, sxtw #2]   x1 = 0x10000, w2 = 0xffffffff (-1)
  hardware    0x10000 + (SignExtend(-1) << 2) = 0xfffc
  translator  0x10000 + 0x00000000fffffffc  = 0x1_0000_fffc   (4 GiB away)
```

`generateOperandExtension()` already existed and was correct — Batch AG had
just fixed its UXTX and SXTX cases. Its only caller was the register-operand
branch.

**And the scale was applied before the widening**, so it wrapped inside 32
bits. This one bites the unsigned form too:

```
ldr x0, [x1, w2, uxtw #3]   x1 = 0, w2 = 0x20000000
  hardware    ZeroExtend(w2,64) << 3 = 0x1_0000_0000
  translator  (0x20000000 << 3) at 32 bits = 0, so addr = 0
```

Extend first, to the address's width; shift after.

There was no test for any of it: the only two `ldr` tests with a register
index use `[x1, x2]` with no extender at all.

C2L-01 floor: ARM64 560 → 562. 5,707 tests.

## Batch AL — the PowerPC `w` instructions were computed at register width

PowerPC's 64-bit mode is live: `capstone2llvmir.cpp` dispatches `CS_MODE_64`
to `createPpc64()`, `powerpc_init.cpp` makes the GPRs `i64`, and the gtest
suite instantiates both modes. The instructions whose names end in `w` work on
the low **word** of their operands whatever the register width, and five of
them were not narrowing:

```
cntlzw r0, r1      r1 = 1                   31 on the hardware, 63 here
mullw  r0, r1, r2  r1 = 0x1_00000002, r2=3   6 on the hardware, 0x3_00000006
mulhw  r0, r1, r2  r1 = 0x1_00000002, r2=2   0 on the hardware, 2 here
divw   r0, r1, r2  r1 = 0x1_00000000, r2=1   0 on the hardware, 0x1_00000000
srawi  r0, r1, 4   r1 = 0xffffffff          -1 on the hardware, 0x0fffffff…
```

Two of them are worth naming individually.

`cntlzw` instantiated `llvm.ctlz` on **the operand's** type, so on PPC64 it
counted the thirty-two zero bits above the word as well.

`mulhw` did `CreateSExtOrTrunc(op, i64)` — which is a **no-op** when the
operand is already i64. It reads like a widening and is one in 32-bit mode; in
64-bit mode it left a 64×64 multiply with bits 63:32 taken.

`sraw`'s whole body is a hand-unrolled 32-bit rotate — its constants are 31
and 32 — applied to whatever width arrived.

All five are inert in 32-bit mode, which is where every test for them that is
not `ALL_MODES` lives. `PPC_INS_CNTLZW_non_zero_32` is explicitly
`ONLY_MODE_32`; the two `ALL_MODES` cntlzw tests use `r1 = 0xffffffffffffffff`,
which answers 0 either way.

MIPS solves this centrally: `isWordOperation()` names the twenty-three
instructions that need it and `narrowToWord()` applies it. PowerPC had no
equivalent — the narrowing was written by hand into the rotate and shift
family (`translateRotlw`, `translateSlwi`, `translateShiftLeft` and four more)
and into nothing else. This batch adds a `narrowToWord()` and uses it, which
is the same shape MIPS already had.

C2L-01 floor: PowerPC 938 → 948. 5,717 tests.

## Batch AM -- x86: eleven defects, every one measured on the host

The container is x86-64, so none of this batch rests on a reading of the
manual. Each fix below names the instruction sequence that was run on the
hardware and the answer it gave.

Six of the eleven came from a systematic sweep of the x86 translator for five
defect shapes (sub-register write width; shift count not provably below the
operand width; one dispatch key covering more than one operation; sign versus
zero extension; float-to-integer without a range guard). The other five came
from an audit of the emulator, which is the instrument the rest of the suite
depends on.

### AM-1  MOVSX sign-extended past its destination into the parent register

`storeRegister` converted the value to the PARENT register's width before
deciding how to write it, so `SEXT_TRUNC_OR_BITCAST` ran the sign to bit 63.
Two failures, both measured:

  movsx ecx, ax    AX = 0xff00           RCX = 0x00000000_ffffff00
                                         was  0xffffffff_ffffff00
  movsx ax, bl     RAX = 0x1122334455667788, BL = 0xff
                                         RAX = 0x11223344_5566_ffff
                                         was  0xffffffff_ffffffff

The second is the worse one: the 8- and 16-bit path is a read-modify-write
whose `OR` had no mask on the converted value, so an all-ones sign extension
set every bit of the parent.

Why it survived: the two MOVSX tests read ECX, and the harness's
`getRegisterValueUnsigned` truncates a sub-register read to that register's own
width. No MOVSX test could observe the parent. The fix masks the converted
value to the destination's width, which is a no-op for every zero-extending
caller -- that is, for every other caller in the file.

### AM-2  `rep movs` stored EDI's final value into ESI

One `add` was computed and stored to both pointers. Measured: after `rep movsb`
with ECX = 16, each pointer advances 16 from its OWN base. `rep movsb` is the
inlined memcpy in current glibc. The non-REP path in the same function always
computed the two separately. The existing test checked only metadata.

### AM-3  VMOVAPS/VMOVAPD were dispatched to the legacy-SSE move

A VEX write zeroes everything above what it writes; a legacy SSE write does
not. Measured: `vmovaps xmm1, xmm3` leaves ymm1[255:128] zero. These two ids
pointed at `translateSseMovWhole` while their unaligned twins VMOVUPS/VMOVUPD
pointed at `translateAvxMov` -- whose own doc comment already listed VMOVAPS
and VMOVAPD among the instructions it handles. Only the table disagreed.

### AM-4  A YMM write did not zero bits 511:256

`storeWideVectorRegister` wrote the XMM and YMM_HI globals and left ZMM_HI
alone. Measured: `vmovaps ymm0, ymm1` zeroes zmm0[511:256]. Nothing but a VEX
or EVEX instruction can write a YMM register in this model, so every value
arriving there carries the rule. `storeVectorOp`'s YMM branch had it; this
path, which is where `storeRegister` sends a YMM destination, did not.

### AM-5  VMOVMSKPS/VMOVMSKPD on a YMM source read only 128 bits

The lane count was hardwired to the XMM width and the i256 register read was
then passed through a helper that TRUNCATES to 128 bits. Measured:
`vmovmskps eax, ymm0` with all eight sign bits set answers 0xff, not 0x0f;
`vmovmskpd` answers 0x0f, not 0x03. The neighbouring PMOVMSKB translator
already derived its width from the operand.

### AM-6  Shift and rotate counts at 8 and 16 bits -- three different rules

All seven of these masked the count to five bits and stopped. Five bits reach
31, which is past the end of an i8 or an i16, and what the architecture does
next is NOT the same instruction to instruction. All three measured:

  SHL/SHR/SAR  do not reduce. `shl al, cl` with cl = 20 gives 0x00, NOT the
               modulo-8 0x20; `sar al, cl` with cl >= 8 gives 0xff.
  ROL/ROR      rotate by the count MOD the width -- and still write CF when
               the MASKED count is non-zero. `rol al, cl` with cl = 8 and
               AL = 0x5a leaves AL alone and takes CF from 1 to 0.
  RCL/RCR      reduce MOD the width PLUS ONE, because the carry is a real
               extra bit of the rotated value. `rcl al, cl` with cl = 9 is the
               identity, CF included; cl = 8 is not.

In LLVM a shift by at least the operand's width is poison whichever of the
three is meant, so each had to be said explicitly. The emulator reduces shift
amounts modulo the width, which made the two rotate cases agree with the
hardware by accident and the linear case answer plausibly -- right for the
wrong reason in one place and wrong in the other.

The first attempt at this fix reduced the ROL/ROR count itself. That made the
whole body conditional on the reduced value, so a count of 8 on a byte skipped
the CF write. The oracle caught it: 88 mismatches across rol8, ror8, rcl8,
rcr8, rol16, ror16, rcl16, rcr16. The SDM says it in two separate lines -- the
rotate loop runs `(count & mask) MOD size` times, and the CF update is guarded
by `(count & mask) != 0` -- and the hardware says it too.

### AM-7  FICOM/FICOMP widened a signed memory integer with UIToFP

Every other x87 integer form uses SITOFP, FILD included, and FICOM reads the
same encoding FILD does. Measured: ST(0) = 0.0 compared against a word holding
0xffff sets C0 = 0, because 0.0 is the greater. This compared against 65535.0
and set C0 = 1, so the `fnstsw`/`sahf`/`jb` that follows branched the wrong way.

### AM-8  FIST/FISTP truncated where the architecture rounds

`CreateFPToSI` truncates, and this one function is dispatched for FIST, FISTP
and FISTTP alike -- so all three got FISTTP's operation. Measured with the
default rounding control: `fistp` on 2.7 stores 3, on 2.5 stores 2 and on 3.5
stores 4. That is ties-to-EVEN, so `roundeven` and not `round`. `fisttp` on
2.7 stores 2, confirming the two are different instructions.

The same site also had the bare-conversion range gap: measured, `fistpl`
answers 0x80000000 for +inf, -inf, NaN, 1e24 and 2147483647.5 alike, and
`fistps` answers 0x8000. `fptosi` calls every one of those poison. Both are
now handled by the same helper the SSE side uses.

RetDec models no FPCW rounding-control field, so the reset value is the only
one that can be modelled; code that sets RC to truncate first is translated as
if it had not. That is now a stated approximation rather than a silent one.

### AM-9  FRNDINT used ties-away-from-zero

`Intrinsic::round` rounds halves away from zero; FRNDINT follows the FPCW
rounding control, whose reset value is nearest-EVEN. Measured: 0.5 -> 0,
1.5 -> 2, 2.5 -> 2, 3.5 -> 4. `Intrinsic::round` answers 1 and 3 for the two
that distinguish them.

### AM-10  FSCALE rounded ST(1) where it should truncate

Measured: `fscale(8.0, 1.9)` = 16, so 2^1 and not 2^2; `fscale(8.0, -0.9)` = 8,
so 2^0 and not 2^-1. The variable holding the result was already named
`roundDown`.

### AM-11  PUSHFD tested for POPFD

`translatePushEflags` gated the AC and ID bits on `id == POPFD || id == POPFQ`.
The only ids dispatched to it are PUSHF, PUSHFD and PUSHFQ, so the branch was
dead and those two flags could be written by POPFD and never read back.
Measured: setting both and pushing returns both set. This is the canonical
CPUID probe, which therefore concluded "CPUID unsupported" on every binary.

### The instrument was wrong too

`scripts/ci/x86_wide_oracle.c` declined to draw a count at or above the operand
width for SHL, SHR and SAR at 8 and 16 bits, on a comment claiming those
instructions "leave the destination AND CF undefined" there. CF, yes. The
destination, no. So the oracle was refusing to draw exactly the inputs that
would have shown AM-6, and the reason it gave was the same misreading of the
manual that produced the bug.

That is the sixth time this session the test that should have caught a defect
was written from the same reading as the defect (after IDIV, EXTR, SRAW, LDRD
and cntlzw). It is now the most reliable single predictor of where a bug is.

The restriction is gone, and with SAR8, SHR16, ROR16 and the four RCL/RCR byte
and word forms added the wide comparison went from 4,320 rows to 17,200.

### Still not value-testable here

FICOM, FIST, FRNDINT and FSCALE are fixed by measurement against the hardware
but cannot be checked by the emulator in this tree: it excludes `x86_fp80` from
its floating-point intrinsic block, so every x87 intrinsic evaluates to 0.0,
and it implements `fptoui` and `fptosi` through one path so a signed/unsigned
distinction is invisible to it. The first of those is addressed in the next
batch; the second remains open.

### Falsification: six of eight caught, and why the other two could not be

Each of the eight decisions in AM-6 was reverted alone, rebuilt, and the wide
comparison re-run against 17,200 measured rows (md5 guard on the source before
and after every mutation):

    A_shl_saturate          222 mismatches   shl8=137  shl16=85
    B_shr_saturate          184 mismatches   shr8=110  shr16=74
    C_linear_clamp          705 mismatches   shl8=203 shr8=147 sar8=74
                                             shl16=131 shr16=104 sar16=46
    E_rcl_rcr_mod           753 mismatches   rcl8=223 rcr8=219 rcl16=156 rcr16=155
    F_rcl_wide_shift         25 mismatches   rcl8=20  rcl16=5
    G_rcr_wide_shift         21 mismatches   rcr8=16  rcr16=5
    D_rotate_reduce           0 mismatches   NOT CAUGHT
    H_rol_complement_mask     0 mismatches   NOT CAUGHT

D and H are the two decisions that exist only to keep poison out of the
emitted IR. Reverting either leaves `shl i8 %x, %n` with n reaching 31, which
LLVM calls poison -- and the interpreter these oracles run on reduces every
shift amount modulo the operand width (`llvmir_emul.cpp`, `getShiftAmount`),
which for a rotate is the architecture's own rule. So the measured answers do
not move. The code is not dead, and this is not the AC-6 / AI-3 situation
where a green mutation meant the mutated line never ran: it means the
instrument cannot see the failure mode.

That is a gap in the instrument, not a reason to drop the fix. SHIFT-01 below
is the instrument that can see it.

## Batch AM, continued -- what fixing the emulator exposed

### AM-12  The emulator could not evaluate any x87 intrinsic

`llvmir_emul.cpp` restricted its floating-point intrinsic block to `float` and
`double`, excluding `x86_fp80`. That was not a representation limit: this
interpreter holds an fp80 value in `GenericValue::DoubleVal` everywhere else
-- `executeFAddInst` and its siblings fall the `X86_FP80TyID` case straight
through to the `Double` one, and `executeFPTruncInst` reads `Src.DoubleVal`
for an fp80 source.

The cost was that every x87 intrinsic fell through to `LowerIntrinsicCall`,
which rewrites `llvm.round.f80` into a call to `roundl` that the interpreter
cannot execute. So the x87 tests could assert only that the CALL APPEARED,
with the register value left as `ANY`. A translator that rounded the wrong way
was indistinguishable from one that did not.

Four tests were sitting on that: FSQRT, F2XM1, FRNDINT and FSCALE. All four
now assert numbers. `Intrinsic::exp2` was added to the same block because
FSCALE needs it and would otherwise still be untestable.

### AM-13  F2XM1 computed 2^(x-1) instead of 2^x - 1

The subtraction was applied to the exponent:

    op0 = op0 - 1;  res = exp2(op0);      // 2^(x-1)

Measured: `f2xm1` on 0.5 gives 0.41421356, which is 2^0.5 - 1. This answered
2^-0.5 = 0.70710678. On 0.25 the hardware gives 0.18920712 and this gave
0.59460356. The two readings agree at exactly one point, x = 1.

This was found by fixing AM-12, not by looking for it. The one test of the
instruction asserted the register as `ANY` and asserted a call to `exp2l` with
16.0 -- which is the argument the WRONG formula passes. The test recorded the
bug in both halves and could not fail.

### AM-14  ARM and ARM64 CLZ declared their defined zero case poison

Both passed `is_zero_poison = true` to `llvm.ctlz`. ARM64 defines `clz x1, x2`
with x2 = 0 as 64 and the W form as 32; A32 defines `clz Rd, Rm` with Rm = 0
as 32. The flag says the result is poison there instead, and -- worse for a
decompiler -- licenses the optimiser to infer the operand is non-zero and
delete the zero test around it.

The x86 side of this tree had already made the opposite call for LZCNT and
TZCNT, with a comment saying the defined zero case is exactly why.

The two ARM64 tests cover this: `ARM64_INS_CLZ_r_r` and `ARM64_INS_CLZ32_r_r`
both pass a zero operand and assert 0x40 and 0x20, the right answers. They
passed either way, because the emulator's `ctlz` ignores its second argument.
A test that asserts the correct value and cannot fail -- a different shape
from the five instances above, where the test asserted the wrong value. Both
are worth looking for. The ARM 32-bit tests use no zero operand at all.

### AM-15  MIPS LUI did not sign-extend on MIPS64

    lui $1, 0xabcd     MIPS64     $1 = 0xffffffffabcd0000
                       translator $1 = 0x00000000abcd0000

MIPS64 defines LUI as `GPR[rt] <- sign_extend(immediate || 0^16)`. The code
carried a `CreateZExt` to the default type that was not a widening at all:
`loadOp` materialises a MIPS immediate at the default type already, so the
cast was a no-op on equal types, and `storeOp`'s sign-extending default could
not help because the value was already the parent's width.

This breaks the standard n64 constant idiom: `lui $2,0xffff; ori $2,$2,0x1234`
is 0xffffffffffff1234 on the machine and was 0x00000000ffff1234 here.

The single LUI test ran in MIPS64 (it was `ALL_MODES`) and asserted
0xabcd0000 there -- the 32-bit answer. It pinned the defect. Seventh instance.

### AM-16  PowerPC MULLI was translated as a word multiply

`PPC_INS_MULLI` and `PPC_INS_MULLW` were dispatched to one function whose body
named neither, so `mullw`'s narrowing was applied to both:

    mullw RT,RA,RB   RT <- (RA)[32:63] x (RB)[32:63]      -- the low words
    mulli RT,RA,SI   prod[0:127] <- (RA) x EXTS(SI)       -- the whole register
                     RT <- prod[64:127]

There is no "mulliw".

    mulli r0, r1, 3    r1 = 0x0000000100000002
      PPC64            r0 = 0x0000000300000006
      translator       r0 = 6

The MULLI test used r1 = 0x2222, which fits in a word and is positive, so the
narrowed and un-narrowed readings agree on it exactly.

AM-15 and AM-16 are CONFIRMED BY READING against the ISA and against
capstone's own operand tables, not measured: this container is x86-64 and has
no MIPS or PowerPC emulator. That is a weaker standard than the rest of this
batch and is stated rather than glossed.

## SHIFT-01 -- a gate for the class the oracles cannot see

Two of the eight AM-6 mutations came back green. Not because the mutated code
was dead -- that was the AC-6 and AI-3 story -- but because the failure mode is
POISON, and nothing in this tree could observe poison.

`shl i8 %x, 20` is poison in LLVM: not "some number", but a value the optimiser
may assume never arises and may propagate through everything downstream. Every
architecture here defines an answer instead. But the hardware oracles compare
against RetDec's own interpreter, and that interpreter reduces every shift
amount modulo the operand width (`llvmir_emul.cpp`, `getShiftAmount`, whose own
comment says "according to the llvm documentation ... the result is undefined.
but we do shift by this rule"). For a rotate, that rule IS the architecture's,
so poison-producing IR gives exactly the right answer and passes.

So the class was invisible from both ends: the gtests and the oracles both run
the IR, and running it is the one thing that cannot show this.

`scripts/ci/shift_poison_check.cpp` does not run it. For a corpus of every
shift and rotate form whose amount is a register rather than an immediate --
70 instructions across x86-64, ARM, ARM64, MIPS and PowerPC, including the ARM
shifted-operand forms where the shift hides inside another instruction's second
operand -- it translates the instruction and asks LLVM's own value tracking
whether each emitted shift amount can reach the operand's width. It runs inside
C2L-01, reusing the objects already built there.

### Constant ranges, not known bits

The first version used `computeKnownBits` and fired on 64 shifts, including the
clamp-and-select idiom the AM-6 fix itself uses. Known bits carry a bitmask, so
the tightest they can say about a value bounded by 8 is "at most 15", and for
`select(n >= 8, 7, n)` they report only the bits the two arms agree on -- which
is "might be 31". The check was firing on the fix rather than on the bug, which
is the one thing a gate must not do.

`computeConstantRange` is exact for the operations that appear here: `urem` by
a constant gives [0, k), `sub` is exact, `umin` gives [0, k+1).

### And where the bound was not local, the translator now says it

Some amounts are bounded only by a branch -- `n - 1` cannot wrap because the
block runs only for a non-zero n. That is true and a local analysis cannot use
it. Rather than teach the checker about dominating branches, those sites now
carry a `umin`, which is the identity for every value the block actually runs
on and states the bound in the value itself. The clamp in
`clampLinearShiftCount` became a `umin` for the same reason: it computes the
same number as the select and, unlike the select, says so locally.

That is a real improvement to the code and not merely an accommodation of the
tool. "This is in range because of a branch thirty lines up" is exactly the
kind of reasoning that was wrong in six of the eight places this batch touched.

### What it found immediately

  * ARM64 LSLV/LSRV/ASRV/RORV -- the unmasked amounts, recorded as open since
    the ARM64 audit and left because they were "poison, not value-observable".
    They are masked now: every one of the four is defined as
    `shift_amount = UInt(operand2) MOD datasize`.
  * ARM64 `generateShiftRor` -- the same, and reached from the shifted-operand
    path as well, where the amount is whatever the program put in the register.
  * ARM `generateShiftRor` -- the Batch AI rewrite covered LSL, LSR and ASR and
    left ROR with all three of the defects it removed from the others: no
    eight-bit mask on the count, a complement of the full width at a count of
    zero, and an unconditional carry write where the architecture preserves it.
    The last of those is a WRONG VALUE, not just poison: `rors r0, r1, r2` with
    r2 = 0 must leave C alone and was writing bit 31 of the result. The only
    register-form test used a count of 5 -- in range and non-zero -- and the
    one test named `ARM_INS_ADD_ror_reg` assembles an ASR.

The self-test builds an unmasked i8 amount and requires it to be flagged, then
masks it to three bits and requires it not to be. A check that cannot report a
problem proves nothing.

### AM-17  The emulator aborted the whole binary on llvm.umin

Stating a shift amount's bound with `llvm.umin` rather than a select made the
test binary die with

    LLVM ERROR: Code generator does not support intrinsic function 'llvm.umin.i8'

and no failing test named. The interpreter had no case for the integer
min/max intrinsics, so they fell to `IntrinsicLowering`, and what that does
with an intrinsic it does not know is `report_fatal_error` -- which takes the
process with it. The block above it already carried a comment saying exactly
this about `llvm.fma`; `umin` was simply the next one to arrive.

`umin`, `umax`, `smin` and `smax` are now evaluated directly. They are
ordinary LLVM intrinsics that any current optimiser emits, so this was a hole
waiting for the first translator to use one, and it fails in the worst
available way: the abort names no test, so the failure reads as "the suite
crashed" rather than "this instruction is wrong".

Diagnosing it cost two rounds, because the first abort happened while source
files were being edited mid-build, and a mismatched object set is a perfectly
good explanation for a crash. It was the wrong one. The lesson is the smaller
one: do not edit while a build is running, because it makes a real failure
indistinguishable from an artefact.

## Batch AN -- three more, and two subagent claims that were wrong in the details

### AN-1  `ldr r0, [r1, -r2]` loaded from r1 PLUS r2

The ARM memory-operand path negated the index when `mem.scale == -1`. That
branch is dead: disassembling `ldr r0, [r1, -r2]` with the bundled capstone
gives `scale = 1` and `subtracted = 1`, and so does every other negated-index
form. So the minus sign was dropped and the address was wrong -- not the
writeback, the ADDRESS, on every `[Rn, -Rm]` load and store.

The sweep that raised this reported it as a writeback-only defect and said the
address "is computed correctly ... and negates via mem.scale == -1". Measuring
capstone directly says otherwise. Worth recording as a reminder that a report
which reads plausibly can still have its central fact backwards.

`subtracted` is NOT applied to the displacement: for `ldr r0, [r1, #-4]`
capstone reports `disp = -4` and `subtracted = 0`, so the sign is already there
and negating again would undo it. Both measured.

### AN-2  MIPS CVT.W.fmt truncated where the ISA rounds

CVT.W.fmt rounds with the current rounding mode, whose default is
nearest-even; a bare `fptosi` truncates, which is TRUNC.W.fmt -- a different
instruction that exists alongside it. `translateFpToInt` eighty lines up
already case-splits TRUNC/ROUND/CEIL/FLOOR onto fptosi/roundeven/ceil/floor
and says in its own comment that the rounding mode is the whole difference
between them. This one was missed.

The destination is also a WORD whatever the source format is -- that is what
the W means, and CVT.L.fmt is the 64-bit one. The width was taken from the
SOURCE, so `cvt.w.d` converted to 64 bits.

### AN-3  PowerPC record forms compared 32 bits where 64-bit mode compares 64

For Rc=1 the Power ISA compares the whole register in 64-bit mode, and the
zero-extending word family -- rlwinm, rlwnm, rlwimi, rotlw, clrlwi, slw, srw,
slwi, srwi -- all leave RA[0:31] zero, so their register value can never be
negative while the 32-bit result can:

    rlwinm. r0, r1, 0, 0, 15   r1 = 0x80000000
      RA = 0x0000000080000000 -> LT = 0, GT = 1
      comparing the word      -> LT = 1, GT = 0

Seven sites now widen to the register's own type first. The SIGN-extending
members of the same family -- cntlzw., divw., mulhw., sraw. -- must not, since
zero-extending would invert their sign, and a test asserts that they still do
not.

There was no record-form test for any of the seven.

### And a whole class of MIPS tests that could not fail

`CVT.W.fmt` leaves an INTEGER in a floating-point register, and the fixture
compares a float register with `EXPECT_NEAR(..., 0.001)`. The bit pattern of a
small integer is a denormal -- 3 is 4.2e-45 -- so every such expectation
compares equal to zero, to itself, and to every other small integer. Both
pre-existing CVT.W tests asserted a value that way, and so did the first draft
of the three added here.

They now compare the raw bits. While fixing that: `MIPS_INS_CVT_W_d` set its
input with an `_f32` literal into a DOUBLE register, so the value it converted
was the 32-bit pattern of 3.1415 read back as 5.3e-315 -- and the vacuous
expectation could not tell.

Falsification: each of the three fixes reverted alone fails 4, 4 and 5 tests
respectively.

### AN-4  The pre-indexed writeback disagreed with its own address

Having fixed the sign in the address (AN-1), the writeback still reloaded the
bare index register, so it dropped both the shift and the sign:

    ldr r0, [r1, r2, lsl #2]!   r1 = 0x1000, r2 = 4
      loads [0x1010]  and leaves r1 = 0x1004
    ldr r0, [r1, -r2]!          r1 = 0x1010, r2 = 8
      loads [0x1008]  and leaves r1 = 0x1018

The address and the writeback were computing different addressing modes from
the same operand. Four sites now share one helper with the address path's
rules.

The helper deliberately does NOT call generateOperandShift, which writes
CPSR_C: the address path has already called it, and a second identical write is
noise. A memory operand's shift is always an immediate in A32 -- `[Rn, Rm, LSL
Rs]` is not encodable -- so every case can be built directly.

There was no test anywhere for `[Rn, Rm, lsl #N]!`, and the one named
`ARM_INS_LDR_minus_reg_preindexed_writeback` assembles `[r1, r2]!` with a
negative value in r2, which exercises none of this.

### Noted, not fixed: a shifted operand writes CPSR_C for non-S instructions

`generateOperandShift` writes CPSR_C unconditionally, so `ldr r0, [r1, r2, lsl
#2]` and `add r0, r1, r2, ror #5` both update the carry. ARM updates the
shifter carry-out only for the flag-setting forms. Several existing tests
assert the current behaviour (`ARM_INS_ADD_ror` expects CPSR_C stored), so
fixing it means revisiting them; it is a real defect and is recorded rather
than guessed at here.
