#include "retdec/neural/batch_refiner.h"

#include <cstdlib>

namespace retdec::neural {

bool neuralBatchingEnabled()
{
    const char* e = std::getenv("RETDEC_NEURAL_BATCH");
    return e && e[0] != '\0' && e[0] != '0';
}

BatchRefiner::BatchRefiner(std::unique_ptr<Inference> backend)
    : refiner_(std::move(backend)) {}

std::vector<RefinementResponse> BatchRefiner::refineAll(
        const std::vector<RefinementRequest>& requests) const {
    std::vector<RefinementResponse> out;
    out.reserve(requests.size());

    if (neuralBatchingEnabled()) {
        // Scaffold: single backend session; future — llama.cpp batched decode.
        for (const auto& req : requests)
            out.push_back(refiner_.refine(req));
        return out;
    }

    for (const auto& req : requests)
        out.push_back(refiner_.refine(req));
    return out;
}

} // namespace retdec::neural
