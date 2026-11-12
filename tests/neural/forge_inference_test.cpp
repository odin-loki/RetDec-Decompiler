/**
 * @file tests/neural/forge_inference_test.cpp
 * @brief The Forge inference backend, end to end, on a tiny model.
 *
 * tests/neural/data/tiny_f32.fpk is Forge's golden test model: a two-layer
 * Llama with random weights (seed 1234) and a byte-level BPE tokenizer of 300
 * tokens, imported from third_party/forge/verify/golden/tiny_llama.py with
 * `forge import --codec f32` (459 KB). Its text is meaningless; what is
 * checked is that the pipeline runs and that every GenerationConfig field
 * reaches the decoder.
 */

#include "retdec/neural/inference.h"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#ifndef RETDEC_NEURAL_TEST_DATA_DIR
#error "RETDEC_NEURAL_TEST_DATA_DIR must name tests/neural/data"
#endif

namespace retdec::neural {
namespace {

namespace fs = std::filesystem;

const std::string kModel = std::string(RETDEC_NEURAL_TEST_DATA_DIR) + "/tiny_f32.fpk";

void setEnv(const char* name, const char* value)
{
#if defined(_WIN32)
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name)
{
#if defined(_WIN32)
	_putenv_s(name, "");
#else
	unsetenv(name);
#endif
}

class ForgeInferenceTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		// The fixture is not in support/models.json; the allowlist is
		// model_verify's business and has tests of its own.
		setEnv("RETDEC_NEURAL_ALLOW_UNVERIFIED", "1");
		backend = createForgeInference();
		ASSERT_TRUE(backend != nullptr);
		ASSERT_TRUE(backend->loadModel(kModel, 128));
		ASSERT_TRUE(backend->isLoaded());
	}

	void TearDown() override
	{
		unsetEnv("RETDEC_NEURAL_ALLOW_UNVERIFIED");
	}

	static GenerationConfig greedy(int maxTokens)
	{
		GenerationConfig c;
		c.temperature = 0.0f;
		c.maxTokens = maxTokens;
		return c;
	}

	std::unique_ptr<Inference> backend;
};

TEST_F(ForgeInferenceTest, GeneratesTokens)
{
	const auto r = backend->generate("the theory of things", greedy(8));
	ASSERT_TRUE(r.ok);
	EXPECT_TRUE(r.error.empty());
	EXPECT_GT(r.tokensGenerated, 0);
	EXPECT_LE(r.tokensGenerated, 8);
	EXPECT_FALSE(r.text.empty());
	// Greedy: every selected token had probability 1 under the chain.
	EXPECT_TRUE(r.hasTokenProb);
	EXPECT_GT(r.meanTokenProb, 0.999f);
}

TEST_F(ForgeInferenceTest, GreedyIsDeterministic)
{
	const auto a = backend->generate("Hello, the w", greedy(12));
	const auto b = backend->generate("Hello, the w", greedy(12));
	ASSERT_TRUE(a.ok);
	ASSERT_TRUE(b.ok);
	EXPECT_EQ(a.text, b.text);
}

TEST_F(ForgeInferenceTest, SeededSamplingIsReproducibleAndReportsProbability)
{
	GenerationConfig c;
	c.temperature = 0.9f;
	c.topK = 40;
	c.topP = 0.95f;
	c.minP = 0.01f;
	c.maxTokens = 16;
	c.seed = 7;
	const auto a = backend->generate("on the", c);
	const auto b = backend->generate("on the", c);
	ASSERT_TRUE(a.ok);
	ASSERT_TRUE(b.ok);
	EXPECT_EQ(a.text, b.text);
	ASSERT_TRUE(a.hasTokenProb);
	EXPECT_GT(a.meanTokenProb, 0.0f);
	EXPECT_LE(a.meanTokenProb, 1.0f);
}

TEST_F(ForgeInferenceTest, TopKOneMatchesGreedy)
{
	GenerationConfig c;
	c.temperature = 0.8f;
	c.topK = 1;
	c.maxTokens = 10;
	c.seed = 99;
	const auto sampled = backend->generate("the theory", c);
	const auto argmax = backend->generate("the theory", greedy(10));
	ASSERT_TRUE(sampled.ok);
	ASSERT_TRUE(argmax.ok);
	EXPECT_EQ(sampled.text, argmax.text);
}

TEST_F(ForgeInferenceTest, MaxTokensBoundsOutput)
{
	const auto r = backend->generate("the", greedy(3));
	ASSERT_TRUE(r.ok);
	EXPECT_LE(r.tokensGenerated, 3);
}

// The KV rows of a shared prompt prefix are kept between calls; the result
// has to be what a fresh session would have produced.
TEST_F(ForgeInferenceTest, KvPrefixReuseMatchesAFreshSession)
{
	GenerationConfig reuse = greedy(10);
	reuse.reuseKvPrefix = true;
	const auto first = backend->generate("the theory of things on the", reuse);
	const auto reused = backend->generate("the theory of things in the", reuse);
	ASSERT_TRUE(first.ok);
	ASSERT_TRUE(reused.ok);

	auto fresh = createForgeInference();
	ASSERT_TRUE(fresh->loadModel(kModel, 128));
	const auto expected = fresh->generate("the theory of things in the", greedy(10));
	ASSERT_TRUE(expected.ok);
	EXPECT_EQ(reused.text, expected.text);
}

TEST_F(ForgeInferenceTest, PromptOverContextIsRefused)
{
	std::string prompt;
	for (int i = 0; i < 200; ++i)
		prompt += "the ";
	const auto r = backend->generate(prompt, greedy(8));
	EXPECT_FALSE(r.ok);
	// refiner.cpp retries with a truncated source on exactly this text (N11).
	EXPECT_NE(r.error.find("prompt exceeds context budget"), std::string::npos);
}

// The grammar has to win over a model with random weights: whatever it would
// have said, the output is the one JSON object shape the grammar allows.
TEST_F(ForgeInferenceTest, GrammarConstrainedOutputParses)
{
	GenerationConfig c;
	c.temperature = 0.7f;
	c.seed = 3;
	c.maxTokens = 32;
	c.grammarGbnf = R"gbnf(
root  ::= "{" ws pair ws "}"
pair  ::= ident ws ":" ws ident
ident ::= "\"" [a-z] [a-z0-9_]? [a-z0-9_]? [a-z0-9_]? "\""
ws    ::= [ ]?
)gbnf";
	for (unsigned seed: {1u, 2u, 3u})
	{
		c.seed = seed;
		const auto r = backend->generate("rename the variables", c);
		ASSERT_TRUE(r.ok);
		rapidjson::Document doc;
		doc.Parse(r.text.c_str());
		ASSERT_FALSE(doc.HasParseError());
		ASSERT_TRUE(doc.IsObject());
		EXPECT_EQ(doc.MemberCount(), 1u);
		for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it)
		{
			EXPECT_TRUE(it->value.IsString());
		}
	}
}

TEST_F(ForgeInferenceTest, NonDefaultGrammarRootIsUsed)
{
	GenerationConfig c = greedy(8);
	c.grammarGbnf = "answer ::= \"yes\" | \"no\"\n";
	c.grammarRoot = "answer";
	const auto r = backend->generate("is it", c);
	ASSERT_TRUE(r.ok);
	EXPECT_TRUE(r.text == "yes" || r.text == "no");
}

TEST_F(ForgeInferenceTest, BadGrammarFallsBackToUnconstrained)
{
	GenerationConfig c = greedy(4);
	c.grammarGbnf = "root ::= undefined_rule\n";
	const auto r = backend->generate("the", c);
	EXPECT_TRUE(r.ok);
}

TEST_F(ForgeInferenceTest, UnloadedGenerateFails)
{
	backend->unloadModel();
	EXPECT_FALSE(backend->isLoaded());
	const auto r = backend->generate("the", greedy(4));
	EXPECT_FALSE(r.ok);
	EXPECT_FALSE(r.error.empty());
}

TEST(ForgeInferenceLoad, MissingModelFailsCleanly)
{
	setEnv("RETDEC_NEURAL_ALLOW_UNVERIFIED", "1");
	auto backend = createForgeInference();
	EXPECT_FALSE(backend->loadModel(std::string(RETDEC_NEURAL_TEST_DATA_DIR) + "/no_such_model.fpk", 128));
	EXPECT_FALSE(backend->isLoaded());
	unsetEnv("RETDEC_NEURAL_ALLOW_UNVERIFIED");
}

TEST(ForgeInferenceLoad, UnverifiedModelIsRefusedWithoutOptIn)
{
	unsetEnv("RETDEC_NEURAL_ALLOW_UNVERIFIED");
	unsetEnv("RETDEC_NEURAL_MODEL_SHA256");
	setEnv("RETDEC_NEURAL_MODELS_JSON", (std::string(RETDEC_NEURAL_TEST_DATA_DIR) + "/no_such_models.json").c_str());
	auto backend = createForgeInference();
	EXPECT_FALSE(backend->loadModel(kModel, 128));
	unsetEnv("RETDEC_NEURAL_MODELS_JSON");
}

// A Hugging Face directory is imported once into $FORGE_CACHE (the name
// `forge run` would use) and then loaded from there. The directory comes
// from tests/neural/make_tiny_hf.py, run by a ctest fixture when numpy and
// safetensors are installed.
TEST(ForgeInferenceLoad, HuggingFaceDirectoryIsImportedAndCached)
{
	const char* hf = std::getenv("RETDEC_NEURAL_TEST_HF_DIR");
	if (!hf || !hf[0] || !fs::is_directory(hf))
		GTEST_SKIP() << "RETDEC_NEURAL_TEST_HF_DIR not set (needs python3 with numpy and safetensors)";

	const fs::path cache = fs::temp_directory_path() / "retdec-neural-forge-cache-test";
	std::error_code ec;
	fs::remove_all(cache, ec);
	setEnv("FORGE_CACHE", cache.string().c_str());
	setEnv("RETDEC_NEURAL_ALLOW_UNVERIFIED", "1");

	auto backend = createForgeInference();
	ASSERT_TRUE(backend->loadModel(hf, 128));
	GenerationConfig c;
	c.temperature = 0.0f;
	c.maxTokens = 6;
	const auto r = backend->generate("the theory of things", c);
	EXPECT_TRUE(r.ok);
	EXPECT_GT(r.tokensGenerated, 0);

	std::size_t imported = 0;
	for (const auto& e: fs::directory_iterator(cache))
		if (e.path().extension() == ".fpk") ++imported;
	EXPECT_EQ(imported, 1u);

	// Second load reuses the cached import.
	auto again = createForgeInference();
	ASSERT_TRUE(again->loadModel(hf, 128));
	const auto r2 = again->generate("the theory of things", c);
	EXPECT_EQ(r2.text, r.text);
	imported = 0;
	for (const auto& e: fs::directory_iterator(cache))
		if (e.path().extension() == ".fpk") ++imported;
	EXPECT_EQ(imported, 1u);

	unsetEnv("FORGE_CACHE");
	unsetEnv("RETDEC_NEURAL_ALLOW_UNVERIFIED");
	fs::remove_all(cache, ec);
}

} // namespace
} // namespace retdec::neural
