#include "retdec/neural/refiner.h"
#include "retdec/neural/gates.h"
#include "retdec/neural/model_verify.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

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

std::string cacheDirFromEnv()
{
	const char* d = std::getenv("RETDEC_NEURAL_CACHE_DIR");
	if (!d || d[0] == '\0') return {};
	return d;
}

std::string cacheKeyHex(const std::string& prompt, const GenerationConfig& gen, RefinementTier tier)
{
	const char* model = std::getenv("RETDEC_NEURAL_MODEL");
	const char* modelSha = std::getenv("RETDEC_NEURAL_MODEL_SHA256");
	std::string blob;
	blob += model ? model : "";
	blob += '\n';
	blob += modelSha ? modelSha : "unpinned";
	blob += '\n';
	blob += std::to_string(static_cast<int>(tier));
	blob += '\n';
	blob += std::to_string(gen.seed);
	blob += '\n';
	blob += std::to_string(gen.temperature);
	blob += '\n';
	blob += std::to_string(gen.topP);
	blob += '\n';
	blob += std::to_string(gen.topK);
	blob += '\n';
	blob += std::to_string(gen.maxTokens);
	blob += '\n';
	blob += gen.grammarGbnf;
	blob += '\n';
	blob += prompt;
	return sha256HexOfBytes(blob.data(), blob.size());
}

bool readCacheFile(const std::string& dir, const std::string& key, std::string* out)
{
	if (dir.empty() || key.empty() || !out) return false;
	std::error_code ec;
	const auto path = std::filesystem::path(dir) / (key + ".txt");
	if (!std::filesystem::is_regular_file(path, ec)) return false;
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return !out->empty();
}

void writeCacheFile(const std::string& dir, const std::string& key, const std::string& text)
{
	if (dir.empty() || key.empty() || text.empty()) return;
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	if (ec) return;
	const auto path = std::filesystem::path(dir) / (key + ".txt");
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) return;
	out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

bool isForbiddenRenameIdent(const std::string& s)
{
	// C11 keywords (header: applyJsonRenameMap skips C keywords) plus the
	// spawn-family idents compared by the structural gate.
	static const char* const kForbidden[] = {
		"auto",          "break",         "case",           "char",
		"const",         "continue",      "default",        "do",
		"double",        "else",          "enum",           "extern",
		"float",         "for",           "goto",           "if",
		"inline",        "int",           "long",           "register",
		"restrict",      "return",        "short",          "signed",
		"sizeof",        "static",        "struct",         "switch",
		"typedef",       "union",         "unsigned",       "void",
		"volatile",      "while",         "_Alignas",       "_Alignof",
		"_Atomic",       "_Bool",         "_Complex",       "_Generic",
		"_Imaginary",    "_Noreturn",     "_Static_assert", "_Thread_local",
		"system",        "popen",         "execve",         "execl",
		"execle",        "execlp",        "execv",          "execvp",
		"execvpe",       "WinExec",       "ShellExecute",   "ShellExecuteA",
		"ShellExecuteW", "CreateProcess", "CreateProcessA", "CreateProcessW",
		"_popen",        "_wpopen",       "_wsystem",
	};
	for (const char* w: kForbidden)
	{
		if (s == w) return true;
	}
	return false;
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
		if (isForbiddenRenameIdent(from) || isForbiddenRenameIdent(to)) continue;
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
	const std::string cdir = cacheDirFromEnv();
	const std::string ckey = cacheKeyHex(prompt, gen, request.tier);
	std::string cached;
	const bool cacheHit = !cdir.empty() && readCacheFile(cdir, ckey, &cached);

	GenerationResult genResult;
	if (cacheHit)
	{
		genResult.ok = true;
		genResult.text = cached;
	}
	else
	{
		genResult = inference_->generate(prompt, gen);
		if (!genResult.ok && genResult.error.find("prompt exceeds context budget") != std::string::npos)
		{
			// N11: retry once with head/tail source instead of silent truncate.
			std::fprintf(stderr, "retdec-neural: context budget exceeded; retrying with truncated source\n");
			RefinementRequest summary = request;
			summary.functionSource = summarizeFunctionSource(request.functionSource);
			prompt = buildRefinementPrompt(summary);
			genResult = inference_->generate(prompt, gen);
		}
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

	std::string refined = cacheHit ? cached : stripMarkdownFences(genResult.text);
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

	if (!cdir.empty() && !cacheHit) writeCacheFile(cdir, ckey, refined);

	response.refinedSource = refined;
	response.accepted = true;
	response.manifestJson = buildManifest(
		true, cacheHit ? "cache hit" : "accepted", request, gen, refined, compileGateLabel(&gates, false), wallMs());
	return response;
}

} // namespace retdec::neural
