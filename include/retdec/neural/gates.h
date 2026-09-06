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

    /// False when the structural gate had to fall back to counting keywords in
    /// the text because no C parser was available. The fallback is strictly
    /// weaker than the AST comparison, so a caller that cares about the
    /// strength of the check -- not just its verdict -- should look at this.
    bool structuralUsedParser = true;

    bool allPassed() const;
    std::string summary() const;
};

/// True when this build can parse C (tree-sitter is linked in), which is what
/// decides whether the structural gate compares parse trees or falls back to
/// counting keywords in the raw text.
bool hasCParserSupport();

GateReport runVerificationGates(const std::string& originalC,
                                const std::string& refinedC);

/// `cc -fsyntax-only` (or `RETDEC_NEURAL_GATE_CC` / `gcc` on Windows).
/// Empty source is always false. Missing compiler is false.
bool compileSyntaxOnly(const std::string& sourceC);

/// Same as compileSyntaxOnly, and fills compiler stdout/stderr when the check runs.
bool compileSyntaxOnly(const std::string& sourceC, std::string& diagnostics);

} // namespace retdec::neural

#endif
