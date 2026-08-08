/**
 * @file include/retdec/neural/decompile_hook.h
 * @brief Optional neural refinement hook after C emission.
 */

#ifndef RETDEC_NEURAL_DECOMPILE_HOOK_H
#define RETDEC_NEURAL_DECOMPILE_HOOK_H

#include "retdec/config/config.h"

#include <string>

namespace retdec::neural {

/// Run neural refinement tiers when RETDEC_NEURAL_REFINE is set and model path exists.
void maybeRefineDecompilerOutput(retdec::config::Config& config,
                                 std::string* outString);

} // namespace retdec::neural

#endif
