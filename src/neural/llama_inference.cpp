/**
 * @file src/neural/llama_inference.cpp
 * @brief llama.cpp-backed Inference implementation (optional build).
 *
 * Targets llama.cpp b9180+ (pinned in cmake/deps.cmake). Read llama.h at that
 * pin before changing symbols.
 */

#include "retdec/neural/inference.h"
#include "retdec/neural/model_verify.h"

#include <cstdlib>
#include <mutex>
#include <string>

#if defined(RETDEC_HAS_LLAMACPP)
#include "llama.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>
#endif

namespace retdec::neural {

#if defined(RETDEC_HAS_LLAMACPP)

namespace {
std::once_flag g_backendOnce;
llama_model* g_model = nullptr;
llama_context* g_context = nullptr;
std::vector<llama_token> g_lastPromptTokens;

void llamaLog(enum ggml_log_level level, const char* text, void*)
{
	if (level >= GGML_LOG_LEVEL_WARN && text) std::fputs(text, stderr);
}

void initBackend()
{
	llama_backend_init();
	llama_log_set(llamaLog, nullptr);
}

int envInt(const char* name, int fallback)
{
	const char* v = std::getenv(name);
	if (!v || !v[0]) return fallback;
	return std::atoi(v);
}

bool envFlag(const char* name)
{
	const char* v = std::getenv(name);
	return v && v[0] != '\0' && v[0] != '0';
}

llama_sampler* buildSampler(const GenerationConfig& config)
{
	auto sparams = llama_sampler_chain_default_params();
	llama_sampler* chain = llama_sampler_chain_init(sparams);
	if (config.topK > 0) llama_sampler_chain_add(chain, llama_sampler_init_top_k(config.topK));
	if (config.topP > 0.0f && config.topP < 1.0f)
		llama_sampler_chain_add(chain, llama_sampler_init_top_p(config.topP, 1));
	if (config.minP > 0.0f) llama_sampler_chain_add(chain, llama_sampler_init_min_p(config.minP, 1));
	llama_sampler_chain_add(chain, llama_sampler_init_temp(config.temperature));
	llama_sampler_chain_add(chain, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
	return chain;
}
} // namespace

class LlamaInference : public Inference {
public:
	bool loadModel(const std::string& ggufPath, int contextLen) override
	{
		if (!verifyModelSha256(ggufPath)) return false;
		std::call_once(g_backendOnce, initBackend);

		unloadModel();

		llama_model_params mparams = llama_model_default_params();
#if defined(RETDEC_NEURAL_GPU_OFFLOAD)
		mparams.n_gpu_layers = envInt("RETDEC_NEURAL_N_GPU_LAYERS", -1);
#else
		mparams.n_gpu_layers = envInt("RETDEC_NEURAL_N_GPU_LAYERS", 0);
#endif
		mparams.load_mtp = envFlag("RETDEC_NEURAL_MTP");

		if (!llama_supports_gpu_offload() && mparams.n_gpu_layers != 0)
		{
			std::fprintf(stderr, "retdec-neural: llama_supports_gpu_offload() is false; using CPU\n");
			mparams.n_gpu_layers = 0;
		}
		else if (mparams.n_gpu_layers != 0)
		{
			std::fprintf(
				stderr, "retdec-neural: GPU offload n_gpu_layers=%d\n", static_cast<int>(mparams.n_gpu_layers));
		}

		g_model = llama_model_load_from_file(ggufPath.c_str(), mparams);
		if (!g_model) return false;

		llama_context_params cparams = llama_context_default_params();
		cparams.n_ctx = static_cast<uint32_t>(contextLen);
		const int nThreads = envInt("RETDEC_NEURAL_THREADS", 0);
		if (nThreads > 0)
		{
			cparams.n_threads = nThreads;
			cparams.n_threads_batch = nThreads;
		}
		const int nBatch = envInt("RETDEC_NEURAL_N_BATCH", 512);
		if (nBatch > 0)
		{
			cparams.n_batch = static_cast<uint32_t>(nBatch);
			cparams.n_ubatch = static_cast<uint32_t>(nBatch);
		}
		g_context = llama_init_from_model(g_model, cparams);
		return g_context != nullptr;
	}

	void unloadModel() override
	{
		g_lastPromptTokens.clear();
		if (g_context)
		{
			llama_free(g_context);
			g_context = nullptr;
		}
		if (g_model)
		{
			llama_model_free(g_model);
			g_model = nullptr;
		}
	}

	bool isLoaded() const override
	{
		return g_context != nullptr;
	}

	GenerationResult generate(const std::string& prompt, const GenerationConfig& config) override
	{
		GenerationResult result;
		if (!g_context || !g_model)
		{
			result.error = "llama: model not loaded";
			return result;
		}

		const llama_vocab* vocab = llama_model_get_vocab(g_model);
		if (!vocab)
		{
			result.error = "llama: missing vocab";
			return result;
		}

		std::vector<llama_token> tokens(prompt.size() + 16);
		int n = llama_tokenize(
			vocab,
			prompt.c_str(),
			static_cast<int32_t>(prompt.size()),
			tokens.data(),
			static_cast<int32_t>(tokens.size()),
			true,
			false);
		if (n < 0)
		{
			tokens.resize(static_cast<std::size_t>(-n));
			n = llama_tokenize(
				vocab,
				prompt.c_str(),
				static_cast<int32_t>(prompt.size()),
				tokens.data(),
				static_cast<int32_t>(tokens.size()),
				true,
				false);
		}
		if (n < 0)
		{
			result.error = "llama: tokenize failed";
			return result;
		}
		tokens.resize(static_cast<std::size_t>(n));

		llama_memory_t mem = llama_get_memory(g_context);
		std::size_t common = 0;
		if (config.reuseKvPrefix && !g_lastPromptTokens.empty())
		{
			const std::size_t limit = std::min(g_lastPromptTokens.size(), tokens.size());
			while (common < limit && g_lastPromptTokens[common] == tokens[common])
				++common;
			if (common < g_lastPromptTokens.size()) llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(common), -1);
		}
		else
		{
			llama_memory_clear(mem, true);
		}

		const uint32_t nBatch = llama_n_batch(g_context);
		for (std::size_t i = common; i < tokens.size();)
		{
			const int32_t n = static_cast<int32_t>(std::min<std::size_t>(nBatch, tokens.size() - i));
			llama_batch batch = llama_batch_get_one(tokens.data() + i, n);
			if (llama_decode(g_context, batch) != 0)
			{
				result.error = "llama: decode failed";
				return result;
			}
			i += static_cast<std::size_t>(n);
		}

		g_lastPromptTokens = tokens;

		llama_sampler* smpl = buildSampler(config);
		std::string out;
		int nTok = 0;
		for (int i = 0; i < config.maxTokens; ++i)
		{
			llama_token tok = llama_sampler_sample(smpl, g_context, -1);
			llama_sampler_accept(smpl, tok);
			if (llama_vocab_is_eog(vocab, tok)) break;
			char piece[64];
			const int len = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, true);
			if (len > 0) out.append(piece, static_cast<std::size_t>(len));
			llama_batch next = llama_batch_get_one(&tok, 1);
			if (llama_decode(g_context, next) != 0) break;
			++nTok;
		}
		llama_sampler_free(smpl);

		result.text = std::move(out);
		result.tokensGenerated = nTok;
		result.ok = true;
		return result;
	}
};

std::unique_ptr<Inference> createLlamaInference()
{
	return std::make_unique<LlamaInference>();
}

#else

std::unique_ptr<Inference> createLlamaInference()
{
	return nullptr;
}

#endif

} // namespace retdec::neural
