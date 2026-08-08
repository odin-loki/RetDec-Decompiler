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

GateReport runVerificationGates(const std::string& originalC,
                                const std::string& refinedC) {
    GateReport report;
    if (refinedC.empty()) {
        report.structural = GateResult::FailStructural;
        return report;
    }
    if (refinedC.size() < originalC.size() / 4 && originalC.size() > 64) {
        report.structural = GateResult::FailStructural;
        return report;
    }
    // Compile and differential gates remain stubs until Phase 8.5 tooling lands.
    return report;
}

} // namespace retdec::neural
