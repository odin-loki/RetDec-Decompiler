/**
 * @file src/llvmir2hll/target_hll.cpp
 * @brief Active high-level language target for llvmir2hll emission.
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 *
 * The definition lives here rather than in llvmir2hll.cpp so that anything
 * linking the emitter -- the unit tests among them -- gets it without also
 * linking the LLVM pass driver.
 */

#include "retdec/llvmir2hll/target_hll.h"

std::string TargetHLL = "c";
