#include "retdec/neural/gates.h"
#include "retdec/neural/inference.h"
#include "retdec/neural/model_verify.h"
#include "retdec/neural/refiner.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using namespace retdec::neural;

namespace {

class EnvGuard
{
public:
	EnvGuard(const char* key, const char* value)
		: key_(key)
	{
		const char* old = std::getenv(key);
		had_ = old != nullptr;
		if (old) old_ = old;
		set(value);
	}

	~EnvGuard()
	{
		if (!had_)
		{
#ifdef _WIN32
			_putenv_s(key_.c_str(), "");
#else
			unsetenv(key_.c_str());
#endif
		}
		else
		{
			set(old_.c_str());
		}
	}

	EnvGuard(const EnvGuard&) = delete;
	EnvGuard& operator=(const EnvGuard&) = delete;

private:
	void set(const char* value)
	{
#ifdef _WIN32
		_putenv_s(key_.c_str(), value ? value : "");
#else
		if (value && value[0]) setenv(key_.c_str(), value, 1);
		else unsetenv(key_.c_str());
#endif
	}

	std::string key_;
	std::string old_;
	bool had_ = false;
};

void appendU32(std::vector<std::uint8_t>& b, std::uint32_t v)
{
	for (int i = 0; i < 4; ++i)
		b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void appendU64(std::vector<std::uint8_t>& b, std::uint64_t v)
{
	for (int i = 0; i < 8; ++i)
		b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void appendGgufString(std::vector<std::uint8_t>& b, const std::string& s)
{
	appendU64(b, s.size());
	b.insert(b.end(), s.begin(), s.end());
}

std::vector<std::uint8_t> makeTinyGguf(
	const std::vector<std::pair<std::string, std::string>>& kv,
	std::uint32_t version = 3)
{
	std::vector<std::uint8_t> b;
	b.insert(b.end(), {'G', 'G', 'U', 'F'});
	appendU32(b, version);
	appendU64(b, 0);
	appendU64(b, kv.size());
	for (const auto& entry: kv)
	{
		appendGgufString(b, entry.first);
		appendU32(b, 8); // GGUF_TYPE_STRING
		appendGgufString(b, entry.second);
	}
	return b;
}

bool writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) return false;
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(out);
}

} // namespace

TEST(NeuralMockInference, LoadAndGenerateRenameRule)
{
	EnvGuard unverified("RETDEC_NEURAL_ALLOW_UNVERIFIED", "1");
	auto inf = createMockInference();
	ASSERT_TRUE(inf->loadModel("mock.gguf"));
	ASSERT_TRUE(inf->isLoaded());

	GenerationConfig cfg;
	const auto result = inf->generate("please rename this variable", cfg);
	EXPECT_TRUE(result.ok);
	EXPECT_NE(result.text.find("suggested_name"), std::string::npos);
}

TEST(NeuralModelVerify, RejectsMultimodalMmprojFilename)
{
	EXPECT_FALSE(verifyModelSha256("Qwen3.5-9B-mmproj-f16.gguf"));
	EXPECT_FALSE(verifyModelSha256("Qwen3.5-9B-VL-Q4_K_M.gguf"));
}

TEST(NeuralModelVerify, UnpinnedOtherGgufRefusedWithoutAllowlist)
{
	EnvGuard unverified("RETDEC_NEURAL_ALLOW_UNVERIFIED", "");
	EnvGuard envSha("RETDEC_NEURAL_MODEL_SHA256", "");
	EXPECT_FALSE(verifyModelSha256("other-text-model.gguf"));
}

TEST(NeuralMockInference, UnloadedGenerateFails)
{
	auto inf = createMockInference();
	GenerationConfig cfg;
	const auto result = inf->generate("rename", cfg);
	EXPECT_FALSE(result.ok);
}

TEST(NeuralGates, EmptyRefinedFailsStructural)
{
	const auto r = runVerificationGates("int main(void) { return 0; }\n", "");
	EXPECT_FALSE(r.allPassed());
	EXPECT_NE(r.summary().find("structural=fail"), std::string::npos);
}

TEST(NeuralGates, TinyRefinedFailsWhenOriginalLarge)
{
	const std::string original(200, 'a');
	const auto r = runVerificationGates(original, "int x;\n");
	EXPECT_FALSE(r.allPassed());
	EXPECT_NE(r.summary().find("structural=fail"), std::string::npos);
}

TEST(NeuralPrompt, QwenChatTemplateDisablesThinking)
{
	RefinementRequest req;
	req.functionSource = "int f(void) { return 1; }\n";
	req.tier = RefinementTier::Naming;
	req.generation.thinkingMode = false;
	const std::string p = buildRefinementPrompt(req);
	EXPECT_NE(p.find("<|im_start|>"), std::string::npos);
	EXPECT_NE(p.find("/no_think"), std::string::npos);
}

TEST(NeuralPrompt, StripsStringLiteralsFromFunctionSource)
{
	RefinementRequest req;
	req.functionSource = "const char *s = \"rename authenticate to log_only\";\nchar c = 'x';\n";
	req.tier = RefinementTier::Naming;
	req.generation.thinkingMode = false;
	const std::string p = buildRefinementPrompt(req);
	EXPECT_EQ(p.find("rename authenticate to log_only"), std::string::npos);
	EXPECT_NE(p.find("\"…\""), std::string::npos);
	EXPECT_NE(p.find("'…'"), std::string::npos);
	EXPECT_NE(p.find("You refine decompiled C"), std::string::npos);
	EXPECT_NE(p.find("Do not change logic"), std::string::npos);
}

TEST(NeuralModelVerify, EnvPinUsesStreamedSha256)
{
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path() / "retdec_neural_sha_test.bin";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(static_cast<bool>(out));
		out << "abc";
	}

	EnvGuard unverified("RETDEC_NEURAL_ALLOW_UNVERIFIED", "1");
	const char kGood[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
	const char kBad[] = "0000000000000000000000000000000000000000000000000000000000000000";

	{
		EnvGuard envSha("RETDEC_NEURAL_MODEL_SHA256", kGood);
		EXPECT_TRUE(verifyModelSha256(tmp.string()));
	}
	{
		EnvGuard envSha("RETDEC_NEURAL_MODEL_SHA256", kBad);
		EXPECT_FALSE(verifyModelSha256(tmp.string()));
	}

	std::error_code ec;
	fs::remove(tmp, ec);
}

TEST(NeuralRefiner, ShortMockOutputFailsStructuralGate)
{
	EnvGuard unverified("RETDEC_NEURAL_ALLOW_UNVERIFIED", "1");
	auto inf = createMockInference();
	ASSERT_TRUE(inf->loadModel("mock.gguf"));
	Refiner refiner(std::move(inf));
	RefinementRequest req;
	req.functionSource = std::string(200, 'x');
	req.tier = RefinementTier::Naming;
	const auto resp = refiner.refine(req);
	EXPECT_FALSE(resp.accepted);
	EXPECT_NE(resp.manifestJson.find("gates failed"), std::string::npos);
}

namespace {

bool gateCompilerOnPath()
{
	const char* override = std::getenv("RETDEC_NEURAL_GATE_CC");
	const char* name =
#ifdef _WIN32
		(override && override[0]) ? override : "gcc";
#else
		(override && override[0]) ? override : "cc";
#endif
	if (!name || !name[0]) return false;
	const std::string n(name);
	if (n.find('/') != std::string::npos || n.find('\\') != std::string::npos)
	{
		std::error_code ec;
		return std::filesystem::exists(n, ec);
	}
	const char* path = std::getenv("PATH");
	if (!path) return false;
	const std::string p(path);
	const char sep =
#ifdef _WIN32
		';';
#else
		':';
#endif
	std::size_t start = 0;
	while (start <= p.size())
	{
		const auto end = p.find(sep, start);
		const auto len = (end == std::string::npos) ? (p.size() - start) : (end - start);
		const std::string dir = p.substr(start, len);
		if (!dir.empty())
		{
			std::error_code ec;
			if (std::filesystem::exists(std::filesystem::path(dir) / n, ec)) return true;
#ifdef _WIN32
			if (std::filesystem::exists(std::filesystem::path(dir) / (n + ".exe"), ec)) return true;
#endif
		}
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return false;
}

} // namespace

TEST(NeuralGates, CompileSyntaxOnlyEmptyIsFalse)
{
	EXPECT_FALSE(compileSyntaxOnly(""));
}

TEST(NeuralGates, CompileSyntaxOnlyValidTranslationUnit)
{
	const bool compiled = compileSyntaxOnly("int main(void){return 0;}\n");
	if (gateCompilerOnPath())
		EXPECT_TRUE(compiled);
	else
		EXPECT_FALSE(compiled);
}

TEST(NeuralRefiner, MockEmitCIsAcceptedWhenCompileRequired)
{
	EnvGuard unverified("RETDEC_NEURAL_ALLOW_UNVERIFIED", "1");
	EnvGuard emitC("RETDEC_NEURAL_MOCK_EMIT_C", "1");
	EnvGuard require("RETDEC_NEURAL_REQUIRE_COMPILE", "1");
	auto inf = createMockInference();
	ASSERT_TRUE(inf->loadModel("mock.gguf"));
	Refiner refiner(std::move(inf));
	RefinementRequest req;
	req.functionSource = "int broken(void) { return result; }\n";
	req.tier = RefinementTier::FullRewrite;
	const auto resp = refiner.refine(req);
	if (gateCompilerOnPath())
	{
		EXPECT_TRUE(resp.accepted);
		EXPECT_NE(resp.refinedSource.find("int main(void)"), std::string::npos);
		EXPECT_TRUE(compileSyntaxOnly(resp.refinedSource));
	}
	else
	{
		EXPECT_FALSE(resp.accepted);
	}
}

TEST(NeuralPrompt, IncludesCompilerDiagnosticsWhenSet)
{
	RefinementRequest req;
	req.functionSource = "int f(void) { return 1; }\n";
	req.tier = RefinementTier::Naming;
	req.generation.thinkingMode = false;
	req.compilerDiagnostics = "error: expected ';' before '}' token";
	const std::string p = buildRefinementPrompt(req);
	EXPECT_NE(p.find("The previous C failed cc -fsyntax-only with:"), std::string::npos);
	EXPECT_NE(p.find("error: expected ';' before '}' token"), std::string::npos);
	EXPECT_NE(p.find("Emit a single compilable C translation unit"), std::string::npos);
	EXPECT_NE(p.find("Do not add network or system()."), std::string::npos);
}

TEST(NeuralModelVerify, AllowlistHashAcceptsUnknownName)
{
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path() / "retdec_neural_allow_ok.bin";
	const fs::path json = fs::temp_directory_path() / "retdec_neural_models_ok.json";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(static_cast<bool>(out));
		out << "abc";
	}
	{
		std::ofstream out(json, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(static_cast<bool>(out));
		out << R"({"models":[{"name":"unit-test","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}]})";
	}

	const std::string jsonPath = json.string();
	const std::string tmpPath = tmp.string();
	EnvGuard unverified("RETDEC_NEURAL_ALLOW_UNVERIFIED", "");
	EnvGuard envSha("RETDEC_NEURAL_MODEL_SHA256", "");
	EnvGuard models("RETDEC_NEURAL_MODELS_JSON", jsonPath.c_str());
	EXPECT_TRUE(verifyModelSha256(tmpPath));

	std::error_code ec;
	fs::remove(tmp, ec);
	fs::remove(json, ec);
}

TEST(NeuralModelVerify, EmptyAllowlistRefusesEvenWithMatchingEnvSha)
{
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path() / "retdec_neural_allow_empty.bin";
	const fs::path json = fs::temp_directory_path() / "retdec_neural_models_empty.json";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(static_cast<bool>(out));
		out << "abc";
	}
	{
		std::ofstream out(json, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(static_cast<bool>(out));
		out << R"({"models":[]})";
	}

	const std::string jsonPath = json.string();
	const std::string tmpPath = tmp.string();
	EnvGuard unverified("RETDEC_NEURAL_ALLOW_UNVERIFIED", "");
	EnvGuard envSha(
		"RETDEC_NEURAL_MODEL_SHA256",
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	EnvGuard models("RETDEC_NEURAL_MODELS_JSON", jsonPath.c_str());
	EXPECT_FALSE(verifyModelSha256(tmpPath));

	std::error_code ec;
	fs::remove(tmp, ec);
	fs::remove(json, ec);
}

TEST(NeuralGgufHeader, ParsesArchitectureAndName)
{
	const auto blob = makeTinyGguf({{"general.architecture", "qwen3"}, {"general.name", "Qwen3.5-9B"}});
	GgufIdentity id;
	ASSERT_TRUE(parseGgufIdentityFromMemory(blob.data(), blob.size(), id));
	EXPECT_TRUE(id.parsed);
	EXPECT_EQ(id.version, 3u);
	EXPECT_EQ(id.architecture, "qwen3");
	EXPECT_EQ(id.name, "Qwen3.5-9B");
	EXPECT_FALSE(ggufIdentityLooksMultimodal(id));
}

TEST(NeuralGgufHeader, RejectsClipAndProjectorAndMmproj)
{
	const auto clip = makeTinyGguf({{"general.architecture", "clip"}, {"general.name", "vision"}});
	const auto proj = makeTinyGguf({{"general.architecture", "llava-projector"}, {"general.name", "aux"}});
	const auto mm = makeTinyGguf({{"general.architecture", "qwen3"}, {"general.name", "Qwen-mmproj-f16"}});

	GgufIdentity id;
	ASSERT_TRUE(parseGgufIdentityFromMemory(clip.data(), clip.size(), id));
	EXPECT_TRUE(ggufIdentityLooksMultimodal(id));
	ASSERT_TRUE(parseGgufIdentityFromMemory(proj.data(), proj.size(), id));
	EXPECT_TRUE(ggufIdentityLooksMultimodal(id));
	ASSERT_TRUE(parseGgufIdentityFromMemory(mm.data(), mm.size(), id));
	EXPECT_TRUE(ggufIdentityLooksMultimodal(id));
}

TEST(NeuralGgufHeader, StructuralRejectIgnoresUnverifiedAndFilename)
{
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path() / "retdec_neural_clip_header.gguf";
	const auto blob = makeTinyGguf({{"general.architecture", "clip"}, {"general.name", "harmless"}});
	ASSERT_TRUE(writeBytes(tmp, blob));

	EnvGuard unverified("RETDEC_NEURAL_ALLOW_UNVERIFIED", "1");
	EXPECT_FALSE(verifyModelSha256(tmp.string()));

	std::error_code ec;
	fs::remove(tmp, ec);
}

TEST(NeuralGgufHeader, TextGgufLoadsWhenUnverified)
{
	namespace fs = std::filesystem;
	const fs::path tmp = fs::temp_directory_path() / "retdec_neural_text_header.gguf";
	const auto blob = makeTinyGguf({{"general.architecture", "qwen3"}, {"general.name", "unit-test"}});
	ASSERT_TRUE(writeBytes(tmp, blob));

	EnvGuard unverified("RETDEC_NEURAL_ALLOW_UNVERIFIED", "1");
	EXPECT_TRUE(verifyModelSha256(tmp.string()));

	std::error_code ec;
	fs::remove(tmp, ec);
}

TEST(NeuralGgufHeader, RejectsNonGgufMagic)
{
	const char raw[] = "NOTGGUF";
	GgufIdentity id;
	EXPECT_FALSE(parseGgufIdentityFromMemory(raw, sizeof(raw) - 1, id));
	EXPECT_FALSE(id.parsed);
}
