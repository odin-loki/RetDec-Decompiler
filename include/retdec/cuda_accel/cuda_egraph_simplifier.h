/**
 * @file include/retdec/cuda_accel/cuda_egraph_simplifier.h
 * @brief CUDA-accelerated e-graph equality saturation — replaces OCLEGraphSimplifier.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace retdec::cuda_accel {

class CUDAContext;

enum class EOpcode : std::uint32_t {
    NOP      = 0,
    LIT      = 1,
    VAR      = 2,
    ADD      = 3,
    SUB      = 4,
    MUL      = 5,
    DIV      = 6,
    AND      = 7,
    OR       = 8,
    XOR      = 9,
    SHL      = 10,
    SHR      = 11,
    ASHR     = 12,
    CAST     = 13,
    DEREF    = 14,
    ARRAY    = 15,
    FIELD    = 16,
    BITFIELD = 17,
};

static constexpr std::uint32_t kNoClass = 0xFFFFFFFFu;

struct ENode {
    EOpcode       opcode{EOpcode::NOP};
    std::uint32_t eclass{kNoClass};
    std::uint32_t lhs{kNoClass};
    std::uint32_t rhs{kNoClass};
    std::uint32_t aux{0};
    std::uint64_t litVal{0};
};

class EGraph {
public:
    std::uint32_t addNode(EOpcode op, std::uint32_t lhs = kNoClass,
                          std::uint32_t rhs = kNoClass, std::uint32_t aux = 0);
    std::uint32_t addLiteral(std::uint64_t val);
    std::uint32_t addVar(std::uint32_t varId);

    std::size_t nodeCount()  const noexcept { return nodes_.size(); }
    std::size_t classCount() const noexcept { return classCount_; }
    void        setClassCount(std::size_t n) { classCount_ = n; }

    // SOA accessors
    std::vector<std::uint32_t>& op()      { return op_; }
    std::vector<std::uint32_t>& eclass()  { return eclass_; }
    std::vector<std::uint32_t>& lhs()     { return lhs_; }
    std::vector<std::uint32_t>& rhs()     { return rhs_; }
    std::vector<std::uint32_t>& aux()     { return aux_; }
    std::vector<std::uint64_t>& lit()     { return lit_; }
    std::vector<std::uint32_t>& ufParent(){ return ufParent_; }
    std::vector<std::uint32_t>& ufRank()  { return ufRank_; }

    void initUnionFind();
    std::uint32_t find(std::uint32_t x);

private:
    std::vector<ENode>        nodes_;
    std::vector<std::uint32_t> op_, eclass_, lhs_, rhs_, aux_;
    std::vector<std::uint64_t> lit_;
    std::vector<std::uint32_t> ufParent_, ufRank_;
    std::size_t classCount_{0};
};

struct EClassResult {
    std::uint32_t classId{kNoClass};
    std::uint32_t bestNode{kNoClass};
    std::uint32_t score{99};
    EOpcode       opcode{EOpcode::NOP};
};

class CUDAEGraphSimplifier {
public:
    explicit CUDAEGraphSimplifier(CUDAContext* ctx = nullptr);
    ~CUDAEGraphSimplifier();

    std::vector<EClassResult> simplify(EGraph& graph, std::uint32_t maxIter = 64);

    bool gpuAvailable() const noexcept { return ctx_ && gpuReady_; }

private:
    std::vector<EClassResult> simplifyCPU(EGraph& graph, std::uint32_t maxIter);

    CUDAContext* ctx_{nullptr};
    bool         gpuReady_{false};
    std::string  lastError_;
};

} // namespace retdec::cuda_accel
