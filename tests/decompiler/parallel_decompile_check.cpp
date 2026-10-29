/**
 * @file tests/decompiler/parallel_decompile_check.cpp
 * @brief Concurrent decompilations must produce what serial ones produce.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 *
 * Usage: parallel_decompile_check CONFIG WORKDIR COPIES JOBS INPUT...
 *
 * Decompiles every INPUT once on its own, then COPIES of each at once through
 * parallelBatchDecompile() on JOBS threads, and fails unless every concurrent
 * output is byte-for-byte the serial one. The bin2llvmir providers used to be
 * cleared process-wide by each pipeline, so concurrent jobs destroyed each
 * other's Config and FileImage; under ThreadSanitizer this also checks that
 * the per-module provider tables are race-free.
 */

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <llvm/Support/ManagedStatic.h>

#include "retdec/config/config.h"
#include "retdec/retdec/retdec.h"

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p)
{
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

retdec::config::Config jobConfig(
		const retdec::config::Config& base,
		const fs::path& input,
		const fs::path& dir)
{
	fs::create_directories(dir);
	auto in = dir / input.filename();
	fs::copy_file(input, in, fs::copy_options::overwrite_existing);
	auto out = (dir / input.filename()).string() + ".c";

	retdec::config::Config c = base;
	auto& p = c.parameters;
	p.setInputFile(in.string());
	p.setOutputFile(out);
	p.setOutputAsmFile(out + ".dsm");
	p.setOutputBitcodeFile(out + ".bc");
	p.setOutputLlvmirFile(out + ".ll");
	p.setOutputConfigFile(out + ".config.json");
	p.setOutputUnpackedFile(out + "-unpacked");
	p.setLogFile((dir / "log.txt").string());
	p.setErrFile((dir / "err.txt").string());
	p.setIsBackendNoTimeVaryingInfo(true);
	return c;
}

} // namespace

int main(int argc, char** argv)
{
	llvm::llvm_shutdown_obj shutdown;

	if (argc < 6)
	{
		std::cerr << "usage: " << argv[0] << " CONFIG WORKDIR COPIES JOBS INPUT...\n";
		return 2;
	}
	const fs::path configPath = argv[1];
	const fs::path work = argv[2];
	const int copies = std::stoi(argv[3]);
	const int jobs = std::stoi(argv[4]);
	std::vector<fs::path> inputs(argv + 5, argv + argc);

	auto base = retdec::config::Config::fromFile(configPath.string());
	base.parameters.fixRelativePaths(fs::canonical(configPath).parent_path().string());

	fs::remove_all(work);

	using clock = std::chrono::steady_clock;

	std::vector<std::string> expected;
	const auto s0 = clock::now();
	for (std::size_t i = 0; i < inputs.size(); ++i)
	{
		auto c = jobConfig(base, inputs[i], work / ("serial-" + std::to_string(i)));
		if (!retdec::decompile(c))
		{
			std::cerr << "serial decompilation failed: " << inputs[i] << "\n";
			return 1;
		}
		expected.push_back(readFile(c.parameters.getOutputFile()));
		if (expected.back().empty())
		{
			std::cerr << "serial decompilation wrote nothing: " << inputs[i] << "\n";
			return 1;
		}
	}
	const auto serialMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - s0).count();

	std::vector<retdec::config::Config> configs;
	std::vector<std::size_t> which;
	for (int k = 0; k < copies; ++k)
	{
		for (std::size_t i = 0; i < inputs.size(); ++i)
		{
			auto dir = work / ("parallel-" + std::to_string(k) + "-" + std::to_string(i));
			configs.push_back(jobConfig(base, inputs[i], dir));
			which.push_back(i);
		}
	}

	const auto p0 = clock::now();
	auto ok = retdec::parallelBatchDecompile(configs, static_cast<std::size_t>(jobs));
	const auto parallelMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - p0).count();

	int failures = 0;
	for (std::size_t j = 0; j < configs.size(); ++j)
	{
		const auto& out = configs[j].parameters.getOutputFile();
		if (!ok[j])
		{
			std::cerr << "job " << j << " failed: " << out << "\n";
			++failures;
			continue;
		}
		if (readFile(out) != expected[which[j]])
		{
			std::cerr << "job " << j << " differs from the serial output: " << out << "\n";
			++failures;
		}
	}

	std::cout << "serial: " << inputs.size() << " jobs in " << serialMs << " ms; "
			  << "parallel: " << configs.size() << " jobs on " << jobs << " threads in "
			  << parallelMs << " ms\n";
	if (failures)
	{
		std::cerr << failures << " of " << configs.size() << " concurrent jobs went wrong\n";
		return 1;
	}
	std::cout << "all " << configs.size() << " concurrent outputs match\n";
	return 0;
}
