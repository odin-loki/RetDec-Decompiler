#include "retdec/neural/refiner.h"
#include "retdec/neural/gates.h"

#include <cstdio>
#include <string>

namespace retdec::neural {

std::string buildRefinementPrompt(const RefinementRequest& request);

namespace {

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

} // namespace

Refiner::Refiner(std::unique_ptr<Inference> backend): inference_(std::move(backend)) {}

RefinementResponse Refiner::refine(const RefinementRequest& request) const
{
	RefinementResponse response;
	if (!inference_ || !inference_->isLoaded())
	{
		response.refinedSource = request.functionSource;
		response.accepted = false;
		response.manifestJson = R"({"accepted":false,"reason":"no backend"})";
		return response;
	}

	std::string prompt = buildRefinementPrompt(request);

	const auto gen = inference_->generate(prompt, request.generation);
	if (!gen.ok)
	{
		response.refinedSource = request.functionSource;
		response.accepted = false;
		response.manifestJson = R"({"accepted":false,"reason":"generation failed"})";
		std::fprintf(stderr, "retdec-neural: generation failed: %s\n", gen.error.c_str());
		return response;
	}

	const std::string refined = stripMarkdownFences(gen.text);
	const auto gates = runVerificationGates(request.functionSource, refined);
	if (!gates.allPassed())
	{
		response.refinedSource = request.functionSource;
		response.accepted = false;
		response.manifestJson =
			std::string(R"({"accepted":false,"reason":"gates failed","detail":")") + gates.summary() + "\"}";
		std::fprintf(stderr, "retdec-neural: gates failed (%s)\n", gates.summary().c_str());
		return response;
	}

	response.refinedSource = refined;
	response.accepted = true;
	response.manifestJson = R"({"accepted":true,"tier":)" + std::to_string(static_cast<int>(request.tier)) + "}";
	return response;
}

} // namespace retdec::neural
