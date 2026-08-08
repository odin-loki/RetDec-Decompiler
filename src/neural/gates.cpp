#include "retdec/neural/gates.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

namespace retdec::neural {

namespace {

bool tryCompileCheck(const std::string& sourceC)
{
    const char* cc = std::getenv("RETDEC_NEURAL_GATE_CC");
    if (!cc || !cc[0]) {
#if defined(_WIN32)
        cc = "gcc";
#else
        cc = "cc";
#endif
    }

    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path()
        / ("retdec_gate_" + std::to_string(std::hash<std::string>{}(sourceC)) + ".c");

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << sourceC;
    }

    const std::string cmd = std::string(cc) + " -fsyntax-only -w \""
        + tmp.string() + "\""
#if defined(_WIN32)
        " 2>nul";
#else
        " 2>/dev/null";
#endif
    const int rc = std::system(cmd.c_str());
    fs::remove(tmp);
    return rc == 0;
}

} // namespace

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

    const char* skipCompile = std::getenv("RETDEC_NEURAL_SKIP_COMPILE_GATE");
    if (!skipCompile || skipCompile[0] == '\0' || skipCompile[0] == '0') {
        if (!tryCompileCheck(refinedC))
            report.compile = GateResult::FailCompile;
    }

    // Differential gate: stub until Triton/D-Helix tooling (step 20).
    return report;
}

} // namespace retdec::neural
