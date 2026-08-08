/**
 * @file src/neural/llama_inference.cpp
 * @brief llama.cpp-backed Inference implementation (optional build).
 */

#include "retdec/neural/inference.h"

#include <mutex>
#include <string>

#if defined(RETDEC_HAS_LLAMACPP)
#include "llama.h"
#include <vector>
#endif

namespace retdec::neural {

#if defined(RETDEC_HAS_LLAMACPP)

namespace {
std::once_flag g_backendOnce;
llama_model*   g_model   = nullptr;
llama_context* g_context = nullptr;

void silenceLlamaLogs(ggml_log_level, const char*, void*) {}

void initBackend() {
    llama_backend_init();
    llama_log_set(silenceLlamaLogs, nullptr);
}
} // namespace

class LlamaInference : public Inference {
public:
    bool loadModel(const std::string& ggufPath, int contextLen) override {
        std::call_once(g_backendOnce, initBackend);

        unloadModel();

        llama_model_params mparams = llama_model_default_params();
        g_model = llama_load_model_from_file(ggufPath.c_str(), mparams);
        if (!g_model) return false;

        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = static_cast<uint32_t>(contextLen);
        g_context = llama_new_context_with_model(g_model, cparams);
        return g_context != nullptr;
    }

    void unloadModel() override {
        if (g_context) {
            llama_free(g_context);
            g_context = nullptr;
        }
        if (g_model) {
            llama_free_model(g_model);
            g_model = nullptr;
        }
    }

    bool isLoaded() const override { return g_context != nullptr; }

    GenerationResult generate(const std::string& prompt,
                              const GenerationConfig& config) override {
        GenerationResult result;
        if (!g_context) {
            result.error = "llama: model not loaded";
            return result;
        }

        llama_kv_cache_clear(g_context);

        std::vector<llama_token> tokens(prompt.size() + 8);
        const int n = llama_tokenize(
            g_model, prompt.c_str(), static_cast<int>(prompt.size()),
            tokens.data(), static_cast<int>(tokens.size()), true, false);
        if (n < 0) {
            result.error = "llama: tokenize failed";
            return result;
        }
        tokens.resize(static_cast<std::size_t>(n));

        llama_batch batch = llama_batch_get_one(tokens.data(), n);
        if (llama_decode(g_context, batch) != 0) {
            result.error = "llama: decode failed";
            return result;
        }

        std::string out;
        for (int i = 0; i < config.maxTokens; ++i) {
            const llama_token tok = llama_sampler_sample(nullptr, g_context, -1);
            if (llama_token_is_eog(g_model, tok)) break;
            char piece[64];
            const int len = llama_token_to_piece(g_model, tok, piece, sizeof(piece), 0, true);
            if (len > 0) out.append(piece, static_cast<std::size_t>(len));
            llama_batch next = llama_batch_get_one(&tok, 1);
            if (llama_decode(g_context, next) != 0) break;
        }

        result.text = std::move(out);
        result.tokensGenerated = static_cast<int>(result.text.size());
        result.ok = true;
        return result;
    }
};

std::unique_ptr<Inference> createLlamaInference() {
    return std::make_unique<LlamaInference>();
}

#else

std::unique_ptr<Inference> createLlamaInference() {
    return nullptr;
}

#endif

} // namespace retdec::neural
