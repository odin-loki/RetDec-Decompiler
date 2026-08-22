/**
 * @file include/retdec/neural/gates.h
 * @brief Verification gates for neural refinement output.
 */

#ifndef RETDEC_NEURAL_GATES_H
#define RETDEC_NEURAL_GATES_H

#include <string>

namespace retdec::neural {

enum class GateResult { Pass, FailCompile, FailStructural, FailDifferential };

struct GateReport {
    GateResult compile      = GateResult::Pass;
    GateResult structural   = GateResult::Pass;
    GateResult differential = GateResult::Pass;

    bool allPassed() const;
    std::string summary() const;
};

GateReport runVerificationGates(const std::string& originalC,
                                const std::string& refinedC);

/// `cc -fsyntax-only` (or `RETDEC_NEURAL_GATE_CC` / `gcc` on Windows).
/// Empty source is always false. Missing compiler is false.
bool compileSyntaxOnly(const std::string& sourceC);

/// Same as compileSyntaxOnly, and fills compiler stdout/stderr when the check runs.
bool compileSyntaxOnly(const std::string& sourceC, std::string& diagnostics);

} // namespace retdec::neural

#endif
