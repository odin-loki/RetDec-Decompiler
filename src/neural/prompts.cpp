#include "retdec/neural/refiner.h"

#include <sstream>
#include <string>

namespace retdec::neural {

namespace {

std::string tierPrompt(RefinementTier tier) {
    switch (tier) {
    case RefinementTier::Naming:
        return "Suggest improved variable and function names only. Do not change logic.\n";
    case RefinementTier::Comments:
        return "Add concise comments and a one-line summary. Do not change code.\n";
    case RefinementTier::StructFields:
        return "Rename struct fields to match recovered layout. Do not change offsets.\n";
    case RefinementTier::IdiomRecovery:
        return "Replace obvious low-level loops with standard library idioms when safe.\n";
    case RefinementTier::FullRewrite:
        return "Rewrite for clarity while preserving semantics.\n";
    }
    return {};
}

} // namespace

std::string buildRefinementPrompt(const RefinementRequest& request) {
    std::ostringstream oss;
    oss << tierPrompt(request.tier);
    if (!request.semanticContextJson.empty()) {
        oss << "Semantic context (JSON):\n" << request.semanticContextJson << "\n\n";
    }
    oss << "Function source:\n" << request.functionSource << "\n";
    return oss.str();
}

} // namespace retdec::neural
