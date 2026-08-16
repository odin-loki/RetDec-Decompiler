/**
 * @file src/retdec/neural_refine_stub.cpp
 * @brief No-op neural hook when RETDEC_ENABLE_NEURAL is OFF.
 */

#include "retdec/neural/decompile_hook.h"

namespace retdec::neural {

void maybeRefineDecompilerOutput(retdec::config::Config&, std::string*) {}

} // namespace retdec::neural
