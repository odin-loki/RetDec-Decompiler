/**
 * @file include/retdec/sort_detect/sort_detect.h
 * @brief STL Recovery — Sorting Algorithm Detector (Stage 25).
 *
 * ## Overview
 *
 * This module analyses the SSA IR of compiled functions to identify which
 * well-known sorting algorithm has been compiled into them.  Detection is
 * purely structural: it examines the loop shape, recursion pattern, index
 * arithmetic, and data-flow of the IR without executing it.
 *
 * ## Algorithms detected
 *
 * ### Introsort (std::sort in GCC libstdc++ / libc++)
 *
 * A three-phase hybrid:
 *   1. **Partition phase** — quicksort-style converging-index partition loop
 *      that performs swaps when two elements are out of order.
 *   2. **Depth guard** — a loop depth counter is decremented each recursive
 *      call; when it hits zero the algorithm delegates to heapsort.
 *   3. **Insertion sort tail** — for sub-ranges smaller than a threshold
 *      (typically 16 elements) an insertion sort is used instead.
 *
 * Confidence scoring:
 *   partition loop present              +0.50
 *   recursive calls on sub-ranges       +0.30
 *   insertion sort fallback (n < 16)    +0.20
 *
 * ### Mergesort
 *
 * Two recursive calls that halve the range (calls with begin+mid and mid+end),
 * followed by a merge loop that has a three-way branch structure:
 *   - advance left pointer  (left element consumed)
 *   - advance right pointer (right element consumed)
 *   - exit branch           (one side exhausted)
 *
 * An auxiliary buffer is allocated (malloc / alloca / VLA) with size
 * proportional to the input range.
 *
 * ### Heapsort
 *
 * Two phases:
 *   1. **Build-heap**: a loop from n/2 down to 0 that calls sift-down.
 *   2. **Sort**: a loop from n−1 down to 1 that extracts the root, swaps
 *      it to the end, then calls sift-down on the reduced heap.
 *
 * Sift-down signature: child index arithmetic `2*i+1` and `2*i+2`, selection
 * of the larger child, conditional swap if parent < child.
 *
 * ### Radix sort
 *
 * Zero comparison instructions (all branches are on loop counters or bit
 * fields, not element comparisons).  Four structural passes:
 *   1. **Nibble/byte extraction** — `(element >> k) & mask` for constant k.
 *   2. **Histogram accumulation** — frequency count into small fixed-size array.
 *   3. **Prefix-sum pass** — running sum over the histogram.
 *   4. **Redistribution** — scatter elements to output array using histogram.
 *
 * The above four passes repeat for each digit position.
 *
 * ## Output
 *
 * Each detector produces a `SortResult`:
 *   - `algorithm`  — detected algorithm name
 *   - `confidence` — in [0.0, 1.0]
 *   - `elementType`— recovered element type (from comparator analysis)
 *   - `compilerVariant` — GCC / Clang / MSVC / Unknown
 *   - `callSites`  — call-graph edges to helper functions (heapsort, merge)
 *
 * ## Shared fingerprint utilities
 *
 * ### PartitionFingerprint
 *
 * Detects the Hoare-style partition loop:
 *   - Two loop variables advancing from opposite ends of the range.
 *   - A comparison instruction between elements (the comparator).
 *   - A swap of the two elements when the comparison succeeds.
 *   - Loop exits when the two pointers cross.
 *
 * ### SiftDownFingerprint
 *
 * Detects the heap sift-down inner loop:
 *   - Index arithmetic of the form `left = 2*i+1`, `right = 2*i+2`.
 *   - A max-child selection (compare + conditional move).
 *   - A conditional swap if `element[parent] < element[child]`.
 *   - Tail iteration or tail recursion down the tree.
 *
 * ### RecursiveHalvingFingerprint
 *
 * Detects two recursive calls on halved ranges within the same function:
 *   - Self-calls where one argument is `begin+mid` and another `mid+end`.
 *   - Or equivalently: recursive calls whose argument ranges are
 *     contiguous sub-ranges of the parent's range.
 *
 * ### InsertionSortFingerprint
 *
 * A small inner loop that:
 *   - Iterates backward from the current position.
 *   - Compares adjacent elements.
 *   - Shifts the larger element right by one position.
 *
 * Typically guarded by a size threshold (n < 16 or n < 32).
 */

#ifndef RETDEC_SORT_DETECT_H
#define RETDEC_SORT_DETECT_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace ssa { class SSAFunction; }
namespace ipa { struct IpaResult; }
} // namespace retdec

namespace retdec {
namespace sort_detect {

// ─── Enumerations ─────────────────────────────────────────────────────────────

enum class SortAlgorithm : uint8_t {
    Unknown,
    Quicksort,    ///< Pure quicksort (not introsort)
    Introsort,    ///< std::sort introsort (quicksort + heapsort + insertion sort)
    Mergesort,    ///< std::stable_sort mergesort
    Heapsort,     ///< std::make_heap / std::sort_heap
    Radixsort,    ///< Non-comparison radix sort
    InsertionSort,///< Standalone insertion sort (or tail of introsort)
    SelectionSort,///< Simple O(n²) selection sort
    BubbleSort,   ///< Bubble sort
    Timsort,      ///< Python/Java-style Timsort (mergesort + insertion sort)
};

enum class CompilerVariant : uint8_t {
    Unknown,
    GCC,    ///< libstdc++ std::sort / std::stable_sort
    Clang,  ///< libc++ std::sort / std::stable_sort
    MSVC,   ///< MSVC STL std::sort
};

// ─── Element type recovered from comparator ───────────────────────────────────

struct ElementType {
    enum class Kind : uint8_t {
        Unknown,
        Int8, Int16, Int32, Int64,
        UInt8, UInt16, UInt32, UInt64,
        Float, Double,
        Pointer,   ///< sorting pointers
        Struct,    ///< user struct (comparator is a function call)
    };

    Kind        kind  = Kind::Unknown;
    std::string name;        ///< struct type name (if known)
    uint8_t     byteWidth = 0;
    bool        isSigned  = false;

    std::string toString() const;
};

// ─── Sort detection result ────────────────────────────────────────────────────

struct SortResult {
    SortAlgorithm   algorithm       = SortAlgorithm::Unknown;
    float           confidence      = 0.0f;
    ElementType     elementType;
    CompilerVariant compilerVariant = CompilerVariant::Unknown;

    /// Names of callee functions identified as sort helpers
    /// (e.g. sift-down routine, merge routine).
    std::vector<std::string> helperFunctions;

    /// True if the comparator is an inlined lambda / functor.
    bool comparatorInlined = false;

    std::string algorithmName() const noexcept;
    std::string toString() const;
};

// ─── Shared fingerprint structures ───────────────────────────────────────────

/**
 * Evidence of a partition loop within a function's IR.
 */
struct PartitionEvidence {
    bool    found          = false;
    float   confidence     = 0.0f;
    uint32_t leftVarId     = UINT32_MAX; ///< lower index SSA value
    uint32_t rightVarId    = UINT32_MAX; ///< upper index SSA value
    uint32_t cmpInstrId    = UINT32_MAX; ///< the comparison instruction
    uint32_t swapInstrId   = UINT32_MAX; ///< first store of the swap
    bool    isHoareStyle   = true;       ///< Hoare vs Lomuto partition
};

/**
 * Evidence of a sift-down (heapify) loop.
 */
struct SiftDownEvidence {
    bool    found          = false;
    float   confidence     = 0.0f;
    bool    hasLeftArith   = false; ///< 2*i+1 pattern
    bool    hasRightArith  = false; ///< 2*i+2 pattern
    bool    hasMaxSelect   = false; ///< max-child selection
    bool    hasConditionalSwap = false;
};

/**
 * Evidence of two recursive calls on halved ranges.
 */
struct RecursiveHalvingEvidence {
    bool    found          = false;
    int     selfCallCount  = 0;   ///< number of recursive self-calls
    bool    halvingConfirmed = false; ///< mid-point argument pattern confirmed
};

/**
 * Evidence of an insertion sort inner loop.
 */
struct InsertionSortEvidence {
    bool    found          = false;
    float   confidence     = 0.0f;
    bool    hasThresholdGuard = false; ///< guarded by n < K
    int     threshold      = 0;        ///< K value (typically 16 or 32)
};

// ─── Fingerprint analysers ────────────────────────────────────────────────────

/**
 * Detects Hoare-style partition loops in the IR.
 *
 * The partition loop has two converging loop variables (left index advancing
 * right, right index advancing left).  A comparison on elements at these
 * positions triggers a three-element swap when elements are out of order.
 */
class PartitionFingerprint {
public:
    PartitionEvidence analyse(const ssa::SSAFunction& fn) const;

private:
    bool hasConvergingIndices(const ssa::SSAFunction& fn) const;
    bool hasSwapPattern(const ssa::SSAFunction& fn) const;
    float scorePartition(const ssa::SSAFunction& fn) const;
};

/**
 * Detects heap sift-down loops.
 */
class SiftDownFingerprint {
public:
    SiftDownEvidence analyse(const ssa::SSAFunction& fn) const;

private:
    bool hasChildIndexArithmetic(const ssa::SSAFunction& fn) const;
    bool hasMaxChildSelection(const ssa::SSAFunction& fn) const;
};

/**
 * Detects two recursive calls on contiguous halved sub-ranges.
 */
class RecursiveHalvingFingerprint {
public:
    RecursiveHalvingEvidence analyse(const ssa::SSAFunction& fn) const;
};

/**
 * Detects insertion sort inner loops.
 */
class InsertionSortFingerprint {
public:
    InsertionSortEvidence analyse(const ssa::SSAFunction& fn) const;

private:
    bool hasBackwardShiftLoop(const ssa::SSAFunction& fn) const;
    bool hasThresholdGuard(const ssa::SSAFunction& fn, int& threshold) const;
};

// ─── Per-algorithm detectors ──────────────────────────────────────────────────

/// Base interface for all sort detectors.
class ISortDetector {
public:
    virtual ~ISortDetector() = default;
    virtual SortResult detect(const ssa::SSAFunction& fn) const = 0;
    virtual SortAlgorithm algorithm() const noexcept = 0;
};

/**
 * Introsort detector.
 *
 * Requires evidence of all three phases:
 *   - Partition loop (quicksort phase)
 *   - Depth counter that triggers heapsort delegate
 *   - Insertion sort for small sub-ranges
 *
 * Confidence = partition_score (0.5) + recursion_score (0.3) + insertion_score (0.2)
 */
class IntrosortDetector : public ISortDetector {
public:
    SortResult  detect(const ssa::SSAFunction& fn) const override;
    SortAlgorithm algorithm() const noexcept override { return SortAlgorithm::Introsort; }

private:
    bool hasDepthCounter(const ssa::SSAFunction& fn) const;
    bool hasHeapsortDelegate(const ssa::SSAFunction& fn) const;
    CompilerVariant detectVariant(const ssa::SSAFunction& fn) const;
};

/**
 * Mergesort detector.
 *
 * Requires:
 *   - Two recursive self-calls on halved ranges
 *   - A merge loop with three-way branch (left consumed / right consumed / done)
 *   - Auxiliary buffer allocation
 */
class MergesortDetector : public ISortDetector {
public:
    SortResult  detect(const ssa::SSAFunction& fn) const override;
    SortAlgorithm algorithm() const noexcept override { return SortAlgorithm::Mergesort; }

private:
    bool hasMergeLoop(const ssa::SSAFunction& fn) const;
    bool hasAuxiliaryAllocation(const ssa::SSAFunction& fn) const;
    float scoreMerge(const ssa::SSAFunction& fn) const;
};

/**
 * Heapsort detector.
 *
 * Requires:
 *   - A build-heap phase (downward loop calling sift-down)
 *   - A sort phase (extract-max + sift-down)
 *   - Sift-down signature (child index arithmetic + conditional swap)
 */
class HeapsortDetector : public ISortDetector {
public:
    SortResult  detect(const ssa::SSAFunction& fn) const override;
    SortAlgorithm algorithm() const noexcept override { return SortAlgorithm::Heapsort; }

private:
    bool hasBuildHeapPhase(const ssa::SSAFunction& fn) const;
    bool hasSortPhase(const ssa::SSAFunction& fn) const;
};

/**
 * Radix sort detector.
 *
 * Key invariant: zero element-comparison instructions.
 *
 * Requires:
 *   - Digit extraction: `(val >> k) & mask` for constant k
 *   - Histogram accumulation into small fixed array (size = radix)
 *   - Prefix-sum pass over histogram
 *   - Scatter/redistribution pass
 *
 * These four passes repeat for each digit.
 */
class RadixsortDetector : public ISortDetector {
public:
    SortResult  detect(const ssa::SSAFunction& fn) const override;
    SortAlgorithm algorithm() const noexcept override { return SortAlgorithm::Radixsort; }

private:
    bool hasDigitExtraction(const ssa::SSAFunction& fn) const;
    bool hasHistogramAccumulation(const ssa::SSAFunction& fn) const;
    bool hasPrefixSumPass(const ssa::SSAFunction& fn) const;
    bool hasScatterPass(const ssa::SSAFunction& fn) const;
    int  countComparisonInstrs(const ssa::SSAFunction& fn) const;
};

/**
 * Insertion sort detector (standalone).
 */
class InsertionSortDetector : public ISortDetector {
public:
    SortResult    detect(const ssa::SSAFunction& fn) const override;
    SortAlgorithm algorithm() const noexcept override { return SortAlgorithm::InsertionSort; }
};

// ─── Element type recovery ────────────────────────────────────────────────────

/**
 * Recovers the element type of a sort from the comparator.
 *
 * Strategy:
 *   1. Find the comparison instruction(s) in the partition / merge / sift loop.
 *   2. Trace the operands back to load instructions.
 *   3. The width and signedness of the load operand gives the element type.
 *   4. If the comparison calls a function (comparator callback), attempt to
 *      recover its parameter type from the function's calling convention.
 */
class ElementTypeRecoverer {
public:
    ElementType recover(const ssa::SSAFunction& fn,
                        const SortResult& partialResult) const;

private:
    ElementType fromCompareWidth(uint8_t bitWidth, bool isSigned) const;
    ElementType fromComparatorCall(const ssa::SSAFunction& fn,
                                   uint32_t callInstrId) const;
};

// ─── Sort detector orchestrator ───────────────────────────────────────────────

/**
 * Top-level sort detection pass.
 *
 * Runs all registered detectors on every function in the program.  Returns
 * only detections with confidence >= `minConfidence`.
 *
 * Algorithm:
 *   For each function:
 *     1. Quick pre-filter: skip functions with < 5 blocks or < 20 instructions.
 *     2. Run each detector independently.
 *     3. Keep the result with the highest confidence.
 *     4. Run ElementTypeRecoverer on the winning result.
 *     5. Infer compiler variant from helper function names / inline patterns.
 */
class SortDetector {
public:
    struct Config {
        float minConfidence  = 0.45f; ///< Minimum confidence to report
        int   minBlocks      = 3;     ///< Skip trivially small functions
        int   minInstrs      = 15;    ///< Skip trivially small functions
        bool  runRadix       = true;
        bool  runMerge       = true;
        bool  runHeap        = true;
        bool  runIntrosort   = true;
        bool  runInsertion   = true;
    };
    static Config defaultConfig() noexcept { return {}; }

    struct Stats {
        uint32_t functionsAnalysed = 0;
        uint32_t functionsSkipped  = 0;
        uint32_t detections        = 0;
        std::unordered_map<SortAlgorithm, uint32_t> byAlgorithm;
    };

    using DetectionMap = std::unordered_map<std::string, SortResult>;

    explicit SortDetector(Config cfg = defaultConfig());

    /// Analyse a single function; returns the best-matching result.
    SortResult analyseFunction(const ssa::SSAFunction& fn) const;

    /// Analyse an entire module (map of function name → SSAFunction).
    DetectionMap analyseModule(
        const std::vector<const ssa::SSAFunction*>& functions) const;

    const Stats& stats() const { return stats_; }

private:
    Config cfg_;
    mutable Stats stats_;

    std::vector<std::unique_ptr<ISortDetector>> detectors_;
    ElementTypeRecoverer typeRecoverer_;

    bool passesPreflight(const ssa::SSAFunction& fn) const;
    CompilerVariant inferVariant(const ssa::SSAFunction& fn,
                                  const SortResult& r) const;
};

} // namespace sort_detect
} // namespace retdec

#endif // RETDEC_SORT_DETECT_H
