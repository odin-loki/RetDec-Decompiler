/**
 * @file include/retdec/neural/refiner.h
 * @brief Orchestrates tiered neural refinement passes.
 */

#ifndef RETDEC_NEURAL_REFINER_H
#define RETDEC_NEURAL_REFINER_H

#include "retdec/neural/inference.h"

#include <memory>
#include <string>

namespace retdec::neural {

enum class RefinementTier {
    Naming = 1,
    Comments = 2,
    StructFields = 3,
    IdiomRecovery = 4,
    FullRewrite = 5
};

struct RefinementRequest {
    std::string functionSource;
    std::string semanticContextJson;
    RefinementTier tier = RefinementTier::Naming;
    GenerationConfig generation;
    /// Non-empty: previous `cc -fsyntax-only` output for a compile-retry pass.
    std::string compilerDiagnostics;
};

struct RefinementResponse {
    std::string refinedSource;
    bool        accepted = false;
    std::string manifestJson;
};

std::string buildRefinementPrompt(const RefinementRequest& request);

class Refiner {
public:
    explicit Refiner(std::unique_ptr<Inference> backend);
    RefinementResponse refine(const RefinementRequest& request) const;

private:
    std::unique_ptr<Inference> inference_;
};

} // namespace retdec::neural

#endif
