#include "retdec/neural/decompile_hook.h"
#include "retdec/neural/inference.h"
#include "retdec/neural/refiner.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace retdec::neural {

namespace {

bool envEnabled(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] != '\0' && v[0] != '0';
}

std::string modelPathFromEnv() {
    const char* p = std::getenv("RETDEC_NEURAL_MODEL");
    return p ? std::string(p) : std::string();
}

int tierMaxFromEnv() {
    const char* t = std::getenv("RETDEC_NEURAL_TIER_MAX");
    if (!t || !t[0]) return 3;
    const int v = std::atoi(t);
    return v < 1 ? 1 : (v > 5 ? 5 : v);
}

void writeSidecar(const std::string& basePath,
                  const std::string& refined,
                  const std::string& manifest) {
    if (basePath.empty()) return;
    const std::string refinedPath = basePath + ".refined.c";
    const std::string manifestPath = basePath + ".refinement-manifest.json";
    std::ofstream(refinedPath) << refined;
    std::ofstream(manifestPath) << manifest;
}

} // namespace

void maybeRefineDecompilerOutput(retdec::config::Config& config,
                                 std::string* outString) {
    (void)config;
    if (!outString || outString->empty()) return;
    if (!envEnabled("RETDEC_NEURAL_REFINE")) return;

#ifndef RETDEC_NEURAL_OFFLINE_ONLY
    if (envEnabled("RETDEC_NO_NETWORK")) {
        if (!envEnabled("RETDEC_NEURAL_ALLOW_NETWORK")) return;
    }
#endif

    const std::string model = modelPathFromEnv();
    if (model.empty()) return;

    auto backend = createLlamaInference();
    if (!backend) backend = createMockInference();
    if (!backend->loadModel(model)) return;

    Refiner refiner(std::move(backend));

    static const RefinementTier kTiers[] = {
        RefinementTier::Naming,
        RefinementTier::Comments,
        RefinementTier::StructFields,
        RefinementTier::IdiomRecovery,
        RefinementTier::FullRewrite,
    };

    const int tierMax = tierMaxFromEnv();
    std::string current = *outString;
    std::string lastManifest = R"({"accepted":false,"reason":"no tier ran"})";
    bool anyAccepted = false;

    for (int i = 0; i < tierMax && i < 5; ++i) {
        RefinementRequest req;
        req.functionSource = current;
        req.tier = kTiers[i];
        req.semanticContextJson = "{}";

        const auto resp = refiner.refine(req);
        lastManifest = resp.manifestJson;
        if (resp.accepted) {
            current = resp.refinedSource;
            anyAccepted = true;
        }
    }

    if (anyAccepted) {
        writeSidecar(config.parameters.getOutputFile(), current, lastManifest);
    }
}

} // namespace retdec::neural
