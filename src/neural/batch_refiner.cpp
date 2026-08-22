#include "retdec/neural/batch_refiner.h"

namespace retdec::neural {

BatchRefiner::BatchRefiner(std::unique_ptr<Inference> backend): refiner_(std::move(backend)) {}

std::vector<RefinementResponse> BatchRefiner::refineAll(const std::vector<RefinementRequest>& requests) const
{
	std::vector<RefinementResponse> out;
	out.reserve(requests.size());

	// Sequential refine on one backend session. Do not set reuseKvPrefix:
	// KV reuse is non-reproducible (N13). RETDEC_NEURAL_BATCH is removed.
	for (const auto& req: requests)
		out.push_back(refiner_.refine(req));
	return out;
}

} // namespace retdec::neural
