/**
 * @file include/retdec/opencl/ocl_egraph_simplifier.h
 * @brief Host-side API for the parallel e-graph equality-saturation pass.
 *
 * An EGraph stores expressions as flat arrays of ENode objects.  Each
 * ENode belongs to an EClass (equivalence class).  The saturation kernel
 * fires rewrite rules, merging e-classes, until a fixpoint is reached.
 * The extraction kernel then selects the minimum C-distance representative
 * for each e-class.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace retdec {
namespace opencl {

class OCLContext;

// ─── Opcodes ─────────────────────────────────────────────────────────────────

enum class EOpcode : uint32_t {
    NOP      = 0,
    LIT      = 1,   ///< integer literal (value stored in litVal)
    VAR      = 2,   ///< variable reference (aux = var id)
    ADD      = 3,
    SUB      = 4,
    MUL      = 5,
    DIV      = 6,
    AND      = 7,
    OR       = 8,
    XOR      = 9,
    SHL      = 10,
    SHR      = 11,  ///< logical shift right
    ASHR     = 12,  ///< arithmetic shift right
    CAST     = 13,  ///< width cast (aux = target width in bytes)
    DEREF    = 14,  ///< pointer dereference
    ARRAY    = 15,  ///< array index: lhs[rhs]
    FIELD    = 16,  ///< struct field: lhs->aux
    BITFIELD = 17,  ///< bit-field: (lhs >> aux) & mask
};

constexpr uint32_t kNoClass = 0xFFFFFFFFu;

// ─── ENode ────────────────────────────────────────────────────────────────────

struct ENode {
    EOpcode  opcode  = EOpcode::NOP;
    uint32_t eclass  = kNoClass; ///< owning e-class
    uint32_t lhs     = kNoClass; ///< left operand e-class
    uint32_t rhs     = kNoClass; ///< right operand e-class
    uint32_t aux     = 0;        ///< shift / field offset / cast width / bit-field shift
    uint64_t litVal  = 0;        ///< literal value (EOpcode::LIT only)
};

// ─── EGraph ──────────────────────────────────────────────────────────────────

/**
 * Flat e-graph: parallel vectors for SOA layout (matches kernel arrays).
 *
 * To build an e-graph:
 *  1. addNode() to insert each expression node, receiving a node index.
 *  2. addLiteral() / addVar() for leaf nodes.
 *  3. Pass the EGraph to OCLEGraphSimplifier::simplify().
 *  4. Read back results via bestNode() / bestScore().
 */
class EGraph {
public:
    EGraph() = default;

    uint32_t addNode(EOpcode op, uint32_t cls, uint32_t l, uint32_t r,
                     uint32_t a = 0, uint64_t literal = 0)
    {
        auto idx = static_cast<uint32_t>(op_.size());
        op_.push_back(static_cast<uint32_t>(op));
        eclass_.push_back(cls);
        lhs_.push_back(l);
        rhs_.push_back(r);
        aux_.push_back(a);
        lit_.push_back(literal);
        return idx;
    }

    uint32_t addLiteral(uint32_t cls, uint64_t value)
    {
        return addNode(EOpcode::LIT, cls, kNoClass, kNoClass, 0, value);
    }

    uint32_t addVar(uint32_t cls, uint32_t varId)
    {
        return addNode(EOpcode::VAR, cls, kNoClass, kNoClass, varId, 0);
    }

    uint32_t nodeCount()  const { return static_cast<uint32_t>(op_.size()); }
    uint32_t classCount() const { return nClasses_; }

    void setClassCount(uint32_t n) { nClasses_ = n; }

    // SOA accessors
    std::vector<uint32_t>&       op()     { return op_; }
    std::vector<uint32_t>&       eclass() { return eclass_; }
    std::vector<uint32_t>&       lhs()    { return lhs_; }
    std::vector<uint32_t>&       rhs()    { return rhs_; }
    std::vector<uint32_t>&       aux()    { return aux_; }
    std::vector<uint64_t>&       lit()    { return lit_; }
    std::vector<uint32_t>&       ufParent() { return ufParent_; }
    std::vector<uint32_t>&       ufRank()   { return ufRank_; }

    const std::vector<uint32_t>& op()     const { return op_; }
    const std::vector<uint32_t>& eclass() const { return eclass_; }
    const std::vector<uint32_t>& lhs()    const { return lhs_; }
    const std::vector<uint32_t>& rhs()    const { return rhs_; }
    const std::vector<uint32_t>& aux()    const { return aux_; }
    const std::vector<uint64_t>& lit()    const { return lit_; }

    void initUnionFind()
    {
        ufParent_.resize(nClasses_);
        ufRank_.resize(nClasses_, 0);
        for (uint32_t i = 0; i < nClasses_; ++i) ufParent_[i] = i;
    }

    uint32_t find(uint32_t x) const
    {
        while (ufParent_[x] != x) x = ufParent_[x];
        return x;
    }

private:
    uint32_t             nClasses_ = 0;
    std::vector<uint32_t> op_;
    std::vector<uint32_t> eclass_;
    std::vector<uint32_t> lhs_;
    std::vector<uint32_t> rhs_;
    std::vector<uint32_t> aux_;
    std::vector<uint64_t> lit_;
    std::vector<uint32_t> ufParent_;
    std::vector<uint32_t> ufRank_;
};

// ─── Extraction result ───────────────────────────────────────────────────────

struct EClassResult {
    uint32_t classId   = kNoClass;
    uint32_t bestNode  = kNoClass; ///< index of selected e-node
    uint32_t score     = 99u;      ///< C-distance of selected node
    EOpcode  opcode    = EOpcode::NOP;
};

// ─── OCLEGraphSimplifier ─────────────────────────────────────────────────────

/**
 * Runs equality saturation over an EGraph, then extracts best expressions.
 *
 * If ctx is nullptr (or no GPU is available) the CPU fallback is used.
 */
class OCLEGraphSimplifier {
public:
    explicit OCLEGraphSimplifier(OCLContext *ctx);
    ~OCLEGraphSimplifier();

    OCLEGraphSimplifier(const OCLEGraphSimplifier &) = delete;
    OCLEGraphSimplifier &operator=(const OCLEGraphSimplifier &) = delete;

    /**
     * Run saturation + extraction on *graph*.
     * Modifies the union-find (ufParent, ufRank) arrays in *graph* in place.
     *
     * @param graph     The e-graph to simplify.
     * @param maxIter   Maximum saturation iterations (default: 64).
     * @return Vector of EClassResult, one per e-class (ordered by classId).
     */
    std::vector<EClassResult> simplify(EGraph &graph, uint32_t maxIter = 64);

    bool gpuAvailable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace opencl
} // namespace retdec
