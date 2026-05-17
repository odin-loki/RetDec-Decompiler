/**
 * @file include/retdec/alias_analysis/alias_analysis.h
 * @brief Stratified Alias Analysis — three tiers for stack, globals, and heap.
 *
 * ## Design overview
 *
 * This module implements a three-tier alias analysis hierarchy:
 *
 *   Tier 1 — StackAliasAnalysis     (exact, O(1) per query)
 *   Tier 2 — GlobalAliasAnalysis    (exact, O(1) per query)
 *   Tier 3 — SteensgaardAnalysis    (flow-insensitive, union-find, O(n α(n)))
 *
 * Plus a companion EscapeAnalysis for pointer parameters.
 *
 * The design follows a "stratification" principle: cheaper and more precise
 * analyses handle the common cases (stack and global variables), while the
 * expensive Steensgaard analysis handles only heap-allocated and
 * indeterminate pointers.
 *
 * ---
 *
 * ## Tier 1: StackAliasAnalysis
 *
 * Two stack accesses A and B alias if and only if their byte ranges overlap:
 *
 *   A.offset < B.offset + B.size  AND  B.offset < A.offset + A.size
 *
 * This is exact because:
 *   (a) Each stack slot has a unique offset from the frame base.
 *   (b) Stack frames are not shared between functions (no aliasing across calls
 *       unless address of local was taken — handled by EscapeAnalysis).
 *   (c) We only consider accesses within one frame (same function).
 *
 * Complexity: O(1) per query.
 *
 * ## Tier 2: GlobalAliasAnalysis
 *
 * Same byte-range overlap check for accesses to statically-known absolute
 * addresses.  Two accesses alias iff their absolute address ranges overlap.
 *
 * Limitation: only covers accesses where the base address is a link-time
 * constant (e.g. accesses to global arrays, vtables, string literals).
 * Accesses through pointers that may or may not point to globals are
 * classified as "may alias" and handed to Steensgaard.
 *
 * ## Tier 3: SteensgaardAnalysis (OCL Steensgaard)
 *
 * Applies to heap and indeterminate pointers.  Uses the Steensgaard
 * (1996) unification-based algorithm:
 *
 *   For each store `*p = q`:
 *     points_to(p) UNION points_to(q)
 *
 *   For each load `r = *p`:
 *     points_to(r) UNION points_to(points_to(p))
 *
 *   For each call `f(p)` where f is external:
 *     mark points_to(p) as may_point_to_anything
 *
 * The union-find structure merges alias classes.  Two pointers p and q
 * may-alias iff they belong to the same equivalence class.
 *
 * The "OCL" in the name refers to Task 6's OpenCL constraint-solver kernel,
 * which can accelerate the constraint propagation for large programs.
 * For functions below a threshold (kOCLThreshold = 500 values), the
 * in-process union-find runs directly.  For larger functions, the
 * constraint set is serialised and dispatched to the OCL kernel.
 *
 * ## EscapeAnalysis
 *
 * A pointer value "escapes" from a function if it may be accessible after
 * the function returns:
 *
 *   1. Stored to a non-local memory location (global write, heap write).
 *   2. Passed to an external/opaque function call.
 *   3. Returned from the function.
 *
 * Stack slots for which the address was taken (via LEA/ADDROF) and the
 * resulting pointer escapes are marked as `mayEscape = true`.  These slots
 * cannot be safely promoted to SSA scalars.
 *
 * ## Alias result
 *
 * All three tiers expose a unified query interface:
 *
 *   AliasResult alias(MemLoc a, MemLoc b) const
 *     → MustAlias   — provably the same memory location
 *     → MayAlias    — could overlap (conservative)
 *     → NoAlias     — provably disjoint
 *
 * The AliasPass calls all three tiers in priority order:
 *   1. If both are stack locations → StackAliasAnalysis (exact).
 *   2. If both are known static addresses → GlobalAliasAnalysis (exact).
 *   3. Otherwise → SteensgaardAnalysis (conservative).
 *
 * ## Integration
 *
 * After the AliasPass runs:
 *   - MemRef values for stack slots with no aliases are promoted to SSA
 *     scalars (feeding into the renaming pass for those vars).
 *   - EscapeInfo is attached to the FunctionSummary for inter-procedural
 *     analysis (Stage 23).
 *   - All alias results feed TypeInference (Stage 19) and CodeGen (Stage 24).
 */

#ifndef RETDEC_ALIAS_ANALYSIS_H
#define RETDEC_ALIAS_ANALYSIS_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace ssa {
class SSAFunction;
struct IrValue;
} // namespace ssa
} // namespace retdec

namespace retdec {
namespace alias_analysis {

// ─── Alias result ────────────────────────────────────────────────────────────

enum class AliasResult : uint8_t {
    MustAlias,  ///< Provably the same memory location
    MayAlias,   ///< Could overlap (conservative)
    NoAlias,    ///< Provably disjoint
};

inline const char* aliasResultName(AliasResult r) noexcept {
    switch (r) {
    case AliasResult::MustAlias: return "MustAlias";
    case AliasResult::MayAlias:  return "MayAlias";
    case AliasResult::NoAlias:   return "NoAlias";
    }
    return "?";
}

// ─── Memory location descriptor ──────────────────────────────────────────────

/**
 * Identifies one memory location for alias queries.
 *
 * Kind classification:
 *   Stack   — frame-relative access (base = frame base, offset = frame offset)
 *   Global  — absolute address (base = 0, offset = absolute_vma)
 *   Heap    — dynamically allocated (base = allocating SSA value ID, offset arbitrary)
 *   Unknown — indeterminate (conservative: MayAlias with everything)
 */
enum class MemLocKind : uint8_t { Stack, Global, Heap, Unknown };

struct MemLoc {
    MemLocKind kind    = MemLocKind::Unknown;
    int64_t    offset  = 0;   ///< frame offset (Stack) or absolute VMA (Global)
    uint8_t    size    = 0;   ///< access width in bytes
    uint32_t   ssaId   = UINT32_MAX; ///< SSA value ID (for Steensgaard lookups)
    uint64_t   baseVMA = 0;   ///< base address (Global) or alloc site VMA (Heap)

    bool isStack()  const { return kind == MemLocKind::Stack;  }
    bool isGlobal() const { return kind == MemLocKind::Global; }
    bool isHeap()   const { return kind == MemLocKind::Heap;   }

    static MemLoc stack(int64_t off, uint8_t sz, uint32_t id = UINT32_MAX) {
        return {MemLocKind::Stack, off, sz, id, 0};
    }
    static MemLoc global(uint64_t addr, uint8_t sz, uint32_t id = UINT32_MAX) {
        return {MemLocKind::Global, (int64_t)addr, sz, id, addr};
    }
    static MemLoc heap(uint32_t id, int64_t off, uint8_t sz) {
        return {MemLocKind::Heap, off, sz, id, 0};
    }
    static MemLoc unknown() {
        return {MemLocKind::Unknown, 0, 0, UINT32_MAX, 0};
    }
};

// ─── Tier 1: Stack alias analysis ────────────────────────────────────────────

/**
 * Exact alias analysis for stack frame accesses.
 *
 * Query: alias(a, b) where a.kind == b.kind == Stack
 *   → NoAlias   if byte ranges are disjoint
 *   → MustAlias if ranges are identical (same offset and size)
 *   → MayAlias  if ranges overlap but are not identical
 *     (e.g. 4-byte access to the same slot as an 8-byte access)
 */
class StackAliasAnalysis {
public:
    AliasResult alias(const MemLoc& a, const MemLoc& b) const noexcept;

    /// Returns true iff [a.offset, a.offset+a.size) and
    ///              [b.offset, b.offset+b.size) overlap.
    static bool byteRangesOverlap(int64_t offA, uint8_t szA,
                                   int64_t offB, uint8_t szB) noexcept;
};

// ─── Tier 2: Global alias analysis ───────────────────────────────────────────

/**
 * Exact alias analysis for statically-known absolute addresses.
 *
 * Populated by scanning the IR for accesses of the form:
 *   MOV [0x404000], rax   — load/store to absolute address
 *
 * For each such access, the (address, size) pair is recorded.
 * Two accesses alias iff their absolute address ranges overlap.
 */
class GlobalAliasAnalysis {
public:
    void addAccess(uint64_t addr, uint8_t size, uint32_t ssaId);
    AliasResult alias(const MemLoc& a, const MemLoc& b) const noexcept;

    std::size_t accessCount() const { return accesses_.size(); }

private:
    struct GlobalAccess {
        uint64_t addr;
        uint8_t  size;
        uint32_t ssaId;
    };
    std::vector<GlobalAccess> accesses_;
};

// ─── Steensgaard constraint types ────────────────────────────────────────────

/**
 * One Steensgaard points-to constraint.
 *
 *   StoreConstraint:  *dst = src  → points_to(dst) ∪= points_to(src)
 *   LoadConstraint:   dst = *src  → points_to(dst) = points_to(points_to(src))
 *   CopyConstraint:   dst = src   → points_to(dst) ∪= points_to(src)
 *   AddrOfConstraint: dst = &src  → points_to(dst) = {src}
 *   ExternalConstraint: escape    → mark as may_point_to_anything
 */
enum class ConstraintKind : uint8_t {
    Store,    ///< *dst = src
    Load,     ///< dst  = *src
    Copy,     ///< dst  = src  (pointer assignment)
    AddrOf,   ///< dst  = &src (address-of)
    External, ///< dst passed to external / unknown function
};

struct PtsConstraint {
    ConstraintKind kind;
    uint32_t lhs;  ///< destination SSA value ID
    uint32_t rhs;  ///< source SSA value ID
};

// ─── Tier 3: Steensgaard analysis ────────────────────────────────────────────

/**
 * Flow-insensitive, unification-based alias analysis for heap pointers.
 *
 * Uses a union-find (disjoint-set) structure to merge alias classes.
 * The algorithm has near-linear complexity O(n α(n)) where n = number of
 * SSA values and α is the inverse Ackermann function.
 *
 * Two pointers may-alias iff they are in the same alias class (same
 * union-find root after constraint propagation).
 *
 * ### OCL dispatch
 *
 * For functions with more than kOCLThreshold pointer values, the
 * constraint set is serialised and dispatched to the OCL kernel
 * (from Task 6: `OCLSteensgaard`).  For smaller functions, the
 * in-process union-find is used directly.
 */
class SteensgaardAnalysis {
public:
    static constexpr std::size_t kOCLThreshold = 500;

    SteensgaardAnalysis();
    ~SteensgaardAnalysis();

    /// Add an SSA value to the analysis universe.
    void addValue(uint32_t id, bool isPointer = true);

    /// Add a points-to constraint.
    void addConstraint(PtsConstraint c);

    /// Run the analysis.  After this, alias() is valid.
    void run();

    /// Query: do pointers a and b may-alias?
    AliasResult alias(uint32_t idA, uint32_t idB) const;

    /// True if `id` may point to anything (escaped, external, etc.)
    bool mayPointToAnything(uint32_t id) const;

    /// Number of alias classes after analysis.
    std::size_t classCount() const;

    std::size_t constraintCount() const { return constraints_.size(); }
    std::size_t valueCount() const { return parent_.size(); }

private:
    // Union-find
    uint32_t find(uint32_t x) const;
    void     unite(uint32_t x, uint32_t y);

    void processCopy(uint32_t lhs, uint32_t rhs);
    void processAddrOf(uint32_t lhs, uint32_t rhs);
    void processStore(uint32_t dst, uint32_t src);
    void processLoad(uint32_t dst, uint32_t src);
    void propagate();

    // Union-find arrays (indexed by SSA value ID, resized on demand)
    mutable std::vector<uint32_t> parent_;
    mutable std::vector<uint32_t> rank_;

    // Points-to sets: for each root, the set of values in its points-to target
    std::unordered_map<uint32_t, uint32_t> pointsTo_;  // root → target root

    std::unordered_set<uint32_t>  escapeSet_;   ///< may_point_to_anything
    std::vector<PtsConstraint>    constraints_;

    bool ran_ = false;
};

// ─── Escape analysis ─────────────────────────────────────────────────────────

/**
 * Detects which pointer values (and the stack slots they point to) escape
 * from the function.
 *
 * A pointer p escapes if:
 *   (a) p is stored to a non-local memory location (global, heap, or a
 *       already-escaped pointer's target).
 *   (b) p is passed to an external/opaque function call.
 *   (c) p is returned from the function.
 *
 * After run(), escapedValues() returns the set of SSA value IDs that
 * may escape.  Stack slots whose address (via ADDROF / LEA) is in this set
 * cannot be promoted to SSA scalars.
 */
class EscapeAnalysis {
public:
    struct EscapeInfo {
        std::unordered_set<uint32_t> escapedValues; ///< SSA IDs that escape
        std::unordered_set<int64_t>  escapedSlots;  ///< frame offsets of escaped stack slots
        bool returnEscapes = false;  ///< function returns a pointer to local
    };

    EscapeInfo run(const ssa::SSAFunction& fn) const;

private:
    bool isExternalCall(const ssa::SSAFunction& fn, uint32_t instrId) const;
};

// ─── Combined alias pass ─────────────────────────────────────────────────────

/**
 * Orchestrates all three alias tiers plus escape analysis.
 *
 * Processing order:
 *   1. Scan the SSA IR for stack/global/heap accesses.
 *   2. Run EscapeAnalysis → mark escaped stack slots.
 *   3. Run StackAliasAnalysis (populated from MemRef values).
 *   4. Run GlobalAliasAnalysis (populated from absolute-address accesses).
 *   5. Collect Steensgaard constraints from pointer operations.
 *   6. Run SteensgaardAnalysis.
 *
 * After run(), clients can call alias(a, b) which dispatches to the
 * appropriate tier.
 *
 * SSA promotion:
 *   Stack slots with NoAlias for every pair of accesses AND not in the
 *   escape set are candidates for SSA scalar promotion.
 *   The result is exposed via promotableSlots().
 */
class AliasPass {
public:
    struct Stats {
        std::size_t stackQueries   = 0;
        std::size_t globalQueries  = 0;
        std::size_t steenQueries   = 0;
        std::size_t mustAliasCount = 0;
        std::size_t noAliasCount   = 0;
        std::size_t mayAliasCount  = 0;
        std::size_t promotableSlots= 0;
        std::size_t escapedSlots   = 0;
        std::size_t steenClasses   = 0;
    };

    void run(const ssa::SSAFunction& fn);

    /// Query the alias relation between two memory locations.
    AliasResult alias(const MemLoc& a, const MemLoc& b) const;

    /// Set of frame offsets for stack slots that can be promoted to SSA scalars.
    const std::unordered_set<int64_t>& promotableSlots() const {
        return promotable_;
    }

    const EscapeAnalysis::EscapeInfo& escapeInfo() const { return escape_; }
    const Stats& stats() const { return stats_; }

private:
    StackAliasAnalysis  stackAlias_;
    GlobalAliasAnalysis globalAlias_;
    SteensgaardAnalysis steensgaard_;
    EscapeAnalysis::EscapeInfo escape_;
    mutable Stats stats_;

    std::unordered_set<int64_t> promotable_;
    std::unordered_set<int64_t> stackSlots_;

    void collectConstraints(const ssa::SSAFunction& fn);
    void determinePromotable(const ssa::SSAFunction& fn);
};

} // namespace alias_analysis
} // namespace retdec

#endif // RETDEC_ALIAS_ANALYSIS_H
