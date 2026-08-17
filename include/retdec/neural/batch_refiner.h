/**
 * @file include/retdec/neural/batch_refiner.h
 * @brief Batched neural refinement (step 27 / 11.4 scaffold).
 */

#ifndef RETDEC_NEURAL_BATCH_REFINER_H
#define RETDEC_NEURAL_BATCH_REFINER_H

#include "retdec/neural/inference.h"
#include "retdec/neural/refiner.h"

#include <memory>
#include <vector>

namespace retdec::neural {

/// True when RETDEC_NEURAL_BATCH is set (non-zero).
bool neuralBatchingEnabled();

/**
 * @brief Refine multiple requests; batches prompts when RETDEC_NEURAL_BATCH=1.
 *
 * Processes sequentially on one backend session. With RETDEC_NEURAL_BATCH=1,
 * later requests reuse the KV prefix (llama.cpp llama_batch decode).
 */
class BatchRefiner {
public:
    explicit BatchRefiner(std::unique_ptr<Inference> backend);

    std::vector<RefinementResponse> refineAll(
        const std::vector<RefinementRequest>& requests) const;

private:
    Refiner refiner_;
};

} // namespace retdec::neural

#endif
