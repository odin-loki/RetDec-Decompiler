/**
 * @file tests/gui/helpers/fake_decompiler.cpp
 * @brief A stand-in for retdec-decompiler (tests/gui/main_window_test.cpp),
 *        compiled so it runs the same way on every platform (a shell script
 *        cannot be spawned as "retdec-decompiler.exe" on Windows). Reads
 *        "<binary> -o <out.c> ...", logs like the real one, then writes the
 *        sidecars of tests/gui/data/main_window/fixture.* next to <out.c>.
 *
 *   FAKE_DECOMPILER_EXIT=N    exit with N (no output written when N != 0)
 *   FAKE_DECOMPILER_SLEEP=S   sleep S seconds before finishing (stop / cancel tests)
 *   FAKE_DECOMPILER_ARGS=F    append the argument vector, one per line, to file F
 *   FAKE_DECOMPILER_FIXTURE=D the fixture directory (default: the test data dir)
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace {

std::string env(const char* name)
{
	const char* v = std::getenv(name);
	return v ? std::string(v) : std::string{};
}

bool copyFixture(const fs::path& from, const fs::path& to)
{
	std::error_code ec;
	fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
	if (ec) std::cout << "Error: cannot copy " << from.string() << ": " << ec.message() << std::endl;
	return !ec;
}

} // namespace

int main(int argc, char** argv)
{
	std::string fixtureDir = env("FAKE_DECOMPILER_FIXTURE");
	if (fixtureDir.empty()) fixtureDir = RETDEC_GUI_FAKE_DECOMPILER_FIXTURE;
	const fs::path fixture(fixtureDir);

	std::string out;
	for (int i = 1; i + 1 < argc; ++i)
		if (std::string(argv[i]) == "-o") out = argv[i + 1];

	if (const std::string argsFile = env("FAKE_DECOMPILER_ARGS"); !argsFile.empty())
	{
		// Binary: the tests look for "\n"-separated pairs such as "-m\nraw".
		std::ofstream f(argsFile, std::ios::binary | std::ios::app);
		for (int i = 1; i < argc; ++i)
			f << argv[i] << '\n';
	}

	std::cout << "Running phase: Initialization ( 0.01s )\n"
				 "Running phase: Input binary to LLVM IR decoding ( 0.02s )\n"
				 " -> stack ( 0.03s )\n"
				 "Warning: unresolved indirect jump at 0x401020\n"
				 " -> simple types ( 0.04s )\n"
				 "[progress] code generation 50%"
			  << std::endl;

	if (const std::string sleep = env("FAKE_DECOMPILER_SLEEP"); !sleep.empty())
		std::this_thread::sleep_for(std::chrono::duration<double>(std::atof(sleep.c_str())));

	const int code = std::atoi(env("FAKE_DECOMPILER_EXIT").c_str());
	if (code != 0)
	{
		std::cout << "Error: fake decompiler failure" << std::endl;
		return code;
	}
	if (out.empty())
	{
		std::cout << "Error: no -o given" << std::endl;
		return 2;
	}

	const fs::path outC(out);
	fs::path base = outC;
	if (base.extension() == ".c") base.replace_extension();
	const auto sidecar = [&](const char* ext) { return fs::path(base.string() + ext); };
	if (!copyFixture(fixture / "fixture.c", outC)
		|| !copyFixture(fixture / "fixture.config.json", sidecar(".config.json"))
		|| !copyFixture(fixture / "fixture.dsm", sidecar(".dsm"))
		|| !copyFixture(fixture / "fixture.ll", sidecar(".ll")))
		return 3;
	std::cout << "Done." << std::endl;
	return 0;
}
