#include "retdec/neural/batch_refiner.h"

#include <cstdlib>

namespace retdec::neural {

bool neuralBatchingEnabled()
{
	const char* e = std::getenv("RETDEC_NEURAL_BATCH");
	return e && e[0] != '\0' && e[0] != '0';
}

BatchRefiner::BatchRefiner(std::unique_ptr<Inference> backend): refiner_(std::move(backend)) {}

std::vector<RefinementResponse> BatchRefiner::refineAll(const std::vector<RefinementRequest>& requests) const
{
	std::vector<RefinementResponse> out;
	out.reserve(requests.size());

	// One llama.cpp session. Sequential refine() already uses llama_batch
	// inside generate(); multi-prompt speculative MTP has no C API at b10451.
	// When batching is on, reuse the KV prefix across requests on this session.
	for (std::size_t i = 0; i < requests.size(); ++i)
	{
		RefinementRequest req = requests[i];
		if (neuralBatchingEnabled() && i > 0) req.generation.reuseKvPrefix = true;
		out.push_back(refiner_.refine(req));
	}
	return out;
}

} // namespace retdec::neural
