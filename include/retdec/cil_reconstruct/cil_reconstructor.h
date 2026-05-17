/**
 * @file include/retdec/cil_reconstruct/cil_reconstructor.h
 * @brief Top-level CIL reconstruction orchestrator.
 *
 * ## Pipeline
 *
 *   1. CilStackSimulator  — typed stack simulation
 *   2. CilVarRecovery     — variable naming, coalescing, stmt conversion
 *   3. CilStructuring     — control-flow structuring (if/while/do-while/for)
 *   4. CilPatternDetector — high-level pattern detection (async, LINQ, …)
 *
 * ## CilStructuring (internal)
 *
 * After variable recovery, we have a flat list of `CilStmt` blocks
 * connected by `goto`.  The structuring pass converts this to structured
 * control flow:
 *
 *   - **If-else**: back-edge free 2-successor block with dominator
 *   - **While**: loop header has back-edge, single continue/break exit
 *   - **Do-while**: loop header has fall-through, back-edge exit
 *   - **For**: init-assign + condition + step = for loop pattern
 *   - **Switch-statement**: multi-successor block with integer expression
 *   - **Try/catch/finally**: EH handlers wired in BcCFG
 *
 * The structuring pass is based on the Relooper algorithm (reduced form)
 * adapted for the CLR's structured exception handling model.
 *
 * ## Output
 *
 * `ReconstructResult` holds the fully structured method, ready for the
 * C# emitter.  It mirrors the JVM's `ReconstructResult` to allow the
 * same emitter infrastructure.
 */

#ifndef RETDEC_CIL_RECONSTRUCT_CIL_RECONSTRUCTOR_H
#define RETDEC_CIL_RECONSTRUCT_CIL_RECONSTRUCTOR_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/cil_reconstruct/cil_patterns.h"
#include "retdec/cil_reconstruct/cil_stack_sim.h"
#include "retdec/cil_reconstruct/cil_var_recovery.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace cil_reconstruct {

using namespace bc_module;

// ─── CilReconstructOptions ────────────────────────────────────────────────────

struct CilReconstructOptions {
    CilStackSimulator::Options  stackOpts;
    CilVarRecovery::Options     varOpts;
    CilPatternDetector::Options patternOpts;

    bool structureLoops    = true;   ///< Reconstruct while/for/do-while
    bool structureSwitch   = true;   ///< Reconstruct switch statements
    bool structureExcept   = true;   ///< Reconstruct try/catch/finally
    bool preserveGotos     = false;  ///< If true, leave unstructured goto
    int  maxStructureDepth = 64;     ///< Recursion limit for structuring
};

// ─── CilReconstructResult ─────────────────────────────────────────────────────

struct CilReconstructResult {
    CilRecoveredMethod method;

    bool   success    = false;
    bool   incomplete = false;   ///< Partial result (some blocks fell back to goto)
    std::string error;

    // Statistics
    uint32_t blockCount      = 0;
    uint32_t stmtCount       = 0;
    uint32_t gotoCount       = 0;  ///< Number of residual unstructured gotos
    bool     hasAsync        = false;
    bool     hasIterator     = false;
    bool     hasLinq         = false;
    bool     hasUnsafe       = false;
    bool     hasPatternMatch = false;
};

// ─── CilReconstructor ─────────────────────────────────────────────────────────

/**
 * @brief Orchestrates the full CIL → CilRecoveredMethod pipeline.
 */
class CilReconstructor {
public:
    explicit CilReconstructor(CilReconstructOptions opts = CilReconstructOptions{});

    /**
     * @brief Reconstruct a single method.
     *
     * @param method  The BcMethod (with populated BcCFG from CilLifter).
     * @param module  The owning BcModule (for cross-class lookups).
     */
    CilReconstructResult reconstruct(const BcMethod& method,
                                      const BcModule& module) const;

    /**
     * @brief Reconstruct all methods in a module.
     *
     * @return Map from `fqClassName::methodName::descriptor` to result.
     */
    std::unordered_map<std::string, CilReconstructResult>
    reconstructAll(const BcModule& module) const;

private:
    CilReconstructOptions opts_;

    // ── Structuring ───────────────────────────────────────────────────────

    std::vector<CilStmt> structureBlocks(
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg) const;

    std::vector<CilStmt> structureRegion(
        const std::vector<uint32_t>& blockIds,
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg,
        int depth) const;

    // Detect and structure EH regions
    std::vector<CilStmt> structureEH(
        const std::vector<uint32_t>& blockIds,
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg) const;

    // Detect loops in a region
    bool detectLoop(const std::vector<uint32_t>& blockIds,
                     const BcCFG& cfg,
                     uint32_t& headerBlock,
                     std::vector<uint32_t>& loopBody,
                     std::vector<uint32_t>& exitBlocks) const;

    // Build if-else from a 2-successor block
    std::vector<CilStmt> buildIfElse(
        uint32_t condBlock,
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg,
        const std::vector<uint32_t>& outerBlocks) const;

    // Build while loop
    std::vector<CilStmt> buildWhile(
        uint32_t headerBlock,
        const std::vector<uint32_t>& loopBlockIds,
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg) const;

    // Build switch statement
    std::vector<CilStmt> buildSwitch(
        uint32_t switchBlock,
        std::vector<CilRecoveredBlock>& blocks,
        const BcCFG& cfg) const;

    // ── Helpers ───────────────────────────────────────────────────────────

    static std::string methodKey(const BcClass& cls, const BcMethod& m);
    static bool        methodNeedsReconstruction(const BcMethod& m);
};

} // namespace cil_reconstruct
} // namespace retdec

#endif // RETDEC_CIL_RECONSTRUCT_CIL_RECONSTRUCTOR_H
