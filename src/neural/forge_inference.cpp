/**
 * @file src/neural/forge_inference.cpp
 * @brief Forge-backed Inference implementation (third_party/forge).
 *
 * Uses Forge's C++ inference API (forge/inference): a shared
 * forge::inference::model, one forge::inference::session for the KV state,
 * Forge's sampler chain and its GBNF grammar constraint. A `.fpk` loads as
 * is; a `.gguf` or a Hugging Face directory is imported once into Forge's
 * model cache ($FORGE_CACHE, default ~/.cache/forge) under the same
 * content-addressed name `forge run` uses, so the two share imports.
 */

#include "retdec/neural/inference.h"
#include "retdec/neural/model_verify.h"

#include <forge/hash/blake3.hpp>
#include <forge/import/gguf.hpp>
#include <forge/import/hf.hpp>
#include <forge/inference/grammar.hpp>
#include <forge/inference/sampler.hpp>
#include <forge/inference/session.hpp>
#include <forge/modelfile/fpk.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace retdec::neural {

namespace {

namespace fs = std::filesystem;
namespace fi = forge::inference;

std::atomic<bool> g_cancel{false};

void onCancelSignal(int)
{
	g_cancel.store(true, std::memory_order_relaxed);
}

/// Routes SIGINT (and SIGTERM off Windows) to the cancel flag for as long as
/// this object is alive, and puts back what was there before. Installing the
/// handler for the life of the process would silently replace the host's own
/// disposition, and since the handler only raises a flag, Ctrl-C would stop
/// ending retdec-decompiler after the first refinement.
class CancelSignalScope {
public:
	CancelSignalScope()
	{
		previousInt_ = std::signal(SIGINT, onCancelSignal);
#ifndef _WIN32
		previousTerm_ = std::signal(SIGTERM, onCancelSignal);
#endif
	}

	~CancelSignalScope()
	{
		if (previousInt_ != SIG_ERR) std::signal(SIGINT, previousInt_);
#ifndef _WIN32
		if (previousTerm_ != SIG_ERR) std::signal(SIGTERM, previousTerm_);
#endif
	}

	CancelSignalScope(const CancelSignalScope&) = delete;
	CancelSignalScope& operator=(const CancelSignalScope&) = delete;

private:
	void (*previousInt_)(int) = SIG_ERR;
#ifndef _WIN32
	void (*previousTerm_)(int) = SIG_ERR;
#endif
};

bool deadlineExceeded(std::chrono::steady_clock::time_point start, int deadlineMs)
{
	if (deadlineMs <= 0) return false;
	const auto ms =
		std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
	return ms > deadlineMs;
}

int envInt(const char* name, int fallback)
{
	const char* v = std::getenv(name);
	if (!v || !v[0]) return fallback;
	while (*v == ' ' || *v == '\t')
		++v;
	int n = 0;
	const auto r = std::from_chars(v, v + std::strlen(v), n);
	if (r.ec != std::errc{}) return fallback;
	return n;
}

std::string envString(const char* name)
{
	const char* v = std::getenv(name);
	return v ? std::string(v) : std::string();
}

bool endsWith(const std::string& s, const std::string& suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// The codec non-.fpk models are imported with. `forge run` defaults to f32;
/// RETDEC_NEURAL_IMPORT_CODEC picks another (q8_0, q4_k_m, ...). GGUF imports
/// keep their quantised blocks whatever this says.
std::string importCodec()
{
	const std::string c = envString("RETDEC_NEURAL_IMPORT_CODEC");
	return c.empty() ? std::string("f32") : c;
}

/// `forge run <hf-dir|model.gguf>` resolution (apps/forge.cpp resolve_model):
/// import once into a content-addressed cache keyed on the absolute path,
/// codec, .fpk format version and each source file's name, size and mtime,
/// and reuse it while the sources are unchanged. Same directory and file
/// names as Forge's CLI, so an import made by either is reused by the other.
std::string resolveModel(const std::string& path, const std::string& codec)
{
	if (!fs::is_directory(path) && !endsWith(path, ".gguf")) return path;

	std::string key = fs::absolute(path).string() + "|" + codec + "|"
					+ std::to_string(int(forge::modelfile::format_version));
	auto stamp = [&](const fs::path& f) {
		key += std::format(
			"|{}:{}:{}",
			f.filename().string(),
			static_cast<unsigned long long>(fs::file_size(f)),
			static_cast<long long>(fs::last_write_time(f).time_since_epoch().count()));
	};
	if (fs::is_directory(path))
	{
		std::vector<fs::path> files;
		for (const auto& e: fs::directory_iterator(path))
			if (e.is_regular_file()) files.push_back(e.path());
		std::sort(files.begin(), files.end());
		for (const auto& f: files)
			stamp(f);
	}
	else
	{
		stamp(path);
	}

	const char* home = std::getenv("HOME");
#if defined(_WIN32)
	if (!home || !home[0]) home = std::getenv("USERPROFILE");
#endif
	const char* cacheEnv = std::getenv("FORGE_CACHE");
	const fs::path dir = cacheEnv && cacheEnv[0] ? fs::path(cacheEnv)
											   : fs::path(home && home[0] ? home : "/tmp") / ".cache" / "forge";
	fs::create_directories(dir);

	const auto digest = forge::hash::blake3::of(
		{reinterpret_cast<const std::uint8_t*>(key.data()), key.size()});
	const std::string h = forge::hash::blake3::hex(digest).substr(0, 24);
	// A trailing separator on a directory argument would leave filename() empty.
	fs::path src = fs::path(path);
	if (!src.has_filename()) src = src.parent_path();
	const fs::path out = dir / (src.filename().string() + "-" + codec + "-" + h + ".fpk");

	if (!fs::exists(out))
	{
		forge::import::import_options o;
		o.codec = codec;
		const std::string tmp = out.string() + ".partial";
		const auto rep = endsWith(path, ".gguf") ? forge::import::import_gguf(path, tmp, o)
												  : forge::import::import_hf(path, tmp, o);
		fs::rename(tmp, out);
		std::fprintf(
			stderr,
			"retdec-neural: imported %s (%s, %zu tensors) -> %s\n",
			path.c_str(),
			rep.template_name.c_str(),
			rep.tensors,
			out.string().c_str());
	}
	return out.string();
}

/// RETDEC_NEURAL_THREADS caps Forge's shared task pool. The pool reads
/// FORGE_THREADS once, when it is first used, so this only has an effect
/// before the first model is loaded in the process -- and an explicit
/// FORGE_THREADS wins.
void applyThreadCount()
{
	const int n = envInt("RETDEC_NEURAL_THREADS", 0);
	if (n <= 0 || std::getenv("FORGE_THREADS")) return;
	const std::string v = std::to_string(n);
#if defined(_WIN32)
	_putenv_s("FORGE_THREADS", v.c_str());
#else
	setenv("FORGE_THREADS", v.c_str(), 0);
#endif
}

/// The execution device. RETDEC_NEURAL_DEVICE takes Forge's FORGE_DEVICE
/// syntax ("cpu", "cuda", "cuda:1", "vulkan", "hip", "metal", "pipe:...");
/// Forge itself already honours FORGE_DEVICE when the model opens. Without
/// either, a build configured with RETDEC_NEURAL_GPU_OFFLOAD=ON asks for
/// CUDA. GPU runtimes are loaded at run time, so an unavailable device falls
/// back to the CPU with a warning rather than failing the load.
std::string requestedDevice()
{
	std::string d = envString("RETDEC_NEURAL_DEVICE");
	if (!d.empty()) return d;
	if (std::getenv("FORGE_DEVICE")) return {};
#if defined(RETDEC_NEURAL_GPU_OFFLOAD)
	return "cuda";
#else
	return {};
#endif
}

/// End-of-generation tokens: the model's EOS plus the chat-turn terminators
/// the common templates use (llama.cpp's llama_vocab_is_eog covers the same).
std::vector<int> endOfGenerationTokens(const forge::modelfile::bpe_tokenizer& tok)
{
	std::vector<int> ids;
	if (tok.eos() >= 0) ids.push_back(tok.eos());
	for (const char* t: {"<|im_end|>", "<|endoftext|>", "<|eot_id|>", "<|end_of_text|>", "<end_of_turn>", "<|end|>"})
	{
		const int id = tok.token_id(t);
		if (id >= 0 && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
	}
	return ids;
}

} // namespace

// One session is one KV state, so generate/load/unload share mutex_ and
// concurrent callers are serialised. Forge's model additionally serialises
// graph runs, so a model shared between sessions would still be safe.
class ForgeInference : public Inference {
public:
	~ForgeInference() override
	{
		unloadModel();
	}

	bool loadModel(const std::string& modelPath, int contextLen) override
	{
		try
		{
			const bool isDir = fs::is_directory(modelPath);
			// A file is checked as given (the allowlist pins the GGUF or .fpk
			// the user downloaded). A directory has no single hash; the .fpk
			// imported from it is checked instead.
			if (!isDir && !verifyModelSha256(modelPath)) return false;
			const std::string fpk = resolveModel(modelPath, importCodec());
			if (isDir && !verifyModelSha256(fpk)) return false;

			applyThreadCount();

			std::lock_guard<std::mutex> lock(mutex_);
			unloadUnlocked();

			auto model = fi::model::open(fpk);
			const std::string dev = requestedDevice();
			if (!dev.empty())
			{
				const std::string used = model->set_device(dev);
				std::fprintf(stderr, "retdec-neural: forge device %s\n", used.c_str());
			}
			if (model->blueprint().session != "generative_sequence")
			{
				std::fprintf(
					stderr,
					"retdec-neural: %s is a '%s' model, not a generative one\n",
					fpk.c_str(),
					model->blueprint().session.c_str());
				return false;
			}
			if (!model->tokenizer())
			{
				std::fprintf(stderr, "retdec-neural: %s has no tokenizer\n", fpk.c_str());
				return false;
			}

			fi::session_options opts;
			const std::int64_t maxCtx = model->max_context();
			opts.context = contextLen > 0 ? std::min<std::int64_t>(contextLen, maxCtx) : maxCtx;
			opts.kv_codec = envString("RETDEC_NEURAL_KV_CODEC");
			session_ = std::make_unique<fi::session>(std::move(model), opts);
			eog_ = endOfGenerationTokens(*session_->tokenizer());
			return true;
		}
		catch (const std::exception& e)
		{
			std::fprintf(stderr, "retdec-neural: forge could not load %s: %s\n", modelPath.c_str(), e.what());
			std::lock_guard<std::mutex> lock(mutex_);
			unloadUnlocked();
			return false;
		}
	}

	void unloadModel() override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		unloadUnlocked();
	}

	bool isLoaded() const override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return session_ != nullptr;
	}

	GenerationResult generate(const std::string& prompt, const GenerationConfig& config) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		GenerationResult result;
		if (!session_)
		{
			result.error = "forge: model not loaded";
			return result;
		}
		try
		{
			return generateUnlocked(prompt, config);
		}
		catch (const std::exception& e)
		{
			// The KV state may be half-written; never reuse it.
			lastPromptTokens_.clear();
			session_->reset();
			result.error = std::string("forge: ") + e.what();
			return result;
		}
	}

private:
	GenerationResult generateUnlocked(const std::string& prompt, const GenerationConfig& config)
	{
		GenerationResult result;
		const auto& tok = *session_->tokenizer();

		// Thinking mode. prompts.cpp asks Qwen for /think or /no_think; when
		// the vocabulary has the <think> markers, "off" is made certain the
		// way the Qwen3 template's enable_thinking=false does it -- an empty
		// reasoning block after the assistant header -- and "on" keeps the
		// reasoning out of the returned text and out of the grammar.
		const bool thinkTemplate = tok.token_id("<think>") >= 0 && tok.token_id("</think>") >= 0;
		std::string fullPrompt = prompt;
		if (thinkTemplate && !config.thinkingMode && endsWith(prompt, "<|im_start|>assistant\n"))
			fullPrompt += "<think>\n\n</think>\n\n";
		const bool reasoning = thinkTemplate && config.thinkingMode;

		// Chat markers are matched as special tokens; prompts.cpp neutralises
		// them inside untrusted data.
		std::vector<int> tokens = tok.encode(fullPrompt);
		if (tok.add_bos()) tokens.insert(tokens.begin(), tok.bos());
		if (tokens.empty())
		{
			result.error = "forge: empty prompt";
			return result;
		}

		// N11: refuse rather than silently truncate into the context.
		const std::int64_t nCtx = session_->context();
		const std::int64_t maxGen = config.maxTokens > 0 ? config.maxTokens : 0;
		if (nCtx <= 0 || static_cast<std::int64_t>(tokens.size()) + maxGen > nCtx)
		{
			result.error = "forge: prompt exceeds context budget";
			return result;
		}

		// Scoped to this generation: see CancelSignalScope. The flag is
		// cleared before the handlers go in, so an earlier signal cannot
		// cancel this call.
		g_cancel.store(false, std::memory_order_relaxed);
		const CancelSignalScope cancelSignals;
		const int deadlineMs = envInt("RETDEC_NEURAL_DEADLINE_MS", 0);
		const auto genStart = std::chrono::steady_clock::now();
		auto interrupted = [&](GenerationResult& r) {
			if (g_cancel.load(std::memory_order_relaxed))
			{
				r.error = "forge: cancelled";
				return true;
			}
			if (deadlineExceeded(genStart, deadlineMs))
			{
				r.error = "forge: deadline exceeded";
				return true;
			}
			return false;
		};

		// KV prefix reuse: keep the cached rows of the prompt prefix shared
		// with the previous call, and feed at least the last token again so
		// there are logits to sample from. Recurrent state (Qwen3.5 DeltaNet)
		// cannot be cut mid-sequence; can_share_prefix says so and the
		// session starts over.
		std::size_t keep = 0;
		if (config.reuseKvPrefix && !lastPromptTokens_.empty())
		{
			const std::size_t limit = std::min(lastPromptTokens_.size(), tokens.size() - 1);
			while (keep < limit && lastPromptTokens_[keep] == tokens[keep])
				++keep;
			if (keep > 0 && session_->can_share_prefix(static_cast<std::int64_t>(keep)))
				session_->rewind(static_cast<std::int64_t>(keep));
			else
				keep = 0;
		}
		if (keep == 0) session_->reset();
		lastPromptTokens_.clear();

		const std::size_t nBatch = static_cast<std::size_t>(std::max(1, envInt("RETDEC_NEURAL_N_BATCH", 512)));
		std::vector<float> logits;
		for (std::size_t i = keep; i < tokens.size();)
		{
			if (interrupted(result))
			{
				session_->reset();
				return result;
			}
			const std::size_t n = std::min(nBatch, tokens.size() - i);
			logits = session_->forward(std::span<const int>(tokens.data() + i, n));
			i += n;
		}
		lastPromptTokens_ = tokens;

		fi::sampler_config sc;
		sc.temperature = config.temperature;
		sc.top_k = config.topK > 0 ? config.topK : 0;
		sc.top_p = config.topP > 0.0f && config.topP < 1.0f ? config.topP : 1.0f;
		sc.min_p = config.minP > 0.0f ? config.minP : 0.0f;
		sc.seed = config.seed;
		fi::sampler smp(sc);

		fi::grammar_constraint* gc = grammarFor(config, tok);
		bool grammarActive = gc != nullptr && !reasoning;
		if (gc) smp.filter = [&](std::span<float> l) {
			if (grammarActive) gc->apply(l);
		};

		std::vector<int> history = tokens;
		std::string out;
		int nTok = 0;
		double probSum = 0.0;
		int probN = 0;
		for (int i = 0; i < config.maxTokens; ++i)
		{
			if (interrupted(result)) return result;

			// The whole chain's distribution, then a draw from it: the same
			// draw sample() makes, plus the selected token's probability (N17).
			const std::vector<double> p = smp.probs(logits, history);
			double total = 0.0;
			for (const double x: p)
				total += x;
			if (!std::isfinite(total) || !(total > 0.0)) break; // every token masked
			const int t = smp.draw(p);
			const double tp = t >= 0 && static_cast<std::size_t>(t) < p.size() ? p[static_cast<std::size_t>(t)] : 0.0;
			if (!(tp > 0.0)) break; // every token masked: the grammar has no continuation
			probSum += tp;
			++probN;

			if (std::find(eog_.begin(), eog_.end(), t) != eog_.end()) break;
			if (grammarActive && !gc->accept(t)) break;
			out += tok.decode_one(t);
			history.push_back(t);
			++nTok;

			if (reasoning && !grammarActive && gc && out.find("</think>") != std::string::npos)
				grammarActive = true;
			if (grammarActive && gc->finished()) break;
			if (i + 1 == config.maxTokens || session_->position() >= nCtx) break;
			logits = session_->forward(std::span<const int>(&t, 1));
		}

		if (reasoning)
		{
			const auto end = out.find("</think>");
			if (end == std::string::npos)
			{
				result.error = "forge: reasoning did not finish within maxTokens";
				return result;
			}
			std::size_t from = end + std::strlen("</think>");
			while (from < out.size() && (out[from] == '\n' || out[from] == ' '))
				++from;
			out.erase(0, from);
		}

		result.text = std::move(out);
		result.tokensGenerated = nTok;
		if (probN > 0)
		{
			result.hasTokenProb = true;
			result.meanTokenProb = static_cast<float>(probSum / static_cast<double>(probN));
		}
		result.ok = true;
		return result;
	}

	/// The grammar constraint for this call, or null. Building one walks the
	/// whole vocabulary into a token trie, so it is kept between calls with
	/// the same grammar (its allowed-token cache stays valid) and reset.
	fi::grammar_constraint* grammarFor(const GenerationConfig& config, const forge::modelfile::bpe_tokenizer& tok)
	{
		if (config.grammarGbnf.empty()) return nullptr;
		const std::string root = config.grammarRoot.empty() ? std::string("root") : config.grammarRoot;
		if (!grammar_ || grammarSource_ != config.grammarGbnf || grammarRoot_ != root)
		{
			grammar_.reset();
			grammarSource_ = config.grammarGbnf;
			grammarRoot_ = root;
			try
			{
				auto g = std::make_shared<const fi::grammar>(grammarSource_, grammarRoot_);
				const int eos = eog_.empty() ? -1 : eog_.front();
				grammar_ = std::make_unique<fi::grammar_constraint>(g, tok, static_cast<int>(session_->vocab()), eos);
			}
			catch (const std::exception& e)
			{
				std::fprintf(stderr, "retdec-neural: GBNF parse failed (%s); unconstrained decode\n", e.what());
				return nullptr;
			}
		}
		grammar_->reset();
		return grammar_.get();
	}

	void unloadUnlocked()
	{
		lastPromptTokens_.clear();
		grammar_.reset();
		grammarSource_.clear();
		grammarRoot_.clear();
		eog_.clear();
		session_.reset();
	}

	std::unique_ptr<fi::session> session_;
	std::vector<int> eog_;
	std::vector<int> lastPromptTokens_;
	std::unique_ptr<fi::grammar_constraint> grammar_;
	std::string grammarSource_;
	std::string grammarRoot_;
	mutable std::mutex mutex_;
};

std::unique_ptr<Inference> createForgeInference()
{
	return std::make_unique<ForgeInference>();
}

} // namespace retdec::neural
