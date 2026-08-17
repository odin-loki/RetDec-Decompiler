#include "retdec/neural/model_verify.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#if defined(_MSC_VER)
#define popen _popen
#define pclose _pclose
#endif

namespace retdec::neural {

namespace {

// SHA-256 of the Unsloth llama.cpp-native Q4_K_M (5.28 GiB).
// Ollama qwen3.5:9b blobs fail on b10451 (rope.dimension_sections length 3).
constexpr const char* kQwen35Q4KmSha256 = "03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8";

std::string fileBasename(const std::string& path)
{
	const auto slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool namesMatchHint(const std::string& path)
{
	auto name = fileBasename(path);
	const char* hints[] = {
		kQwen35TextOnlyGgufHint,
		"Qwen3.5-9B-Q4_K_M.gguf",
		"Qwen3.5-9B-Q4_K_M.unsloth.gguf",
	};
	for (const char* hint: hints)
	{
		const std::string h = hint;
		if (name.size() != h.size()) continue;
		bool ok = true;
		for (std::size_t i = 0; i < name.size(); ++i)
		{
			const auto a = static_cast<unsigned char>(name[i]);
			const auto b = static_cast<unsigned char>(h[i]);
			if (std::tolower(a) != std::tolower(b))
			{
				ok = false;
				break;
			}
		}
		if (ok) return true;
	}
	return false;
}

std::string sha256HexOfFile(const std::string& path)
{
	std::string cmd = "sha256sum \"" + path + "\"";
#if defined(_WIN32)
	cmd = "certutil -hashfile \"" + path + "\" SHA256";
#endif
	FILE* pipe = popen(cmd.c_str(), "r");
	if (!pipe) return {};

	char buffer[256];
	std::string output;
	while (fgets(buffer, sizeof(buffer), pipe))
		output += buffer;
	pclose(pipe);

#if defined(_WIN32)
	// certutil: take hex line after header
	std::istringstream iss(output);
	std::string line;
	while (std::getline(iss, line))
	{
		if (line.size() >= 64)
		{
			std::string hex;
			for (char c: line)
			{
				if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
					hex += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			if (hex.size() >= 64) return hex.substr(0, 64);
		}
	}
	return {};
#else
	std::istringstream iss(output);
	std::string hex;
	iss >> hex;
	return hex;
#endif
}

} // namespace

bool verifyModelSha256(const std::string& modelPath)
{
	auto lower = modelPath;
	for (char& c: lower)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (lower.find("mmproj") != std::string::npos || lower.find("-vl-") != std::string::npos
		|| lower.find("_vl_") != std::string::npos)
		return false;

	const char* envSha = std::getenv("RETDEC_NEURAL_MODEL_SHA256");
	const bool haveEnv = envSha && envSha[0];
	const bool pinDefault = !haveEnv && namesMatchHint(modelPath);
	if (!haveEnv && !pinDefault) return true;

	const std::string actual = sha256HexOfFile(modelPath);
	if (actual.empty()) return !haveEnv;

	const char* expected = haveEnv ? envSha : kQwen35Q4KmSha256;

	std::string exp(expected);
	for (char& c: exp)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

	if (actual != exp)
	{
		std::fprintf(
			stderr,
			"retdec-neural: SHA-256 mismatch for %s\n"
			"  expected %s\n"
			"  actual   %s\n",
			modelPath.c_str(),
			exp.c_str(),
			actual.c_str());
		return false;
	}
	return true;
}

} // namespace retdec::neural
