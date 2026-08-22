#include "retdec/neural/refiner.h"
#include "retdec/neural/gates.h"
#include "retdec/neural/model_verify.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <cctype>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace retdec::neural {

std::string buildRefinementPrompt(const RefinementRequest& request);

namespace {

bool envFlag(const char* name)
{
	const char* v = std::getenv(name);
	return v && v[0] != '\0' && v[0] != '0';
}

bool isConservativeTier(RefinementTier tier)
{
	return tier == RefinementTier::Naming || tier == RefinementTier::Comments || tier == RefinementTier::StructFields;
}

// GenerationConfig defaults (0.7) and the hook Qwen default (0.6) are
// default-high for Naming/Comments/StructFields — force greedy decode.
void applyDeterministicDefaults(GenerationConfig& gen, RefinementTier tier)
{
	if (isConservativeTier(tier) && (gen.temperature == 0.7f || gen.temperature == 0.6f)) gen.temperature = 0.0f;
	if (!envFlag("RETDEC_NEURAL_REUSE_KV")) gen.reuseKvPrefix = false;
}

std::string compileGateLabel(const GateReport* gates, bool compileSyntaxReject)
{
	if (compileSyntaxReject) return "fail";
	if (!gates) return "skip";
	if (envFlag("RETDEC_NEURAL_SKIP_COMPILE_GATE")) return "skip";
	if (gates->structural != GateResult::Pass) return "skip";
	if (gates->compile == GateResult::FailCompile) return "fail";
	return "pass";
}

std::string stripMarkdownFences(std::string text)
{
	while (!text.empty() && (text.front() == '\n' || text.front() == '\r' || text.front() == ' '))
		text.erase(text.begin());
	if (text.compare(0, 3, "```") != 0) return text;
	const auto nl = text.find('\n');
	if (nl == std::string::npos) return text;
	text.erase(0, nl + 1);
	const auto end = text.rfind("```");
	if (end != std::string::npos) text.erase(end);
	return text;
}

std::string buildManifest(
	bool accepted,
	const std::string& reason,
	const RefinementRequest& request,
	const GenerationConfig& gen,
	const std::string& outputSource,
	const std::string& compileGate,
	long long wallMs,
	const std::string& detail = {})
{
	const std::string inHash = sha256HexOfBytes(request.functionSource.data(), request.functionSource.size());
	const std::string outHash = sha256HexOfBytes(outputSource.data(), outputSource.size());

	rapidjson::StringBuffer sb;
	rapidjson::Writer<rapidjson::StringBuffer> w(sb);
	w.StartObject();
	w.Key("accepted");
	w.Bool(accepted);
	w.Key("reason");
	w.String(reason.c_str(), static_cast<rapidjson::SizeType>(reason.size()));
	w.Key("tier");
	w.Int(static_cast<int>(request.tier));
	w.Key("seed");
	w.Uint(gen.seed);
	w.Key("temperature");
	w.Double(gen.temperature);
	w.Key("top_p");
	w.Double(gen.topP);
	w.Key("top_k");
	w.Int(gen.topK);
	w.Key("reuse_kv");
	w.Bool(gen.reuseKvPrefix);
	w.Key("input_sha256");
	w.String(inHash.c_str(), static_cast<rapidjson::SizeType>(inHash.size()));
	w.Key("output_sha256");
	w.String(outHash.c_str(), static_cast<rapidjson::SizeType>(outHash.size()));
	w.Key("compile_gate");
	w.String(compileGate.c_str(), static_cast<rapidjson::SizeType>(compileGate.size()));
	w.Key("wall_ms");
	w.Int64(wallMs);
	if (!detail.empty())
	{
		w.Key("detail");
		w.String(detail.c_str(), static_cast<rapidjson::SizeType>(detail.size()));
	}
	w.EndObject();
	return sb.GetString();
}

std::string summarizeFunctionSource(const std::string& src)
{
	if (src.size() < 2048) return src;
	std::vector<std::string> lines;
	std::string line;
	for (char c: src)
	{
		line.push_back(c);
		if (c == '\n')
		{
			lines.push_back(std::move(line));
			line.clear();
		}
	}
	if (!line.empty()) lines.push_back(std::move(line));
	if (lines.size() <= 80) return src;

	std::string out;
	const std::size_t head = 40;
	const std::size_t tail = 20;
	for (std::size_t i = 0; i < head && i < lines.size(); ++i)
		out += lines[i];
	out += "/* [truncated for context] */\n";
	const std::size_t start = lines.size() > tail ? lines.size() - tail : 0;
	for (std::size_t i = start; i < lines.size(); ++i)
		out += lines[i];
	return out;
}

} // namespace

const char* namingRenameMapGbnf()
{
	return R"gbnf(
root ::= object
object ::= "{" ws (pair ("," ws pair)*)? "}"
pair ::= string ws ":" ws string
string ::= "\"" chars "\""
chars ::= char*
char ::= [^"\\] | "\\" ["\\/bfnrt]
ws ::= [ \t\n]*
)gbnf";
}

std::string applyJsonRenameMap(const std::string& source, const std::string& jsonObject)
{
	rapidjson::Document doc;
	if (doc.Parse(jsonObject.c_str()).HasParseError() || !doc.IsObject()) return source;

	std::vector<std::pair<std::string, std::string>> pairs;
	for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it)
	{
		if (!it->name.IsString() || !it->value.IsString()) continue;
		const std::string from = it->name.GetString();
		const std::string to = it->value.GetString();
		if (from.empty() || to.empty() || from == to) continue;
		auto isIdent = [](const std::string& s) {
			if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
			for (char c: s)
			{
				if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
			}
			return true;
		};
		if (!isIdent(from) || !isIdent(to)) continue;
		if (from == "if" || from == "for" || from == "while" || from == "return" || from == "int") continue;
		pairs.emplace_back(from, to);
	}
	std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });

	std::string out;
	out.reserve(source.size() + 16);
	for (std::size_t i = 0; i < source.size();)
	{
		if (!(std::isalpha(static_cast<unsigned char>(source[i])) || source[i] == '_'))
		{
			out.push_back(source[i]);
			++i;
			continue;
		}
		const std::size_t start = i;
		++i;
		while (i < source.size() && (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_'))
			++i;
		const std::string tok = source.substr(start, i - start);
		std::string repl = tok;
		for (const auto& p: pairs)
		{
			if (p.first == tok)
			{
				repl = p.second;
				break;
			}
		}
		out += repl;
	}
	return out;
}

Refiner::Refiner(std::unique_ptr<Inference> backend): inference_(std::move(backend)) {}

RefinementResponse Refiner::refine(const RefinementRequest& request) const
{
	const auto t0 = std::chrono::steady_clock::now();
	auto wallMs = [&t0]() -> long long {
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	};

	GenerationConfig gen = request.generation;
	applyDeterministicDefaults(gen, request.tier);

	RefinementResponse response;
	if (!inference_ || !inference_->isLoaded())
	{
		response.refinedSource = request.functionSource;
		response.accepted = false;
		response.manifestJson = buildManifest(
			false, "no backend", request, gen, request.functionSource, compileGateLabel(nullptr, false), wallMs());
		return response;
	}

	std::string prompt = buildRefinementPrompt(request);

	auto genResult = inference_->generate(prompt, gen);
	if (!genResult.ok && genResult.error.find("prompt exceeds context budget") != std::string::npos)
	{
		// N11: retry once with head/tail source instead of silent truncate.
		std::fprintf(stderr, "retdec-neural: context budget exceeded; retrying with truncated source\n");
		RefinementRequest summary = request;
		summary.functionSource = summarizeFunctionSource(request.functionSource);
		prompt = buildRefinementPrompt(summary);
		genResult = inference_->generate(prompt, gen);
	}
	if (!genResult.ok)
	{
		response.refinedSource = request.functionSource;
		response.accepted = false;
		response.manifestJson = buildManifest(
			false,
			"generation failed",
			request,
			gen,
			request.functionSource,
			compileGateLabel(nullptr, false),
			wallMs());
		std::fprintf(stderr, "retdec-neural: generation failed: %s\n", genResult.error.c_str());
		return response;
	}

	std::string refined = stripMarkdownFences(genResult.text);
	if (request.tier == RefinementTier::Naming)
	{
		std::string json = refined;
		while (!json.empty() && (json.front() == ' ' || json.front() == '\n'))
			json.erase(json.begin());
		if (!json.empty() && json.front() == '{') refined = applyJsonRenameMap(request.functionSource, json);
	}
	const auto gates = runVerificationGates(request.functionSource, refined);
	if (!gates.allPassed())
	{
		// Keep the failed attempt when compile failed so the hook can capture diagnostics.
		response.refinedSource = (gates.compile == GateResult::FailCompile) ? refined : request.functionSource;
		response.accepted = false;
		response.manifestJson = buildManifest(
			false,
			"gates failed",
			request,
			gen,
			response.refinedSource,
			compileGateLabel(&gates, false),
			wallMs(),
			gates.summary());
		std::fprintf(stderr, "retdec-neural: gates failed (%s)\n", gates.summary().c_str());
		return response;
	}

	const char* requireCompile = std::getenv("RETDEC_NEURAL_REQUIRE_COMPILE");
	if (requireCompile && requireCompile[0] != '\0' && requireCompile[0] != '0')
	{
		if (!compileSyntaxOnly(refined))
		{
			response.refinedSource = refined;
			response.accepted = false;
			response.manifestJson =
				buildManifest(false, "compile_syntax", request, gen, refined, compileGateLabel(&gates, true), wallMs());
			std::fprintf(stderr, "retdec-neural: compile_syntax rejected refinement\n");
			return response;
		}
	}

	response.refinedSource = refined;
	response.accepted = true;
	response.manifestJson =
		buildManifest(true, "accepted", request, gen, refined, compileGateLabel(&gates, false), wallMs());
	return response;
}

} // namespace retdec::neural
