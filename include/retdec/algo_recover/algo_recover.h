/**
 * @file include/retdec/algo_recover/algo_recover.h
 * @brief STL Recovery — Algorithm Header Recovery (Stage 27).
 *
 * ## Overview
 *
 * This module recognises compiled `<algorithm>` loop patterns in the SSA IR
 * and emits idiomatic C++ standard-library calls in their place.
 *
 * Each detected algorithm is described by an `AlgorithmResult` that carries:
 *   - `kind`         — which `<algorithm>` call was identified
 *   - `confidence`   — in [0.0, 1.0]
 *   - `emissionTier` — High/Medium/Low determines emit form:
 *       High   → `std::transform(first, last, out, f)`
 *       Medium → `/* std::transform? *‌/ for (auto& e : v) out[i++] = f(e);`
 *       Low    → plain loop with no annotation
 *   - `emittedForm`  — the ready-to-emit C++ string for this site
 *
 * ## Algorithms detected
 *
 * ### std::transform
 *
 * Structural invariant:
 *   - A loop with a single Load from a source pointer and a single Store to
 *     a destination pointer, both advanced by the same stride.
 *   - The stored value is a function application of the loaded value (direct
 *     computation or Call in the loop body).
 *   - Source and destination ranges do not alias (non-overlapping base pointers).
 *
 * In IR:
 *   ```
 *   LOOP:
 *     %elem  = Load(src_ptr)
 *     %result= f(%elem)           // any computation or Call
 *     Store(%result, dst_ptr)
 *     src_ptr = src_ptr + stride
 *     dst_ptr = dst_ptr + stride
 *     Compare(src_ptr, src_end)
 *     Branch LOOP | EXIT
 *   ```
 *
 * ### std::accumulate
 *
 * Structural invariant:
 *   - A phi node carrying the accumulator across loop iterations.
 *   - A binary operation combining the accumulator with the loaded element.
 *   - No store inside the loop (result is only the phi value after the loop).
 *
 * Supported combiners:
 *   - Add  → `std::accumulate(first, last, 0)`
 *   - Mul  → `std::accumulate(first, last, 1, std::multiplies<>{})`
 *   - Or   → `std::accumulate(first, last, 0, std::bit_or<>{})`
 *   - Xor  → `std::accumulate(first, last, 0, std::bit_xor<>{})`
 *   - max  → `*std::max_element(first, last)`
 *   - min  → `*std::min_element(first, last)`
 *
 * ### std::find / std::find_if
 *
 * Structural invariant:
 *   - A loop with one equality or predicate Compare against the loaded element.
 *   - Early exit: a conditional Branch that leaves the loop when the condition
 *     is true (the element is found), returning the current pointer.
 *   - No store inside the loop.
 *
 * Variants:
 *   - Constant comparand → `std::find(first, last, value)`
 *   - Computed comparand (lambda/function) → `std::find_if(first, last, pred)`
 *
 * ### std::partition
 *
 * Structural invariant:
 *   - Converging-index pattern: two pointer/index variables starting at
 *     opposite ends, approaching each other (one incremented, one decremented).
 *   - An element swap when the ordering predicate fails.
 *   - Convergence check: `left >= right` or `left == right` exits the loop.
 *   - **No** recursive calls and **no** depth counter (that would be sort).
 *
 * ### std::for_each
 *
 * Structural invariant:
 *   - A loop with a single Call per element (no result used from the call).
 *   - No accumulator phi node.
 *   - No store of a computed value to a destination range.
 *
 * ### Iterator pattern recovery
 *
 * `IteratorPatternRecovery` converts raw pointer-arithmetic loops to
 * range-based for syntax:
 *   - `begin/end` pair → `for (auto& e : v)`
 *   - `rbegin/rend` pair → `for (auto& e : std::ranges::reverse_view(v))`
 *   - `back_inserter` pattern (push_back in loop body) → emit with
 *     `std::back_inserter(dst)` as output iterator for transform.
 */

#ifndef RETDEC_ALGO_RECOVER_H
#define RETDEC_ALGO_RECOVER_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retdec {
namespace ssa { class SSAFunction; }
} // namespace retdec

namespace retdec {
namespace algo_recover {

// ─── Enumerations ─────────────────────────────────────────────────────────────

enum class AlgorithmKind : uint8_t {
    Unknown,
    Transform,       ///< std::transform
    Accumulate,      ///< std::accumulate
    MaxElement,      ///< *std::max_element
    MinElement,      ///< *std::min_element
    Find,            ///< std::find
    FindIf,          ///< std::find_if
    Partition,       ///< std::partition
    ForEach,         ///< std::for_each
    Copy,            ///< std::copy (transform with identity)
    Fill,            ///< std::fill (store constant in loop)
    Count,           ///< std::count / std::count_if
    AnyOf,           ///< std::any_of
    AllOf,           ///< std::all_of
    NoneOf,          ///< std::none_of
    Reverse,         ///< std::reverse (swap from ends)
    RotateLeft,      ///< std::rotate
};

/// Binary combiner used by accumulate-family algorithms.
enum class CombinerKind : uint8_t {
    Unknown,
    Add,        ///< +   → std::accumulate with default op
    Mul,        ///< *   → std::multiplies<>{}
    Or,         ///< |   → std::bit_or<>{}
    Xor,        ///< ^   → std::bit_xor<>{}
    And,        ///< &   → std::bit_and<>{}
    Max,        ///< max → std::max_element
    Min,        ///< min → std::min_element
};

/// How confident we are — drives the emission form.
enum class EmissionTier : uint8_t {
    Low,        ///< plain loop, no annotation
    Medium,     ///< comment-annotated loop
    High,       ///< full std:: call
};

// ─── Algorithm detection result ───────────────────────────────────────────────

struct AlgorithmResult {
    AlgorithmKind kind            = AlgorithmKind::Unknown;
    float         confidence      = 0.0f;
    EmissionTier  tier            = EmissionTier::Low;
    CombinerKind  combiner        = CombinerKind::Unknown; ///< for Accumulate
    bool          hasLambda       = false;  ///< inlined lambda / predicate
    bool          hasBackInserter = false;  ///< output via push_back
    bool          isReverse       = false;  ///< rbegin/rend loop

    std::string   emittedForm;    ///< ready-to-emit C++ string
    std::string   kindName() const noexcept;
    std::string   toString() const;
};

// ─── Evidence structs ─────────────────────────────────────────────────────────

struct TransformEvidence {
    bool  found              = false;
    float confidence         = 0.0f;
    bool  hasSrcDstLoad      = false;  ///< load from src, store to dst
    bool  hasTwoPtrsAdvanced = false;  ///< both ptrs incremented in same loop
    bool  hasNoReorder       = false;  ///< no src-dst overlap / sort pattern
    bool  hasLambdaCall      = false;  ///< inlined function/call in loop body
    bool  hasBackInserter    = false;  ///< output via push_back
};

struct AccumulateEvidence {
    bool        found       = false;
    float       confidence  = 0.0f;
    bool        hasPhi      = false;  ///< accumulator phi node
    bool        hasBinOp    = false;  ///< acc combined with element
    bool        hasNoStore  = false;  ///< no store in loop body
    CombinerKind combiner   = CombinerKind::Unknown;
};

struct FindEvidence {
    bool  found         = false;
    float confidence    = 0.0f;
    bool  hasCompare    = false;  ///< equality or predicate compare
    bool  hasEarlyExit  = false;  ///< conditional branch leaving loop
    bool  hasNoStore    = false;  ///< no store in loop
    bool  hasLambda     = false;  ///< predicate (not constant comparand)
};

struct PartitionEvidence {
    bool  found               = false;
    float confidence          = 0.0f;
    bool  hasConvergingPtrs   = false;  ///< two ptrs from opposite ends
    bool  hasSwap             = false;  ///< element swap pattern
    bool  hasConvergenceCheck = false;  ///< left >= right exit
    bool  hasNoRecursion      = false;  ///< not a sort (no recursive call)
};

struct ForEachEvidence {
    bool  found         = false;
    float confidence    = 0.0f;
    bool  hasLoopCall   = false;  ///< Call in loop body
    bool  hasNoPhi      = false;  ///< no accumulator
    bool  hasNoDstStore = false;  ///< no computed store to dst range
};

// ─── Per-algorithm detectors ──────────────────────────────────────────────────

class IAlgorithmDetector {
public:
    virtual ~IAlgorithmDetector() = default;
    virtual AlgorithmResult detect(const ssa::SSAFunction& fn) const = 0;
    virtual AlgorithmKind kind() const noexcept = 0;
};

/** std::transform detector. */
class TransformDetector : public IAlgorithmDetector {
public:
    AlgorithmResult detect(const ssa::SSAFunction& fn) const override;
    AlgorithmKind   kind() const noexcept override { return AlgorithmKind::Transform; }
private:
    TransformEvidence analyse(const ssa::SSAFunction& fn) const;
    float             score(const TransformEvidence& ev) const;
    std::string       emit(const TransformEvidence& ev, EmissionTier tier) const;
};

/** std::accumulate / max_element / min_element detector. */
class AccumulateDetector : public IAlgorithmDetector {
public:
    AlgorithmResult detect(const ssa::SSAFunction& fn) const override;
    AlgorithmKind   kind() const noexcept override { return AlgorithmKind::Accumulate; }
private:
    AccumulateEvidence analyse(const ssa::SSAFunction& fn) const;
    float              score(const AccumulateEvidence& ev) const;
    CombinerKind       detectCombiner(const ssa::SSAFunction& fn) const;
    std::string        emit(const AccumulateEvidence& ev, AlgorithmKind k,
                            EmissionTier tier) const;
};

/** std::find / std::find_if detector. */
class FindDetector : public IAlgorithmDetector {
public:
    AlgorithmResult detect(const ssa::SSAFunction& fn) const override;
    AlgorithmKind   kind() const noexcept override { return AlgorithmKind::Find; }
private:
    FindEvidence analyseFind(const ssa::SSAFunction& fn) const;
    float        score(const FindEvidence& ev) const;
    std::string  emit(const FindEvidence& ev, EmissionTier tier) const;
};

/** std::partition detector (converging-index, standalone). */
class PartitionDetector : public IAlgorithmDetector {
public:
    AlgorithmResult detect(const ssa::SSAFunction& fn) const override;
    AlgorithmKind   kind() const noexcept override { return AlgorithmKind::Partition; }
private:
    PartitionEvidence analyse(const ssa::SSAFunction& fn) const;
    float             score(const PartitionEvidence& ev) const;
    bool              hasRecursion(const ssa::SSAFunction& fn) const;
};

/** std::for_each detector. */
class ForEachDetector : public IAlgorithmDetector {
public:
    AlgorithmResult detect(const ssa::SSAFunction& fn) const override;
    AlgorithmKind   kind() const noexcept override { return AlgorithmKind::ForEach; }
private:
    ForEachEvidence analyse(const ssa::SSAFunction& fn) const;
    float           score(const ForEachEvidence& ev) const;
};

// ─── Iterator pattern recovery ────────────────────────────────────────────────

/**
 * Converts raw pointer-arithmetic loops to range-based for syntax.
 *
 * Patterns:
 *   begin/end pair → `for (auto& e : v)`
 *   rbegin/rend    → `for (auto& e : std::ranges::reverse_view(v))`
 *   push_back sink → emits `std::back_inserter(dst)` as output iterator.
 */
class IteratorPatternRecovery {
public:
    struct IteratorResult {
        bool        isBeginEnd     = false;
        bool        isReverseIter  = false;
        bool        hasBackInserter= false;
        std::string rangeForForm;   ///< "for (auto& e : v)"
        std::string backInserter;   ///< "std::back_inserter(dst)"
    };

    IteratorResult recover(const ssa::SSAFunction& fn) const;

private:
    bool hasBeginEndPair(const ssa::SSAFunction& fn) const;
    bool hasReverseIter(const ssa::SSAFunction& fn) const;
    bool hasBackInserter(const ssa::SSAFunction& fn) const;
};

// ─── Algorithm detector orchestrator ─────────────────────────────────────────

/**
 * Top-level algorithm detection pass.
 *
 * Runs all registered detectors and returns the highest-confidence result,
 * then applies iterator pattern recovery to augment the emitted form.
 */
class AlgorithmDetector {
public:
    struct Config {
        float highTierThreshold   = 0.75f;
        float mediumTierThreshold = 0.45f;
        int   minBlocks           = 2;
        int   minInstrs           = 5;
        bool  runTransform        = true;
        bool  runAccumulate       = true;
        bool  runFind             = true;
        bool  runPartition        = true;
        bool  runForEach          = true;
    };
    static Config defaultConfig() noexcept { return {}; }

    struct Stats {
        uint32_t functionsAnalysed  = 0;
        uint32_t functionsSkipped   = 0;
        uint32_t detections         = 0;
        uint32_t highTier           = 0;
        uint32_t mediumTier         = 0;
        uint32_t lowTier            = 0;
    };

    using DetectionMap = std::vector<std::pair<std::string, AlgorithmResult>>;

    explicit AlgorithmDetector(Config cfg = defaultConfig());

    AlgorithmResult detect(const ssa::SSAFunction& fn) const;
    DetectionMap    detectModule(
        const std::vector<const ssa::SSAFunction*>& fns) const;

    const Stats& stats() const { return stats_; }

private:
    Config cfg_;
    mutable Stats stats_;
    std::vector<std::unique_ptr<IAlgorithmDetector>> detectors_;
    IteratorPatternRecovery iterRecover_;

    bool          passesPreflight(const ssa::SSAFunction& fn) const;
    EmissionTier  assignTier(float confidence) const;
};

} // namespace algo_recover
} // namespace retdec

#endif // RETDEC_ALGO_RECOVER_H
