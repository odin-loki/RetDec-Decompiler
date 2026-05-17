/**
 * @file include/retdec/cuda_accel/cuda_steensgaard.h
 * @brief CUDA-accelerated Steensgaard points-to analysis — replaces OCLSteensgaard.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace retdec::cuda_accel {

class CUDAContext;

enum class ConstraintKind : std::uint8_t {
    Copy   = 0,
    AddrOf = 1,
    Store  = 2,
    Load   = 3,
};

struct PtsConstraint {
    ConstraintKind kind;
    std::uint32_t  varA;
    std::uint32_t  varB;
};

struct AliasResult {
    std::vector<std::uint32_t> aliasClass;  ///< aliasClass[v] == aliasClass[u] → may-alias
    std::vector<std::uint32_t> pointsTo;    ///< pointsTo[v] == kNoTarget → not a pointer

    static constexpr std::uint32_t kNoTarget = 0xFFFFFFFFu;

    bool mayAlias(std::uint32_t a, std::uint32_t b) const {
        if (a >= aliasClass.size() || b >= aliasClass.size()) return true;
        return aliasClass[a] == aliasClass[b];
    }
    bool hasPointsTo(std::uint32_t v) const {
        return v < pointsTo.size() && pointsTo[v] != kNoTarget;
    }
};

class CUDASteensgaard {
public:
    static constexpr std::uint32_t kMaxIterations = 64;

    explicit CUDASteensgaard(CUDAContext* ctx = nullptr);
    ~CUDASteensgaard();

    AliasResult analyze(std::uint32_t numVars,
                        const std::vector<PtsConstraint>& constraints);

    bool               usesGPU()        const noexcept { return ctx_ && gpuReady_; }
    const std::string& lastError()      const noexcept { return lastError_; }
    std::uint32_t      lastIterations() const noexcept { return lastIter_; }

private:
    AliasResult analyzeCPU(std::uint32_t numVars,
                            const std::vector<PtsConstraint>& constraints);

    CUDAContext*  ctx_{nullptr};
    bool          gpuReady_{false};
    std::string   lastError_;
    std::uint32_t lastIter_{0};
};

} // namespace retdec::cuda_accel
