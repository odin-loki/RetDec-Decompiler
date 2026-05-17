/**
 * @file include/retdec/container_detect/container_detect.h
 * @brief STL Recovery — Container Identification (Stage 26).
 *
 * ## Overview
 *
 * This module identifies compiled C++ STL containers from their binary
 * structure fingerprints in the SSA IR.  Detection uses a three-layer model:
 *
 *   **Layer 1 — Structure detection**
 *     Analyse memory layout and access patterns to identify the container's
 *     internal data structure (three-pointer vector, circular-linked list node,
 *     red-black tree node, bucket array, SSO string).
 *
 *   **Layer 2 — Context validation**
 *     Confirm that the detected structure appears in the expected calling
 *     context: consistent element size, growth factor, iteration pattern.
 *
 *   **Layer 3 — Object reconstruction**
 *     Emit idiomatic C++ access patterns (v.push_back(), s.size(), m[key])
 *     replacing raw pointer arithmetic.
 *
 * ## Containers detected
 *
 * ### std::vector<T>
 *
 * Internal layout (libstdc++/libc++/MSVC STL):
 *   ```
 *   struct vector_impl {
 *       T* begin;
 *       T* end;          // one past last element
 *       T* capacity_end; // one past allocated storage
 *   };
 *   ```
 * Signals:
 *   - Three pointer-width loads from consecutive offsets of a struct.
 *   - Growth pattern: new capacity = old × 2 (GCC) or × 1.5 (MSVC);
 *     followed by a copy/move loop and `free(old)`.
 *   - Size computation: `end - begin` in pointer arithmetic.
 *   - Element access: `begin[i]` (Load from begin + i * sizeof(T)).
 *
 * ### std::list<T>
 *
 * Internal layout (libstdc++ intrusive double-linked list):
 *   ```
 *   struct _List_node_base { _List_node_base* _next; _List_node_base* _prev; };
 *   struct _List_node<T> : _List_node_base { T _M_data; };
 *   ```
 * Signals:
 *   - A sentinel node whose `next` and `prev` both point back to itself at
 *     construction time (empty list pattern).
 *   - Node allocation via `malloc` / `new` with size = 2*pointer + sizeof(T).
 *   - Iteration: follow `_next` pointer, compare to sentinel address.
 *   - Insert/erase: update 4 pointer fields (prev/next in 2 nodes).
 *
 * ### std::map<K,V> / std::set<K>
 *
 * Red-black tree structure:
 *   ```
 *   struct _Rb_tree_node {
 *       _Rb_color _M_color;  // or packed into parent pointer low bit
 *       _Rb_tree_node* _M_parent;
 *       _Rb_tree_node* _M_left;
 *       _Rb_tree_node* _M_right;
 *       value_type _M_value_field;
 *   };
 *   ```
 * Signals:
 *   - Left/right rotation pattern (CLRS style):
 *       left_rotate:  `y = x->right; x->right = y->left; y->left = x;`
 *       right_rotate: `x = y->left;  y->left = x->right; x->right = y;`
 *   - Colour field: bit-flag on parent pointer (low bit) or explicit field.
 *   - Three-pointer node layout (parent, left, right).
 *   - Rebalancing: case analysis on sibling colour and uncle colour.
 *
 * ### std::unordered_map<K,V> / std::unordered_set<K>
 *
 * Signals:
 *   - A bucket array (pointer array of fixed size or dynamic).
 *   - Hash computation: function call or inline FNV/murmur producing `size_t`.
 *   - Modulo: `hash & (bucket_count - 1)` for power-of-two bucket counts,
 *     or `hash % bucket_count` for arbitrary counts.
 *   - Chain traversal: load `node->next`, compare to nullptr.
 *
 * ### std::string (SSO)
 *
 * Small String Optimisation (SSO) branch:
 *   ```
 *   if (len < threshold)  // typically 15 or 22 bytes
 *       use inline_buffer;
 *   else
 *       use heap_ptr;
 *   ```
 * Signals:
 *   - A compare against a small constant (15, 22, 23) selecting between two
 *     memory access patterns.
 *   - Inline path: pointer arithmetic within the string object.
 *   - Heap path: pointer load + strlen/memcpy pattern.
 *
 * ### std::shared_ptr<T>
 *
 * Two-pointer layout:
 *   ```
 *   struct shared_ptr { T* ptr; control_block* ctrl; };
 *   struct control_block { atomic<int> strong_count; atomic<int> weak_count; };
 *   ```
 * Signals:
 *   - Two-pointer load from consecutive offsets.
 *   - Atomic decrement of strong_count, zero-check, then `free(ptr)`.
 *   - Atomic increment on copy construction.
 *
 * ## Output
 *
 * Each `ContainerResult` describes:
 *   - `kind`          — detected container kind
 *   - `confidence`    — in [0.0, 1.0]
 *   - `elementType`   — recovered element type (from element size / comparator)
 *   - `keyType`       — for map/set (from comparator)
 *   - `compilerVariant` — GCC / Clang / MSVC
 *   - `accessPatterns`  — list of `AccessPattern` describing how the container
 *                         is used (iterate, insert, lookup, erase)
 *   - `emittedForm`   — idiomatic C++ accessor string for Layer 3 emission
 */

#ifndef RETDEC_CONTAINER_DETECT_H
#define RETDEC_CONTAINER_DETECT_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace ssa { class SSAFunction; }
} // namespace retdec

namespace retdec {
namespace container_detect {

// ─── Enumerations ─────────────────────────────────────────────────────────────

enum class ContainerKind : uint8_t {
    Unknown,
    Vector,
    List,
    Deque,
    Map,
    Set,
    UnorderedMap,
    UnorderedSet,
    String,
    SharedPtr,
    UniquePtr,
    WeakPtr,
    Optional,
    Variant,
    Array,       ///< std::array (fixed-size, no heap allocation)
};

enum class CompilerVariant : uint8_t {
    Unknown,
    GCC,         ///< libstdc++
    Clang,       ///< libc++
    MSVC,        ///< MSVC STL
};

// ─── Access pattern ───────────────────────────────────────────────────────────

enum class AccessKind : uint8_t {
    Unknown,
    Iterate,     ///< begin/end loop
    PushBack,    ///< vector::push_back / list::push_back
    Insert,      ///< map::insert / set::insert
    Lookup,      ///< map::find / map::operator[]
    Erase,       ///< erase(iterator)
    SizeCheck,   ///< .size() or .empty()
    Reserve,     ///< vector::reserve / unordered::reserve
    Sort,        ///< in conjunction with sort_detect
};

struct AccessPattern {
    AccessKind  kind     = AccessKind::Unknown;
    uint32_t    instrId  = UINT32_MAX;  ///< representative IR instruction
    std::string emitted;                ///< idiomatic C++ form (e.g. "v.push_back(x)")
};

// ─── Recovered type info ─────────────────────────────────────────────────────

struct RecoveredType {
    enum class Kind : uint8_t {
        Unknown,
        Int8, Int16, Int32, Int64,
        UInt8, UInt16, UInt32, UInt64,
        Float, Double,
        Pointer,
        Struct,
        String,
    };
    Kind        kind      = Kind::Unknown;
    std::string name;         ///< struct type name if known
    uint8_t     byteWidth = 0;
    bool        isSigned  = false;

    std::string toString() const;
};

// ─── Container detection result ───────────────────────────────────────────────

struct ContainerResult {
    ContainerKind    kind            = ContainerKind::Unknown;
    float            confidence      = 0.0f;
    RecoveredType    elementType;
    RecoveredType    keyType;         ///< for map/set
    CompilerVariant  compilerVariant = CompilerVariant::Unknown;

    std::vector<AccessPattern> accessPatterns;
    std::string      emittedType;    ///< e.g. "std::vector<int32_t>"
    std::string      objectName;     ///< recovered variable name

    std::string kindName() const noexcept;
    std::string toString() const;
};

// ─── Layer-1 structure evidence ───────────────────────────────────────────────

/// Evidence of a three-pointer vector layout.
struct VectorEvidence {
    bool  found           = false;
    float confidence      = 0.0f;
    bool  hasBeginEndCap  = false;  ///< three-pointer layout confirmed
    bool  hasGrowthPattern= false;  ///< reallocation + copy + free
    bool  hasSizeArith    = false;  ///< end - begin for size
    bool  hasIndexAccess  = false;  ///< begin[i] element access
    float growthFactor    = 0.0f;   ///< 2.0 (GCC) or 1.5 (MSVC)
    uint8_t elementByteWidth = 0;
};

/// Evidence of a doubly-linked list node structure.
struct ListEvidence {
    bool  found           = false;
    float confidence      = 0.0f;
    bool  hasSentinelNode = false;  ///< self-referential init pattern
    bool  hasNodeAlloc    = false;  ///< malloc/new for node + data
    bool  hasChainTraversal = false;///< follow next pointer in loop
    bool  hasFourPtrUpdate= false;  ///< 4-pointer update on insert/erase
    uint8_t nodeDataOffset= 0;      ///< offset of data within node
};

/// Evidence of a red-black tree node layout.
struct RbTreeEvidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasLeftRotation  = false;
    bool  hasRightRotation = false;
    bool  hasColourField   = false; ///< bit flag or packed pointer
    bool  hasThreePtrNode  = false; ///< parent/left/right layout
    bool  hasRebalancing   = false; ///< case analysis on uncle colour
};

/// Evidence of a hash-table bucket array.
struct HashTableEvidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasBucketArray   = false;
    bool  hasHashCompute   = false;
    bool  hasModulo        = false; ///< hash & (n-1) or hash % n
    bool  hasChainTraversal= false;
};

/// Evidence of SSO string layout.
struct SSOEvidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasSSOBranch     = false; ///< compare against threshold constant
    bool  hasInlinePath    = false;
    bool  hasHeapPath      = false;
    int   ssoThreshold     = 0;
};

/// Evidence of shared_ptr control block.
struct SharedPtrEvidence {
    bool  found            = false;
    float confidence       = 0.0f;
    bool  hasTwoPointers   = false;
    bool  hasAtomicDecrement = false;
    bool  hasZeroCheckFree = false;
    bool  hasAtomicIncrement = false;
};

// ─── Per-container detectors ──────────────────────────────────────────────────

class IContainerDetector {
public:
    virtual ~IContainerDetector() = default;
    virtual ContainerResult detect(const ssa::SSAFunction& fn) const = 0;
    virtual ContainerKind kind() const noexcept = 0;
};

/**
 * std::vector<T> detector.
 *
 * Three-layer analysis:
 *   L1: Detect three-pointer layout (begin, end, capacity_end).
 *   L2: Confirm growth pattern (realloc + copy + free) and element size.
 *   L3: Emit push_back / size / operator[] accessors.
 */
class VectorDetector : public IContainerDetector {
public:
    ContainerResult detect(const ssa::SSAFunction& fn) const override;
    ContainerKind kind() const noexcept override { return ContainerKind::Vector; }

private:
    VectorEvidence analyseStructure(const ssa::SSAFunction& fn) const;
    float          scoreEvidence(const VectorEvidence& ev) const;
    CompilerVariant detectVariant(const ssa::SSAFunction& fn,
                                   const VectorEvidence& ev) const;
    std::vector<AccessPattern> recoverAccessPatterns(
        const ssa::SSAFunction& fn) const;
    std::string emitType(const RecoveredType& elem) const;
};

/**
 * std::list<T> detector.
 */
class ListDetector : public IContainerDetector {
public:
    ContainerResult detect(const ssa::SSAFunction& fn) const override;
    ContainerKind kind() const noexcept override { return ContainerKind::List; }

private:
    ListEvidence   analyseStructure(const ssa::SSAFunction& fn) const;
    float          scoreEvidence(const ListEvidence& ev) const;
};

/**
 * std::map<K,V> / std::set<K> detector.
 */
class MapDetector : public IContainerDetector {
public:
    ContainerResult detect(const ssa::SSAFunction& fn) const override;
    ContainerKind kind() const noexcept override { return ContainerKind::Map; }

private:
    RbTreeEvidence analyseStructure(const ssa::SSAFunction& fn) const;
    float          scoreEvidence(const RbTreeEvidence& ev) const;
    bool hasLeftRotation(const ssa::SSAFunction& fn) const;
    bool hasRightRotation(const ssa::SSAFunction& fn) const;
    bool hasColourField(const ssa::SSAFunction& fn) const;
};

/**
 * std::unordered_map<K,V> / std::unordered_set<K> detector.
 */
class UnorderedMapDetector : public IContainerDetector {
public:
    ContainerResult detect(const ssa::SSAFunction& fn) const override;
    ContainerKind kind() const noexcept override { return ContainerKind::UnorderedMap; }

private:
    HashTableEvidence analyseStructure(const ssa::SSAFunction& fn) const;
    float             scoreEvidence(const HashTableEvidence& ev) const;
    bool hasBucketModulo(const ssa::SSAFunction& fn) const;
    bool hasHashFunction(const ssa::SSAFunction& fn) const;
};

/**
 * std::string (SSO) detector.
 */
class StringDetector : public IContainerDetector {
public:
    ContainerResult detect(const ssa::SSAFunction& fn) const override;
    ContainerKind kind() const noexcept override { return ContainerKind::String; }

private:
    SSOEvidence analyseStructure(const ssa::SSAFunction& fn) const;
    float       scoreEvidence(const SSOEvidence& ev) const;
    bool hasSSOThresholdCompare(const ssa::SSAFunction& fn, int& threshold) const;
};

/**
 * std::shared_ptr<T> detector.
 */
class SharedPtrDetector : public IContainerDetector {
public:
    ContainerResult detect(const ssa::SSAFunction& fn) const override;
    ContainerKind kind() const noexcept override { return ContainerKind::SharedPtr; }

private:
    SharedPtrEvidence analyseStructure(const ssa::SSAFunction& fn) const;
    float             scoreEvidence(const SharedPtrEvidence& ev) const;
    bool hasAtomicOperation(const ssa::SSAFunction& fn) const;
};

// ─── Template type recovery ───────────────────────────────────────────────────

/**
 * Recovers the element type T of a detected container from:
 *   1. The element byte-width inferred from loads/stores within the container.
 *   2. The comparator function's parameter type (for map/set).
 *   3. The hash function's input type (for unordered_map/set).
 */
class TemplateTypeRecoverer {
public:
    RecoveredType recoverElementType(const ssa::SSAFunction& fn,
                                      const ContainerResult& partial,
                                      uint8_t elementByteWidth = 0) const;
    RecoveredType recoverKeyType(const ssa::SSAFunction& fn,
                                  const ContainerResult& partial) const;

private:
    RecoveredType fromByteWidth(uint8_t w, bool isSigned = true) const;
    RecoveredType fromComparatorParam(const ssa::SSAFunction& fn) const;
    RecoveredType fromHashParam(const ssa::SSAFunction& fn) const;
};

// ─── Container detector orchestrator ─────────────────────────────────────────

/**
 * Top-level container detection pass.
 *
 * Runs all registered container detectors on each function in a module.
 * Returns the highest-confidence detection for each function (if any).
 */
class ContainerDetector {
public:
    struct Config {
        float minConfidence = 0.40f;
        int   minBlocks     = 2;
        int   minInstrs     = 8;
        bool  runVector     = true;
        bool  runList       = true;
        bool  runMap        = true;
        bool  runUnordered  = true;
        bool  runString     = true;
        bool  runSharedPtr  = true;
    };
    static Config defaultConfig() noexcept { return {}; }

    struct Stats {
        uint32_t functionsAnalysed = 0;
        uint32_t functionsSkipped  = 0;
        uint32_t detections        = 0;
        std::unordered_map<ContainerKind, uint32_t> byKind;
    };

    using DetectionMap = std::unordered_map<std::string, ContainerResult>;

    explicit ContainerDetector(Config cfg = defaultConfig());

    ContainerResult analyseFunction(const ssa::SSAFunction& fn) const;
    DetectionMap    analyseModule(
        const std::vector<const ssa::SSAFunction*>& functions) const;

    const Stats& stats() const { return stats_; }

private:
    Config cfg_;
    mutable Stats stats_;
    std::vector<std::unique_ptr<IContainerDetector>> detectors_;
    TemplateTypeRecoverer typeRecoverer_;

    bool passesPreflight(const ssa::SSAFunction& fn) const;
};

} // namespace container_detect
} // namespace retdec

#endif // RETDEC_CONTAINER_DETECT_H
