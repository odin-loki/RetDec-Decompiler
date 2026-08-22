/**
 * @file include/retdec/neural/batch_refiner.h
 * @brief Sequential neural refinement over multiple requests.
 */

#ifndef RETDEC_NEURAL_BATCH_REFINER_H
#define RETDEC_NEURAL_BATCH_REFINER_H

#include "retdec/neural/inference.h"
#include "retdec/neural/refiner.h"

#include <memory>
#include <vector>

namespace retdec::neural {

/**
 * @brief Refine multiple requests sequentially on one backend session.
 *
 * Real llama.cpp batched decode is not implemented (N13). Each request is
 * refined independently so output does not depend on prior functions.
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
