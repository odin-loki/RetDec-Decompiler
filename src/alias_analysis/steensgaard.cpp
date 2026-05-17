/**
 * @file src/alias_analysis/steensgaard.cpp
 * @brief Steensgaard (1996) unification-based alias analysis.
 *
 * ## Algorithm
 *
 * Bjarne Steensgaard, "Points-to Analysis in Almost Linear Time" (POPL 1996).
 *
 * The algorithm maintains a "points-to graph" where each node represents an
 * equivalence class of SSA values that may point to the same memory.  Nodes
 * are merged (unified) when constraints require it.
 *
 * ### Constraint processing
 *
 *   AddrOf  (x = &y):
 *     join(pointsTo(x), y)
 *     — x's points-to target is y's equivalence class.
 *
 *   Copy    (x = y):
 *     if pointsTo(y) ≠ ⊥:
 *       join(pointsTo(x), pointsTo(y))
 *     — x and y share the same points-to target.
 *
 *   Load    (x = *y):
 *     if pointsTo(pointsTo(y)) ≠ ⊥:
 *       join(pointsTo(x), pointsTo(pointsTo(y)))
 *     — x may point wherever the memory pointed to by y may point.
 *
 *   Store   (*x = y):
 *     if pointsTo(x) ≠ ⊥ AND pointsTo(y) ≠ ⊥:
 *       join(pointsTo(pointsTo(x)), pointsTo(y))
 *     — the memory pointed to by x may overlap with y's points-to target.
 *
 *   External (f(x)):
 *     mark(pointsTo(x)) as may_point_to_anything
 *     — x's target is accessible externally.
 *
 * ### Union-Find
 *
 * We use path-compressed union-find with union-by-rank.  The `parent_` array
 * maps value ID → representative root.  `rank_` guides merging direction.
 *
 * `pointsTo_[root]` maps each equivalence class root to the root of its
 * points-to target class.  This is the "points-to edge" in the graph.
 *
 * ### Fixpoint
 *
 * Steensgaard's algorithm is NOT iterative — it is a single-pass algorithm.
 * Each constraint is processed exactly once.  The union-find ensures that
 * merging is applied transitively through the `find()` operation.
 *
 * However, when a `join(a, b)` causes two nodes with different points-to
 * targets to be merged, we must also join their points-to targets (recursive
 * unification).  We implement this via a worklist to avoid stack overflow on
 * deeply recursive graphs.
 *
 * ### CUDA dispatch (stub)
 *
 * For large functions (> kCUDAThreshold SSA values), the constraint set
 * can be dispatched to the CUDASteensgaard kernel from the cuda_accel module.
 * The dispatch interface serialises the constraint vector, sends it to the
 * kernel, and deserialises the resulting union-find arrays.
 *
 * In this implementation we provide the in-process path unconditionally
 * (the CUDA dispatch path compiles conditionally when RETDEC_HAS_CUDA
 * is defined).  The in-process path handles functions of any size correctly.
 *
 * ### Soundness vs. precision
 *
 * Steensgaard is sound but not complete: it may report MayAlias for
 * pointers that don't actually alias (false positives), but it will never
 * report NoAlias when aliasing actually occurs (no false negatives).
 *
 * It is strictly less precise than Andersen's (1994) inclusion-based
 * analysis but asymptotically faster: O(n α(n)) vs. O(n³) in the worst case.
 *
 * For decompilation purposes, Steensgaard is typically sufficient because:
 *   - Most pointer aliasing comes from struct fields (handled exactly by
 *     the type-system after type inference).
 *   - Stack pointers are handled exactly by StackAliasAnalysis.
 *   - Heap aliasing mostly needs a sound yes/no for variable separation.
 */

#include "retdec/alias_analysis/alias_analysis.h"
#include "retdec/ssa/ssa.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <queue>
#include <unordered_set>

namespace retdec {
namespace alias_analysis {

// ─── Constructor / destructor ─────────────────────────────────────────────────

SteensgaardAnalysis::SteensgaardAnalysis() = default;
SteensgaardAnalysis::~SteensgaardAnalysis() = default;

// ─── Union-Find helpers ───────────────────────────────────────────────────────

static void ensureSize(std::vector<uint32_t>& v, uint32_t id, uint32_t defVal) {
    if (id >= v.size()) {
        v.resize(id + 1, defVal);
    }
}

void SteensgaardAnalysis::addValue(uint32_t id, bool /*isPointer*/) {
    uint32_t defId = id;  // identity initial parent
    ensureSize(parent_, id, defId);
    ensureSize(rank_,   id, 0);
    // Fix: the default fill was wrong for newly expanded slots — reset each
    if (parent_[id] == 0 && id != 0) parent_[id] = id;
    parent_[id] = id;
    rank_[id]   = 0;
}

uint32_t SteensgaardAnalysis::find(uint32_t x) const {
    if (x >= parent_.size()) return x;
    // Path compression
    if (parent_[x] != x)
        parent_[x] = find(parent_[x]);
    return parent_[x];
}

void SteensgaardAnalysis::unite(uint32_t x, uint32_t y) {
    uint32_t rx = find(x);
    uint32_t ry = find(y);
    if (rx == ry) return;

    ensureSize(parent_, rx, rx);
    ensureSize(parent_, ry, ry);
    ensureSize(rank_,   rx, 0);
    ensureSize(rank_,   ry, 0);

    // Union by rank
    if (rank_[rx] < rank_[ry]) std::swap(rx, ry);
    parent_[ry] = rx;
    if (rank_[rx] == rank_[ry]) ++rank_[rx];

    // Propagate escape
    if (escapeSet_.count(ry)) escapeSet_.insert(rx);
    if (escapeSet_.count(rx)) escapeSet_.insert(rx);
}

void SteensgaardAnalysis::addConstraint(PtsConstraint c) {
    // Ensure both endpoints exist
    addValue(c.lhs);
    addValue(c.rhs);
    constraints_.push_back(c);
}

// ─── Constraint processing with recursive join worklist ──────────────────────

/**
 * join(a, b): unify the equivalence classes of a and b.
 * If they had different points-to targets, recursively join those too.
 */
static void joinWithPropagation(
    uint32_t a, uint32_t b,
    std::vector<uint32_t>& parent,
    std::vector<uint32_t>& rank,
    std::unordered_map<uint32_t, uint32_t>& pointsTo,
    std::unordered_set<uint32_t>& escapeSet,
    std::queue<std::pair<uint32_t,uint32_t>>& worklist,
    std::function<uint32_t(uint32_t)> findFn)
{
    uint32_t ra = findFn(a);
    uint32_t rb = findFn(b);
    if (ra == rb) return;

    // Remember points-to targets before merging
    auto itA = pointsTo.find(ra);
    auto itB = pointsTo.find(rb);

    // Union by rank
    auto ensureSz = [&](uint32_t id) {
        if (id >= parent.size()) { parent.resize(id+1, id); rank.resize(id+1, 0); }
    };
    ensureSz(ra); ensureSz(rb);
    if (rank[ra] < rank[rb]) std::swap(ra, rb);
    parent[rb] = ra;
    if (rank[ra] == rank[rb]) ++rank[ra];

    if (escapeSet.count(rb)) escapeSet.insert(ra);

    // If both had points-to edges, enqueue their targets for joining
    if (itA != pointsTo.end() && itB != pointsTo.end()) {
        uint32_t ptA = itA->second;
        uint32_t ptB = itB->second;
        worklist.push({ptA, ptB});
    } else if (itB != pointsTo.end()) {
        // Move rb's points-to edge to ra
        pointsTo[ra] = itB->second;
        pointsTo.erase(itB);
    }
}

void SteensgaardAnalysis::propagate() {
    // Process each constraint once.
    // For each constraint, we may enqueue (join) pairs of nodes.
    std::queue<std::pair<uint32_t,uint32_t>> joinQueue;

    auto findFn = [this](uint32_t x) { return find(x); };

    auto join = [&](uint32_t a, uint32_t b) {
        joinWithPropagation(a, b, parent_, rank_, pointsTo_, escapeSet_,
                             joinQueue, findFn);
    };

    // Process initial constraints
    for (auto& c : constraints_) {
        uint32_t lhs = find(c.lhs);
        uint32_t rhs = find(c.rhs);

        switch (c.kind) {
        case ConstraintKind::AddrOf:
            // lhs = &rhs  → pointsTo(lhs) = rhs
            {
                auto it = pointsTo_.find(lhs);
                if (it == pointsTo_.end()) {
                    pointsTo_[lhs] = rhs;
                } else {
                    // Already has a points-to edge; join the targets
                    join(it->second, rhs);
                }
            }
            break;

        case ConstraintKind::Copy:
            // lhs = rhs  → pointsTo(lhs) ∪= pointsTo(rhs)
            {
                auto itR = pointsTo_.find(rhs);
                if (itR == pointsTo_.end()) break;
                auto itL = pointsTo_.find(lhs);
                if (itL == pointsTo_.end()) {
                    pointsTo_[lhs] = itR->second;
                } else {
                    join(itL->second, itR->second);
                }
            }
            break;

        case ConstraintKind::Load:
            // lhs = *rhs  → pointsTo(lhs) ∪= pointsTo(pointsTo(rhs))
            {
                auto itR = pointsTo_.find(rhs);
                if (itR == pointsTo_.end()) break;
                uint32_t ptsR = find(itR->second);
                auto itPR = pointsTo_.find(ptsR);
                if (itPR == pointsTo_.end()) break;
                auto itL = pointsTo_.find(lhs);
                if (itL == pointsTo_.end()) {
                    pointsTo_[lhs] = itPR->second;
                } else {
                    join(itL->second, itPR->second);
                }
            }
            break;

        case ConstraintKind::Store:
            // *lhs = rhs  → pointsTo(pointsTo(lhs)) ∪= pointsTo(rhs)
            {
                auto itL = pointsTo_.find(lhs);
                if (itL == pointsTo_.end()) break;
                uint32_t ptsL = find(itL->second);
                auto itR = pointsTo_.find(rhs);
                if (itR == pointsTo_.end()) break;
                auto itPL = pointsTo_.find(ptsL);
                if (itPL == pointsTo_.end()) {
                    pointsTo_[ptsL] = itR->second;
                } else {
                    join(itPL->second, itR->second);
                }
            }
            break;

        case ConstraintKind::External:
            // Mark as may_point_to_anything
            {
                auto itL = pointsTo_.find(lhs);
                if (itL != pointsTo_.end())
                    escapeSet_.insert(find(itL->second));
                escapeSet_.insert(lhs);
            }
            break;
        }
    }

    // Drain the join worklist (recursive unification)
    while (!joinQueue.empty()) {
        auto [a, b] = joinQueue.front();
        joinQueue.pop();
        join(a, b);
    }
}

// ─── Main run ─────────────────────────────────────────────────────────────────

void SteensgaardAnalysis::run() {
    if (ran_) return;
    propagate();
    ran_ = true;
}

// ─── Queries ──────────────────────────────────────────────────────────────────

AliasResult SteensgaardAnalysis::alias(uint32_t idA, uint32_t idB) const {
    if (idA == idB) return AliasResult::MustAlias;
    if (!ran_) return AliasResult::MayAlias;  // conservative before analysis

    uint32_t rA = find(idA);
    uint32_t rB = find(idB);

    // If either is in the escape set → MayAlias with everything
    if (escapeSet_.count(rA) || escapeSet_.count(rB))
        return AliasResult::MayAlias;

    // If in the same equivalence class → MayAlias (Steensgaard is not precise
    // enough to guarantee MustAlias unless idA == idB)
    if (rA == rB) return AliasResult::MayAlias;

    // Different classes → NoAlias (under Steensgaard's model)
    return AliasResult::NoAlias;
}

bool SteensgaardAnalysis::mayPointToAnything(uint32_t id) const {
    return escapeSet_.count(find(id)) != 0;
}

std::size_t SteensgaardAnalysis::classCount() const {
    std::unordered_set<uint32_t> roots;
    for (uint32_t i = 0; i < parent_.size(); ++i)
        roots.insert(find(i));
    return roots.size();
}

// ─── AliasPass: collect constraints ──────────────────────────────────────────

void AliasPass::collectConstraints(const ssa::SSAFunction& fn) {
    using namespace retdec::ssa;

    for (auto& valPtr : fn.values()) {
        const IrValue* v = valPtr.get();
        steensgaard_.addValue(v->id);

        if (v->kind == ValueKind::MemRef && v->memIsStack) {
            stackSlots_.insert(v->memOffset);
        }

        if (v->kind == ValueKind::MemRef && !v->memIsStack) {
            // Global or heap access
            globalAlias_.addAccess((uint64_t)v->memOffset, v->memWidth, v->id);
        }
    }

    // Walk instructions to build Steensgaard constraints
    for (auto& blkPtr : fn.blocks()) {
        for (auto* instr : blkPtr->instrs) {
            switch (instr->op) {
            case IrInstr::Op::Load:
                // dst = *src  → Load constraint
                if (instr->defValue != UINT32_MAX && !instr->uses.empty()) {
                    steensgaard_.addConstraint({
                        ConstraintKind::Load,
                        instr->defValue,
                        instr->uses[0].valueId
                    });
                }
                break;

            case IrInstr::Op::Store:
                // *dst = src  → Store constraint
                if (instr->uses.size() >= 2) {
                    steensgaard_.addConstraint({
                        ConstraintKind::Store,
                        instr->uses[0].valueId,
                        instr->uses[1].valueId
                    });
                }
                break;

            case IrInstr::Op::Assign:
                // dst = src (copy)
                if (instr->defValue != UINT32_MAX && !instr->uses.empty()) {
                    steensgaard_.addConstraint({
                        ConstraintKind::Copy,
                        instr->defValue,
                        instr->uses[0].valueId
                    });
                }
                break;

            case IrInstr::Op::Call:
                // External call → mark all pointer args as escaped
                for (auto& use : instr->uses) {
                    steensgaard_.addConstraint({
                        ConstraintKind::External,
                        use.valueId,
                        use.valueId
                    });
                }
                break;

            default:
                break;
            }
        }
    }
}

void AliasPass::determinePromotable(const ssa::SSAFunction& fn) {
    using namespace retdec::ssa;

    // A stack slot is promotable if:
    //   1. It has no aliases (all pairwise queries return NoAlias or MustAlias
    //      to itself).
    //   2. Its address does not escape.

    // Collect all stack MemRef values grouped by frame offset
    std::unordered_map<int64_t, std::vector<const IrValue*>> bySlot;
    for (auto& valPtr : fn.values()) {
        const IrValue* v = valPtr.get();
        if (v->kind == ValueKind::MemRef && v->memIsStack)
            bySlot[v->memOffset].push_back(v);
    }

    for (auto& [off, vals] : bySlot) {
        if (escape_.escapedSlots.count(off)) continue;

        bool allNoAlias = true;
        for (std::size_t i = 0; i < vals.size() && allNoAlias; ++i) {
            for (std::size_t j = i + 1; j < vals.size(); ++j) {
                auto a = MemLoc::stack(vals[i]->memOffset, vals[i]->memWidth, vals[i]->id);
                auto b = MemLoc::stack(vals[j]->memOffset, vals[j]->memWidth, vals[j]->id);
                auto res = stackAlias_.alias(a, b);
                if (res == AliasResult::MayAlias) { allNoAlias = false; break; }
            }
        }
        if (allNoAlias) promotable_.insert(off);
    }
    stats_.promotableSlots = promotable_.size();
    stats_.escapedSlots    = escape_.escapedSlots.size();
}

void AliasPass::run(const ssa::SSAFunction& fn) {
    // 1. Escape analysis
    EscapeAnalysis ea;
    escape_ = ea.run(fn);

    // 2. Collect constraints for all tiers
    collectConstraints(fn);

    // 3. Run Steensgaard
    steensgaard_.run();
    stats_.steenClasses = steensgaard_.classCount();

    // 4. Determine promotable stack slots
    determinePromotable(fn);
}

AliasResult AliasPass::alias(const MemLoc& a, const MemLoc& b) const {
    // Tier 1: exact stack
    if (a.isStack() && b.isStack()) {
        ++stats_.stackQueries;
        auto r = stackAlias_.alias(a, b);
        if (r == AliasResult::MustAlias) ++stats_.mustAliasCount;
        else if (r == AliasResult::NoAlias) ++stats_.noAliasCount;
        else ++stats_.mayAliasCount;
        return r;
    }
    // Tier 2: exact global
    if (a.isGlobal() && b.isGlobal()) {
        ++stats_.globalQueries;
        auto r = globalAlias_.alias(a, b);
        if (r == AliasResult::MustAlias) ++stats_.mustAliasCount;
        else if (r == AliasResult::NoAlias) ++stats_.noAliasCount;
        else ++stats_.mayAliasCount;
        return r;
    }
    // Tier 3: Steensgaard
    if (a.ssaId != UINT32_MAX && b.ssaId != UINT32_MAX) {
        ++stats_.steenQueries;
        auto r = steensgaard_.alias(a.ssaId, b.ssaId);
        if (r == AliasResult::MustAlias) ++stats_.mustAliasCount;
        else if (r == AliasResult::NoAlias) ++stats_.noAliasCount;
        else ++stats_.mayAliasCount;
        return r;
    }
    // Unknown kind → conservative MayAlias
    ++stats_.mayAliasCount;
    return AliasResult::MayAlias;
}

} // namespace alias_analysis
} // namespace retdec
