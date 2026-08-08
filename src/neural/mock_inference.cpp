#include "retdec/neural/inference.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace retdec::neural {

namespace {

struct MockRule {
    std::string substring;
    std::string response;
};

const std::vector<MockRule>& mockRules() {
    static const std::vector<MockRule> rules = {
        {"rename", "suggested_name = recovered_value"},
        {"comment", "// Recovered loop over buffer"},
        {"struct", "field_count"},
    };
    return rules;
}

} // namespace

class MockInference : public Inference {
public:
    bool loadModel(const std::string& /*ggufPath*/, int /*contextLen*/) override {
        loaded_ = true;
        return true;
    }

    void unloadModel() override { loaded_ = false; }

    bool isLoaded() const override { return loaded_; }

    GenerationResult generate(const std::string& prompt,
                              const GenerationConfig& /*config*/) override {
        GenerationResult result;
        if (!loaded_) {
            result.error = "mock: model not loaded";
            return result;
        }
        for (const auto& rule : mockRules()) {
            if (prompt.find(rule.substring) != std::string::npos) {
                result.text = rule.response;
                result.tokensGenerated = static_cast<int>(rule.response.size());
                result.ok = true;
                return result;
            }
        }
        result.text = "mock: no rule matched";
        result.tokensGenerated = 1;
        result.ok = true;
        return result;
    }

private:
    bool loaded_ = false;
};

std::unique_ptr<Inference> createMockInference() {
    return std::make_unique<MockInference>();
}

std::unique_ptr<Inference> createLlamaInference() {
    return nullptr; // Implemented when RETDEC_ENABLE_NEURAL links llama.cpp
}

} // namespace retdec::neural
