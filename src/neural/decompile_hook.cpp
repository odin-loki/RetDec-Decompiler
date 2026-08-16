#include "retdec/neural/decompile_hook.h"
#include "retdec/neural/inference.h"
#include "retdec/neural/refiner.h"

#include "retdec/common/function.h"
#include "retdec/common/semantic_detection.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
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

std::string jsonEscape(const std::string& s) {
    std::ostringstream oss;
    for (char c : s) {
        switch (c) {
        case '"': oss << "\\\""; break;
        case '\\': oss << "\\\\"; break;
        case '\n': oss << "\\n"; break;
        case '\r': oss << "\\r"; break;
        default: oss << c; break;
        }
    }
    return oss.str();
}

std::string serializeSemanticContext(const retdec::config::Config& config) {
    std::ostringstream oss;
    oss << "{\"functions\":[";
    bool firstFn = true;
    for (const auto& fn : config.functions) {
        if (fn.semanticDetections.empty()) continue;
        if (!firstFn) oss << ',';
        firstFn = false;
        oss << "{\"name\":\"" << jsonEscape(fn.getName()) << "\",\"detections\":[";
        bool firstDet = true;
        for (const auto& d : fn.semanticDetections) {
            if (!firstDet) oss << ',';
            firstDet = false;
            oss << "{\"kind\":\"" << jsonEscape(d.kind)
                << "\",\"label\":\"" << jsonEscape(d.label)
                << "\",\"confidence\":" << d.confidence;
            if (!d.detail.empty())
                oss << ",\"detail\":\"" << jsonEscape(d.detail) << '"';
            oss << '}';
        }
        oss << "]}";
    }
    oss << "]}";
    return oss.str();
}

} // namespace

void maybeRefineDecompilerOutput(retdec::config::Config& config,
                                 std::string* outString) {
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
    const std::string semanticJson = serializeSemanticContext(config);

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
        req.semanticContextJson = semanticJson;
        req.generation.reuseKvPrefix = (i > 0);

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
