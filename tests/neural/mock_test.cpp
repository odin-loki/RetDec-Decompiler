#include "retdec/neural/gates.h"
#include "retdec/neural/inference.h"
#include "retdec/neural/model_verify.h"
#include "retdec/neural/refiner.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

using namespace retdec::neural;

TEST(NeuralMockInference, LoadAndGenerateRenameRule)
{
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

TEST(NeuralModelVerify, UnpinnedOtherGgufPassesWithoutEnv)
{
	EXPECT_TRUE(verifyModelSha256("other-text-model.gguf"));
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

	const char* old = std::getenv("RETDEC_NEURAL_MODEL_SHA256");
	const std::string saved = old ? old : "";
	const char kGood[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
	const char kBad[] = "0000000000000000000000000000000000000000000000000000000000000000";

#ifdef _WIN32
	ASSERT_EQ(_putenv_s("RETDEC_NEURAL_MODEL_SHA256", kGood), 0);
#else
	ASSERT_EQ(setenv("RETDEC_NEURAL_MODEL_SHA256", kGood, 1), 0);
#endif
	EXPECT_TRUE(verifyModelSha256(tmp.string()));

#ifdef _WIN32
	ASSERT_EQ(_putenv_s("RETDEC_NEURAL_MODEL_SHA256", kBad), 0);
#else
	ASSERT_EQ(setenv("RETDEC_NEURAL_MODEL_SHA256", kBad, 1), 0);
#endif
	EXPECT_FALSE(verifyModelSha256(tmp.string()));

	if (saved.empty())
	{
#ifdef _WIN32
		_putenv("RETDEC_NEURAL_MODEL_SHA256=");
#else
		unsetenv("RETDEC_NEURAL_MODEL_SHA256");
#endif
	}
	else
	{
#ifdef _WIN32
		_putenv_s("RETDEC_NEURAL_MODEL_SHA256", saved.c_str());
#else
		setenv("RETDEC_NEURAL_MODEL_SHA256", saved.c_str(), 1);
#endif
	}

	std::error_code ec;
	fs::remove(tmp, ec);
}

TEST(NeuralRefiner, ShortMockOutputFailsStructuralGate)
{
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
