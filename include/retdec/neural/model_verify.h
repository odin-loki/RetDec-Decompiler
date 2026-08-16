/**
 * @file include/retdec/neural/model_verify.h
 * @brief GGUF model SHA-256 verification at load (step 8.8).
 */

#ifndef RETDEC_NEURAL_MODEL_VERIFY_H
#define RETDEC_NEURAL_MODEL_VERIFY_H

#include <string>

namespace retdec::neural {

/// Text-only Qwen3.5-9B Instruct GGUF (Q4_K_M). Do not load mmproj / VL variants.
inline constexpr const char* kQwen35TextOnlyGgufHint =
    "Qwen3.5-9B-Instruct-Q4_K_M.gguf";

/// Verify file SHA-256 against RETDEC_NEURAL_MODEL_SHA256 when set.
/// Rejects multimodal mmproj / VL filenames regardless of SHA.
bool verifyModelSha256(const std::string& modelPath);

} // namespace retdec::neural

#endif
