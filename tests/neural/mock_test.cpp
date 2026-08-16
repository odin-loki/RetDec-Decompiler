#include "retdec/neural/inference.h"
#include "retdec/neural/model_verify.h"

#include <gtest/gtest.h>

using namespace retdec::neural;

TEST(NeuralMockInference, LoadAndGenerateRenameRule) {
    auto inf = createMockInference();
    ASSERT_TRUE(inf->loadModel("mock.gguf"));
    ASSERT_TRUE(inf->isLoaded());

    GenerationConfig cfg;
    const auto result = inf->generate("please rename this variable", cfg);
    EXPECT_TRUE(result.ok);
    EXPECT_NE(result.text.find("suggested_name"), std::string::npos);
}

TEST(NeuralModelVerify, RejectsMultimodalMmprojFilename) {
    EXPECT_FALSE(verifyModelSha256("Qwen3.5-9B-mmproj-f16.gguf"));
    EXPECT_FALSE(verifyModelSha256("Qwen3.5-9B-VL-Q4_K_M.gguf"));
}

TEST(NeuralMockInference, UnloadedGenerateFails) {
    auto inf = createMockInference();
    GenerationConfig cfg;
    const auto result = inf->generate("rename", cfg);
    EXPECT_FALSE(result.ok);
}
