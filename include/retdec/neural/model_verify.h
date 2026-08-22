/**
 * @file include/retdec/neural/model_verify.h
 * @brief GGUF model SHA-256 allowlist + header checks at load (N6/N7).
 */

#ifndef RETDEC_NEURAL_MODEL_VERIFY_H
#define RETDEC_NEURAL_MODEL_VERIFY_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace retdec::neural {

/// Text-only Qwen3.5-9B Instruct GGUF (Q4_K_M). Do not load mmproj / VL variants.
inline constexpr const char* kQwen35TextOnlyGgufHint =
	"Qwen3.5-9B-Instruct-Q4_K_M.gguf";

/// Identity fields read from the GGUF KV header (no tensor payloads).
struct GgufIdentity
{
	bool parsed = false;
	std::uint32_t version = 0;
	std::string architecture;
	std::string name;
};

/// Parse GGUF magic `GGUF`, little-endian version, and KV pairs enough to
/// read `general.architecture` and `general.name` when present.
bool parseGgufIdentity(const std::string& modelPath, GgufIdentity& out);

/// Same as parseGgufIdentity for an in-memory GGUF prefix (tests / tiny blobs).
bool parseGgufIdentityFromMemory(const void* data, std::size_t size, GgufIdentity& out);

/// True when architecture contains clip/projector or name contains mmproj.
bool ggufIdentityLooksMultimodal(const GgufIdentity& id);

/// Verify a model at load: filename + GGUF-header multimodal reject,
/// optional RETDEC_NEURAL_MODEL_SHA256 pin, and the models.json allowlist.
/// Unknown hashes refuse unless RETDEC_NEURAL_ALLOW_UNVERIFIED is set.
/// No-op path when neural is off (this is only called from loadModel).
bool verifyModelSha256(const std::string& modelPath);

} // namespace retdec::neural

#endif
