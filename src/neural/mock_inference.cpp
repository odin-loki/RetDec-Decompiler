#include "retdec/neural/inference.h"
#include "retdec/neural/model_verify.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace retdec::neural {

namespace {

struct MockRule
{
	std::string substring;
	std::string response;
};

const std::vector<MockRule>& mockRules()
{
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
	bool loadModel(const std::string& ggufPath, int /*contextLen*/) override
	{
		if (!verifyModelSha256(ggufPath)) return false;
		loaded_ = true;
		return true;
	}

	void unloadModel() override
	{
		loaded_ = false;
	}

	bool isLoaded() const override
	{
		return loaded_;
	}

	GenerationResult generate(const std::string& prompt, const GenerationConfig& /*config*/) override
	{
		GenerationResult result;
		if (!loaded_)
		{
			result.error = "mock: model not loaded";
			return result;
		}
		for (const auto& rule: mockRules())
		{
			if (prompt.find(rule.substring) != std::string::npos)
			{
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

std::unique_ptr<Inference> createMockInference()
{
	return std::make_unique<MockInference>();
}

#ifndef RETDEC_HAS_LLAMACPP
std::unique_ptr<Inference> createLlamaInference()
{
	return nullptr; // Implemented in llama_inference.cpp when llama.cpp is linked
}
#endif

} // namespace retdec::neural
