#include "retdec/neural/decompile_hook.h"
#include "retdec/neural/gates.h"
#include "retdec/neural/inference.h"
#include "retdec/neural/refiner.h"

#include "retdec/common/function.h"
#include "retdec/common/semantic_detection.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace retdec::neural {

namespace {

bool envEnabled(const char* name)
{
	const char* v = std::getenv(name);
	return v && v[0] != '\0' && v[0] != '0';
}

std::string modelPathFromEnv()
{
	const char* p = std::getenv("RETDEC_NEURAL_MODEL");
	return p ? std::string(p) : std::string();
}

int tierMaxFromEnv()
{
	const char* t = std::getenv("RETDEC_NEURAL_TIER_MAX");
	if (!t || !t[0]) return 3;
	const int v = std::atoi(t);
	return v < 1 ? 1 : (v > 5 ? 5 : v);
}

float envFloat(const char* name, float fallback)
{
	const char* v = std::getenv(name);
	if (!v || !v[0]) return fallback;
	char* end = nullptr;
	const float x = std::strtof(v, &end);
	if (end == v) return fallback;
	return x;
}

int envInt(const char* name, int fallback)
{
	const char* v = std::getenv(name);
	if (!v || !v[0]) return fallback;
	const int n = std::atoi(v);
	return n == 0 && v[0] != '0' ? fallback : n;
}

void writeSidecar(const std::string& basePath, const std::string& refined, const std::string& manifest)
{
	if (basePath.empty()) return;
	const std::string refinedPath = basePath + ".refined.c";
	const std::string manifestPath = basePath + ".refinement-manifest.json";
	std::ofstream(refinedPath) << refined;
	std::ofstream(manifestPath) << manifest;
}

std::string jsonEscape(const std::string& s)
{
	std::ostringstream oss;
	for (char c: s)
	{
		switch (c)
		{
		case '"': oss << "\\\""; break;
		case '\\': oss << "\\\\"; break;
		case '\n': oss << "\\n"; break;
		case '\r': oss << "\\r"; break;
		default: oss << c; break;
		}
	}
	return oss.str();
}

std::string serializeSemanticContext(const retdec::config::Config& config)
{
	std::ostringstream oss;
	oss << "{\"functions\":[";
	bool firstFn = true;
	for (const auto& fn: config.functions)
	{
		if (fn.semanticDetections.empty()) continue;
		if (!firstFn) oss << ',';
		firstFn = false;
		oss << "{\"name\":\"" << jsonEscape(fn.getName()) << "\",\"detections\":[";
		bool firstDet = true;
		for (const auto& d: fn.semanticDetections)
		{
			if (!firstDet) oss << ',';
			firstDet = false;
			oss << "{\"kind\":\"" << jsonEscape(d.kind) << "\",\"label\":\"" << jsonEscape(d.label)
				<< "\",\"confidence\":" << d.confidence;
			if (!d.detail.empty()) oss << ",\"detail\":\"" << jsonEscape(d.detail) << '"';
			oss << '}';
		}
		oss << "]}";
	}
	oss << "]}";
	return oss.str();
}

} // namespace

void maybeRefineDecompilerOutput(retdec::config::Config& config, std::string* outString)
{
	std::string fileBuf;
	if (!outString || outString->empty())
	{
		const std::string path = config.parameters.getOutputFile();
		if (path.empty()) return;
		std::ifstream in(path);
		if (!in) return;
		fileBuf.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		if (fileBuf.empty()) return;
		outString = &fileBuf;
	}
	if (!envEnabled("RETDEC_NEURAL_REFINE")) return;

#ifndef RETDEC_NEURAL_OFFLINE_ONLY
	if (envEnabled("RETDEC_NO_NETWORK"))
	{
		if (!envEnabled("RETDEC_NEURAL_ALLOW_NETWORK")) return;
	}
#endif

	const std::string model = modelPathFromEnv();
	if (model.empty()) return;

	std::unique_ptr<Inference> backend;
	if (envEnabled("RETDEC_NEURAL_FORCE_MOCK"))
	{
#if defined(NDEBUG) && !defined(RETDEC_NEURAL_ALLOW_MOCK)
		std::fprintf(stderr, "retdec-neural: mock inference is disabled in release builds\n");
		return;
#else
		backend = createMockInference();
#endif
	}
	else
	{
		backend = createLlamaInference();
		if (!backend)
		{
#if defined(NDEBUG) && !defined(RETDEC_NEURAL_ALLOW_MOCK)
			std::fprintf(stderr, "retdec-neural: no llama.cpp backend; mock fallback disabled in release\n");
			return;
#else
			backend = createMockInference();
#endif
		}
	}
	int ctxLen = 4096;
	if (const char* ctxEnv = std::getenv("RETDEC_NEURAL_CTX"))
	{
		const int v = std::atoi(ctxEnv);
		if (v >= 512) ctxLen = v;
	}
	if (!backend->loadModel(model, ctxLen))
	{
		std::fprintf(stderr, "retdec-neural: failed to load GGUF: %s\n", model.c_str());
		const std::string outPath = config.parameters.getOutputFile();
		if (!outPath.empty())
		{
			std::ofstream(outPath + ".refinement-manifest.json")
				<< R"({"accepted":false,"reason":"failed to load GGUF"})";
		}
		return;
	}

	Refiner refiner(std::move(backend));
	const std::string semanticJson = serializeSemanticContext(config);

	static const RefinementTier kTiers[] = {
		RefinementTier::Naming,
		RefinementTier::Comments,
		RefinementTier::StructFields,
		RefinementTier::IdiomRecovery,
		RefinementTier::FullRewrite,
	};

	const int tierMax = tierMaxFromEnv();
	std::string current = *outString;
	std::string lastManifest = R"({"accepted":false,"reason":"no tier ran"})";
	bool anyAccepted = false;
	bool compileRetryUsed = false;
	const bool requireCompile = envEnabled("RETDEC_NEURAL_REQUIRE_COMPILE");

	for (int i = 0; i < tierMax && i < 5; ++i)
	{
		RefinementRequest req;
		req.functionSource = current;
		req.tier = kTiers[i];
		req.semanticContextJson = semanticJson;
		req.generation.reuseKvPrefix = envEnabled("RETDEC_NEURAL_REUSE_KV") && (i > 0);
		// Naming/Comments/StructFields default to temperature 0 (N9).
		// Other tiers keep the Qwen Instruct default unless the env is set.
		const bool conservativeTier =
			(req.tier == RefinementTier::Naming || req.tier == RefinementTier::Comments
			 || req.tier == RefinementTier::StructFields);
		req.generation.temperature = envFloat("RETDEC_NEURAL_TEMPERATURE", conservativeTier ? 0.0f : 0.6f);
		req.generation.topP = envFloat("RETDEC_NEURAL_TOP_P", 0.95f);
		const int topK = envInt("RETDEC_NEURAL_TOP_K", 20);
		if (topK > 0) req.generation.topK = topK;
		if (std::isfinite(req.generation.temperature))
		{
			if (req.generation.temperature < 0.0f)
				req.generation.temperature = 0.0f;
			else if (req.generation.temperature > 2.0f)
				req.generation.temperature = 2.0f;
		}
		if (std::isfinite(req.generation.topP))
		{
			if (req.generation.topP < 0.0f)
				req.generation.topP = 0.0f;
			else if (req.generation.topP > 1.0f)
				req.generation.topP = 1.0f;
		}
		req.generation.minP = 0.0f;
		req.generation.thinkingMode = envEnabled("RETDEC_NEURAL_THINKING");
		if (req.tier == RefinementTier::Naming)
		{
			req.generation.grammarGbnf = namingRenameMapGbnf();
			req.generation.grammarRoot = "root";
		}
		const char* maxTok = std::getenv("RETDEC_NEURAL_MAX_TOKENS");
		if (maxTok && maxTok[0])
		{
			const int n = std::atoi(maxTok);
			if (n > 0) req.generation.maxTokens = n;
		}

		const auto resp = refiner.refine(req);
		lastManifest = resp.manifestJson;
		if (resp.accepted)
		{
			current = resp.refinedSource;
			anyAccepted = true;
		}

		const bool compileReject = !resp.accepted
								&& (resp.manifestJson.find("compile_syntax") != std::string::npos
									|| resp.manifestJson.find("compile=fail") != std::string::npos);
		if (compileRetryUsed) continue;
		if (!compileReject && !requireCompile) continue;

		std::string diags;
		const std::string attempt = resp.refinedSource.empty() ? current : resp.refinedSource;
		const bool compiles = compileSyntaxOnly(attempt, diags);
		if (resp.accepted && compiles) continue;

		compileRetryUsed = true;
		RefinementRequest retry = req;
		retry.functionSource = current;
		retry.tier = RefinementTier::FullRewrite;
		retry.generation.reuseKvPrefix = false; // compile-retry never reuses KV
		retry.compilerDiagnostics = diags.empty() ? std::string("cc -fsyntax-only failed") : diags;

		const auto retryResp = refiner.refine(retry);
		lastManifest = retryResp.manifestJson;
		if (retryResp.accepted && compileSyntaxOnly(retryResp.refinedSource))
		{
			current = retryResp.refinedSource;
			anyAccepted = true;
		}
		else if (retryResp.accepted)
		{
			lastManifest = R"({"accepted":false,"reason":"compile_syntax"})";
		}
	}

	const std::string outPath = config.parameters.getOutputFile();
	if (anyAccepted)
	{
		writeSidecar(outPath, current, lastManifest);
	}
	else if (!outPath.empty())
	{
		std::ofstream(outPath + ".refinement-manifest.json") << lastManifest;
		std::fprintf(stderr, "retdec-neural: no tier accepted; %s\n", lastManifest.c_str());
	}
}

} // namespace retdec::neural
