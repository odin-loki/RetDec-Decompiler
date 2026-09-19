/**
 * @file include/retdec/neural/decompile_hook.h
 * @brief Optional neural refinement hook after C emission.
 */

#ifndef RETDEC_NEURAL_DECOMPILE_HOOK_H
#define RETDEC_NEURAL_DECOMPILE_HOOK_H

#include "retdec/config/config.h"

#include <string>

namespace retdec::neural {

/// Run neural refinement when a GGUF is available (default on).
/// Set RETDEC_NEURAL_REFINE=0 to disable. RETDEC_NEURAL_MODEL overrides
/// the shipped share/retdec/models/Qwen3.5-9B-Q4_K_M.gguf path.
void maybeRefineDecompilerOutput(retdec::config::Config& config,
                                 std::string* outString);

} // namespace retdec::neural

#endif
