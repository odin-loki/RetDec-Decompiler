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

llama_sampler* buildSampler(const GenerationConfig& config, const llama_vocab* vocab)
{
	auto sparams = llama_sampler_chain_default_params();
	llama_sampler* chain = llama_sampler_chain_init(sparams);
	if (vocab && !config.grammarGbnf.empty())
	{
		const char* root = config.grammarRoot.empty() ? "root" : config.grammarRoot.c_str();
		llama_sampler* g = llama_sampler_init_grammar(vocab, config.grammarGbnf.c_str(), root);
		if (g)
			llama_sampler_chain_add(chain, g);
		else
			std::fprintf(stderr, "retdec-neural: GBNF parse failed; unconstrained decode\n");
	}
	if (config.topK > 0) llama_sampler_chain_add(chain, llama_sampler_init_top_k(config.topK));
	if (config.topP > 0.0f && config.topP < 1.0f)
		llama_sampler_chain_add(chain, llama_sampler_init_top_p(config.topP, 1));
	if (config.minP > 0.0f) llama_sampler_chain_add(chain, llama_sampler_init_min_p(config.minP, 1));
	llama_sampler_chain_add(chain, llama_sampler_init_temp(config.temperature));
	llama_sampler_chain_add(chain, llama_sampler_init_dist(config.seed));
	return chain;
}
} // namespace

// One llama_context is single-threaded. generate/load/unload share mutex_.
// Concurrent callers are serialized; do not decode from two threads on the
// same context. Batched multi-sequence decode needs n_seq_max > 1 (unused).
class LlamaInference : public Inference {
public:
	~LlamaInference() override
	{
		unloadModel();
	}

	bool loadModel(const std::string& ggufPath, int contextLen) override
	{
		if (!verifyModelSha256(ggufPath)) return false;
		std::call_once(g_backendOnce, initBackend);

		std::lock_guard<std::mutex> lock(mutex_);
		unloadUnlocked();

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

		model_ = llama_model_load_from_file(ggufPath.c_str(), mparams);
		if (!model_) return false;

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
		context_ = llama_init_from_model(model_, cparams);
		return context_ != nullptr;
	}

	void unloadModel() override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		unloadUnlocked();
	}

	bool isLoaded() const override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return context_ != nullptr;
	}

	GenerationResult generate(const std::string& prompt, const GenerationConfig& config) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		GenerationResult result;
		if (!context_ || !model_)
		{
			result.error = "llama: model not loaded";
			return result;
		}

		const llama_vocab* vocab = llama_model_get_vocab(model_);
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

		// N11: refuse rather than silently truncate into n_ctx.
		const uint32_t nCtx = llama_n_ctx(context_);
		const int maxGen = config.maxTokens > 0 ? config.maxTokens : 0;
		if (nCtx == 0 || static_cast<uint64_t>(n) + static_cast<uint64_t>(maxGen) > nCtx)
		{
			result.error = "llama: prompt exceeds context budget";
			return result;
		}

		llama_memory_t mem = llama_get_memory(context_);
		std::size_t common = 0;
		if (config.reuseKvPrefix && !lastPromptTokens_.empty())
		{
			const std::size_t limit = std::min(lastPromptTokens_.size(), tokens.size());
			while (common < limit && lastPromptTokens_[common] == tokens[common])
				++common;
			if (common < lastPromptTokens_.size()) llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(common), -1);
		}
		else
		{
			llama_memory_clear(mem, true);
		}

		const uint32_t nBatch = llama_n_batch(context_);
		for (std::size_t i = common; i < tokens.size();)
		{
			const int32_t nTok = static_cast<int32_t>(std::min<std::size_t>(nBatch, tokens.size() - i));
			llama_batch batch = llama_batch_get_one(tokens.data() + i, nTok);
			if (llama_decode(context_, batch) != 0)
			{
				result.error = "llama: decode failed";
				return result;
			}
			i += static_cast<std::size_t>(nTok);
		}

		lastPromptTokens_ = tokens;

		llama_sampler* smpl = buildSampler(config, vocab);
		std::string out;
		int nTok = 0;
		for (int i = 0; i < config.maxTokens; ++i)
		{
			llama_token tok = llama_sampler_sample(smpl, context_, -1);
			llama_sampler_accept(smpl, tok);
			if (llama_vocab_is_eog(vocab, tok)) break;
			char piece[64];
			int len = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, true);
			if (len < 0)
			{
				std::vector<char> buf(static_cast<std::size_t>(-len));
				len = llama_token_to_piece(vocab, tok, buf.data(), static_cast<int32_t>(buf.size()), 0, true);
				if (len > 0) out.append(buf.data(), static_cast<std::size_t>(len));
			}
			else if (len > 0)
			{
				out.append(piece, static_cast<std::size_t>(len));
			}
			llama_batch next = llama_batch_get_one(&tok, 1);
			if (llama_decode(context_, next) != 0) break;
			++nTok;
		}
		llama_sampler_free(smpl);

		result.text = std::move(out);
		result.tokensGenerated = nTok;
		result.ok = true;
		return result;
	}

private:
	void unloadUnlocked()
	{
		lastPromptTokens_.clear();
		if (context_)
		{
			llama_free(context_);
			context_ = nullptr;
		}
		if (model_)
		{
			llama_model_free(model_);
			model_ = nullptr;
		}
	}

	llama_model* model_ = nullptr;
	llama_context* context_ = nullptr;
	std::vector<llama_token> lastPromptTokens_;
	mutable std::mutex mutex_;
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
