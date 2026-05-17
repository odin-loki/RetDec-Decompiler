/**
 * @file include/retdec/opencl/ocl_steensgaard.h
 * @brief Parallel Steensgaard points-to analysis (heap alias layer).
 *
 * Handles the heap alias tier; CPU handles stack/global exactly.
 * Use AliasAnalysisMgr to combine both.
 */

#ifndef RETDEC_OPENCL_OCL_STEENSGAARD_H
#define RETDEC_OPENCL_OCL_STEENSGAARD_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace retdec {
namespace opencl {

class OCLContext;

// ─── Constraint encoding ─────────────────────────────────────────────────────

enum class ConstraintKind : std::uint8_t {
    Copy   = 0,  ///< a := b        (copy)
    AddrOf = 1,  ///< a := &b       (address-of)
    Store  = 2,  ///< *a := b       (store through pointer)
    Load   = 3,  ///< a := *b       (load through pointer)
};

struct PtsConstraint {
    ConstraintKind kind;
    std::uint32_t  varA;  ///< LHS variable index
    std::uint32_t  varB;  ///< RHS variable index
};

// ─── Analysis result ──────────────────────────────────────────────────────────

struct AliasResult {
    /// alias_class[i] = ECR root for variable i.
    /// Two variables a and b may alias iff alias_class[a] == alias_class[b].
    std::vector<std::uint32_t> aliasClass;

    /// points_to[i] = ECR root that variable i points to, or kNoTarget.
    std::vector<std::uint32_t> pointsTo;

    static constexpr std::uint32_t kNoTarget = 0xFFFFFFFFu;

    /// Query alias relationship.
    bool mayAlias(std::uint32_t a, std::uint32_t b) const noexcept
    {
        if (a >= aliasClass.size() || b >= aliasClass.size()) return true;
        return aliasClass[a] == aliasClass[b];
    }

    bool hasPointsTo(std::uint32_t var) const noexcept
    {
        if (var >= pointsTo.size()) return false;
        return pointsTo[var] != kNoTarget;
    }
};

// ─── OCLSteensgaard ──────────────────────────────────────────────────────────

class OCLSteensgaard {
public:
    static constexpr std::uint32_t kMaxIterations = 64;

    explicit OCLSteensgaard(OCLContext* ctx = nullptr);
    ~OCLSteensgaard();

    OCLSteensgaard(OCLSteensgaard&&) noexcept;
    OCLSteensgaard& operator=(OCLSteensgaard&&) noexcept;

    OCLSteensgaard(const OCLSteensgaard&)            = delete;
    OCLSteensgaard& operator=(const OCLSteensgaard&) = delete;

    /// Run Steensgaard analysis.
    ///
    /// @param numVars      Total number of abstract locations / variables.
    /// @param constraints  Points-to constraints.  Variable indices in [0, numVars).
    /// @returns            AliasResult.  On failure, returns an all-may-alias result.
    AliasResult analyze(std::uint32_t                       numVars,
                        const std::vector<PtsConstraint>&   constraints);

    bool usesGPU() const noexcept;
    const std::string& lastError() const noexcept;
    std::uint32_t lastIterations() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace opencl
} // namespace retdec

#endif // RETDEC_OPENCL_OCL_STEENSGAARD_H
