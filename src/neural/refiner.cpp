#include "retdec/neural/refiner.h"
#include "retdec/neural/gates.h"

namespace retdec::neural {

std::string buildRefinementPrompt(const RefinementRequest& request);

Refiner::Refiner(std::unique_ptr<Inference> backend)
    : inference_(std::move(backend)) {}

RefinementResponse Refiner::refine(const RefinementRequest& request) const {
    RefinementResponse response;
    if (!inference_ || !inference_->isLoaded()) {
        response.refinedSource = request.functionSource;
        response.accepted = false;
        response.manifestJson = R"({"accepted":false,"reason":"no backend"})";
        return response;
    }

    std::string prompt = buildRefinementPrompt(request);

    const auto gen = inference_->generate(prompt, request.generation);
    if (!gen.ok) {
        response.refinedSource = request.functionSource;
        response.accepted = false;
        response.manifestJson = R"({"accepted":false,"reason":"generation failed"})";
        return response;
    }

    const auto gates = runVerificationGates(request.functionSource, gen.text);
    if (!gates.allPassed()) {
        response.refinedSource = request.functionSource;
        response.accepted = false;
        response.manifestJson = R"({"accepted":false,"reason":"gates failed"})";
        return response;
    }

    response.refinedSource = gen.text;
    response.accepted = true;
    response.manifestJson = R"({"accepted":true,"tier":)" + std::to_string(static_cast<int>(request.tier)) + "}";
    return response;
}

} // namespace retdec::neural
