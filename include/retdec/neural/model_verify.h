/**
 * @file include/retdec/neural/model_verify.h
 * @brief GGUF model SHA-256 verification at load (step 8.8).
 */

#ifndef RETDEC_NEURAL_MODEL_VERIFY_H
#define RETDEC_NEURAL_MODEL_VERIFY_H

#include <string>

namespace retdec::neural {

/// Verify file SHA-256 against RETDEC_NEURAL_MODEL_SHA256 when set.
bool verifyModelSha256(const std::string& modelPath);

} // namespace retdec::neural

#endif
