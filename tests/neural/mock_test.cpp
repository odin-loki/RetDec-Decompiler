#include "retdec/neural/gates.h"
#include "retdec/neural/inference.h"
#include "retdec/neural/model_verify.h"
#include "retdec/neural/refiner.h"

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
