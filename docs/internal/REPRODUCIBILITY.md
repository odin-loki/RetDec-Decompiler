# Reproducibility of the decompiler's output

Two runs of `retdec-decompiler` on the same binary, with the same options,
have to produce the same `.c`. People diff decompiler output, and the
function-analysis cache is only meaningful if the thing it caches is a
function of its input.

Two CI checks cover it:

* `scripts/ci/check_cache_differential.sh` (CACHE-05) — the first two of its
  four runs per binary are both cache-off, and if they disagree the check
  says so in those words and stops, because that is a different bug report
  from a cache defect. It runs the nine `ci-core` names, all gcc `-O0`.
* `scripts/ci/check_output_determinism.sh` (DET-01) — two cache-off runs of
  a third of the built corpus, sampled by a hash of the name so every
  compiler and optimisation level is represented. `--self-test` checks that
  the gate can actually fail, and runs in `check_push_gates.sh`. CACHE-05's nine binaries are all small
  and all `-O0`; the HLL copy-propagation passes do not even take their
  parallel path below twenty-four functions, which is where the first of
  the defects below lived.

## The invariant

Nothing that reaches the emitted code may depend on:

* **a pointer value.** `VarSet`, `StmtSet`, `FuncSet`, `TypeSet` and the rest
  of `support/types.h` are `std::set<ShPtr<T>>`, ordered by the address the
  object happens to have. Within one thread that order follows allocation
  order and is stable; across threads it is not, because glibc gives each
  thread its own mmap'd arena, placed by ASLR. The same goes for
  `std::unordered_*` keyed by a pointer: the bucket a pointer lands in moves
  with the heap base.
* **the wall clock or the thread schedule.** Which optimizer instance picks
  up a function, how long a CFG took to build, how much of a time budget is
  left.

A `std::sort` over such a container is only as deterministic as its
comparator is total. Sorting on a key that leaves two distinct elements
tied hands the tie back to the input order, which is the pointer order
again. `std::sort` is not stable, and a comparator that is not a strict
weak ordering makes it undefined behaviour outright.

## Where this was violated

| Where | What it was |
|-------|-------------|
| `CopyPropagationOptimizer`, `SimpleCopyPropagationOptimizer` | Workers were built with a different `ValueAnalysis` and `VarUsesVisitor` configuration than the main thread, which shares their work, and functions were handed out by an atomic counter — so the analysis a function got was decided by thread scheduling. Now a fixed stride, and workers configured like the main thread. |
| `copy_propagation_optimizer.cpp`'s `ordered(StmtSet)` | Sorted on the statement's text, which two distinct statements can share. Ties now break on the statement's position in the function's CFG. Removal order is observable: removing a statement moves its address comment onto the one that follows it. |
| `GlobalVarsSorter` | `std::sort` over a comparator that was a strict weak ordering in 487 of the 65536 dependency graphs over four variables. Replaced by Kahn's algorithm over the base order. |
| `StructTypesSorter` | Sorted on the structure's name; every unnamed structure has the empty name. Ties now break on the type's text. |
| `sortByName()` in `utils/ir.cpp` | Compared names case-insensitively, so `Foo` and `foo` tied. Falls through to an exact comparison. |
| `StructureConverter::tryControlledNodeSplitting` | Built the edge list from `CFGNode::getPredecessors()`, an `unordered_set<ShPtr<CFGNode>>`, and `edgeRefs[0]` is the edge that keeps the original node while the rest are redirected to a clone. Which predecessor kept it followed pointer hashes. The edges are now sorted on the node's position in the deterministic breadth-first `order` walk, then on the address the block came from. `redirectByPred` was a second `unordered_map` iterated to apply the redirects, and is a vector now. |

## What it takes to catch one

DET-01 runs each corpus binary twice and compares. Two runs of the same
binary in the same job get similar address-space layouts, so a defect of
this shape can agree for a long time and then disagree once:
`mergesort-gcc-O3` passed two DET-01 runs before the node-splitting entry
above showed up on the third.

The second run therefore gets four kilobytes of padding in its environment
block, which moves the initial stack and with it the addresses the allocator
hands out. Turning a coincidence into a difference on purpose is the only
thing that makes a two-run check worth running.

## Time budgets

`HLL_OPT_BUDGET_SECONDS` (60s, `optimizer_manager.cpp`), the per-function
20s budget in `CopyPropagationOptimizer`, and the 200ms CFG-build guard in
`SimpleCopyPropagationOptimizer` all make the output a function of how
loaded the machine is. That is the point of a budget — it bounds the run —
but it means a binary large enough to hit one is not reproducible by
construction. The corpus binaries CACHE-05 uses decompile in about a
second, nowhere near any of them.
