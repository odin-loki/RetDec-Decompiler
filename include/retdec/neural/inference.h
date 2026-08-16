/**
 * @file include/retdec/neural/inference.h
 * @brief Neural inference interface (llama.cpp backend + mock for tests).
 */

#ifndef RETDEC_NEURAL_INFERENCE_H
#define RETDEC_NEURAL_INFERENCE_H

#include <memory>
#include <string>
#include <vector>

namespace retdec::neural {

struct GenerationConfig {
    float temperature = 0.7f;
    float topP        = 0.9f;
    float minP        = 0.0f;
    int   topK        = 20;
    int   maxTokens   = 512;
    bool  thinkingMode = false;
    /// Keep shared prompt-prefix KV between generate() calls on the same function.
    bool  reuseKvPrefix = false;
};

struct GenerationResult {
    std::string text;
    int         tokensGenerated = 0;
    bool        ok              = false;
    std::string error;
};

class Inference {
public:
    virtual ~Inference() = default;

    virtual bool loadModel(const std::string& ggufPath, int contextLen = 16384) = 0;
    virtual void unloadModel() = 0;
    virtual bool isLoaded() const = 0;

    virtual GenerationResult generate(const std::string& prompt,
                                      const GenerationConfig& config) = 0;
};

std::unique_ptr<Inference> createMockInference();
std::unique_ptr<Inference> createLlamaInference();

} // namespace retdec::neural

#endif
