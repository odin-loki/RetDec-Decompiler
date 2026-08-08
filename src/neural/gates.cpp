#include "retdec/neural/gates.h"

namespace retdec::neural {

bool GateReport::allPassed() const {
    return compile == GateResult::Pass
        && structural == GateResult::Pass
        && differential == GateResult::Pass;
}

std::string GateReport::summary() const {
    return std::string("compile=") + (compile == GateResult::Pass ? "pass" : "fail")
        + " structural=" + (structural == GateResult::Pass ? "pass" : "fail")
        + " differential=" + (differential == GateResult::Pass ? "pass" : "fail");
}

GateReport runVerificationGates(const std::string& /*originalC*/,
                                const std::string& /*refinedC*/) {
    // Stub: always pass until compile/differential tooling is wired (Phase 8.5).
    return GateReport{};
}

} // namespace retdec::neural
