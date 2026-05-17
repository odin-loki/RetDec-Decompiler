#include "retdec-ml.hpp"

// Stub implementation of the RetDec Qwen3-Coder ML inference framework.
// The real implementation is built from src/ml/ (Tasks 44–47).

namespace retdec::ml {

bool MLModel::loadFromPath(const char* /*path*/)
{
    _error  = "Stub: real ML framework not built. "
              "Build RetDec with -DRETDEC_ENABLE_ML=ON (Task 44).";
    _loaded = false;
    return false;
}

std::vector<int> MLModel::encode(std::string_view /*text*/) const
{
    return {};
}

std::string MLModel::decode(const std::vector<int>& /*ids*/) const
{
    return {};
}

std::string MLModel::generate(std::string_view /*prompt*/,
                              const SamplingParams& /*params*/,
                              TokenCallback /*callback*/)
{
    return {};
}

} // namespace retdec::ml
