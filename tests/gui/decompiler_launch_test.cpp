/**
 * @file tests/gui/decompiler_launch_test.cpp
 */

#include "retdec/gui/model/decompiler_launch.h"
#include "retdec/gui/model/settings.h"

#include <forge/json/json.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/os.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace m = retdec::gui::model;
namespace fs = std::filesystem;

namespace {

struct TempDir
{
	fs::path path;
	TempDir()
	{
		path = fs::path(forge::os::fs::make_temp_dir("retdec-gui-launch-").value);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
	fs::path file(const std::string& name) const
	{
		return path / name;
	}
};

void writeFile(const fs::path& p, std::string_view data, bool append = false)
{
	std::ofstream f(p, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
	f << data;
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
	return std::find(v.begin(), v.end(), s) != v.end();
}

int indexOf(const std::vector<std::string>& v, const std::string& s)
{
	const auto it = std::find(v.begin(), v.end(), s);
	return it == v.end() ? -1 : static_cast<int>(it - v.begin());
}

std::vector<std::string> buildArgs(const m::DecompilerLaunchRequest& req, bool passesFile = false)
{
	auto r = m::buildDecompilerArguments(req, passesFile);
	EXPECT_TRUE(r.has_value()) << (r ? "" : r.error());
	return r ? r->args : std::vector<std::string>{};
}

m::DecompilerLaunchRequest tmpRequest()
{
	m::DecompilerLaunchRequest req;
	req.binaryPath = "/tmp/bin";
	req.outputPath = "/tmp/out.c";
	return req;
}

forge::json::value configWithDetection(const char* fn, const char* kind, const char* label, double conf)
{
	using namespace forge::json;
	object det{{"kind", kind}, {"label", label}, {"confidence", conf}};
	object f{{"name", fn}, {"startAddr", "0x401000"}, {"semanticDetections", array{value(det)}}};
	return object{{"functions", array{value(f)}}};
}

} // namespace

TEST(DecompilerLaunch, ResolveGuiDecompiledCPathBesideBinary)
{
	EXPECT_EQ(m::resolveGuiDecompiledCPath("C:/bin/sample.exe", "").generic_string(), "C:/bin/sample.gui-decompiled.c");
}

TEST(DecompilerLaunch, ResolveGuiDecompiledCPathCustomDir)
{
	EXPECT_EQ(
		m::resolveGuiDecompiledCPath("C:/OneDrive/Desktop/foo.exe", "D:/retdec-out").generic_string(),
		"D:/retdec-out/foo.gui-decompiled.c");
	// Whitespace around the configured directory is ignored.
	EXPECT_EQ(m::resolveGuiDecompiledCPath("/x/foo.exe", "  /out  ").generic_string(), "/out/foo.gui-decompiled.c");
	// A binary with no directory part.
	EXPECT_EQ(m::resolveGuiDecompiledCPath("foo.exe", "").generic_string(), "foo.gui-decompiled.c");
}

TEST(DecompilerLaunch, LocateGuiDecompiledCPathPrefersConfiguredDir)
{
	TempDir outDir, binDir;
	const fs::path binPath = binDir.file("app.exe");
	writeFile(binPath, "MZ");

	const fs::path beside = m::resolveGuiDecompiledCPath(binPath, "");
	writeFile(beside, "int x;");
	const fs::path configured = m::resolveGuiDecompiledCPath(binPath, outDir.path.string());
	writeFile(configured, "int y;");

	EXPECT_EQ(m::locateGuiDecompiledCPath(binPath, outDir.path.string()), fs::absolute(configured).lexically_normal());
}

TEST(DecompilerLaunch, LocateGuiDecompiledCPathFallsBackToBesideBinary)
{
	TempDir binDir;
	const fs::path binPath = binDir.file("app.exe");
	writeFile(binPath, "MZ");
	const fs::path beside = m::resolveGuiDecompiledCPath(binPath, "");
	writeFile(beside, "int x;");

	EXPECT_EQ(
		m::locateGuiDecompiledCPath(binPath, "C:/nonexistent-output-dir"), fs::absolute(beside).lexically_normal());
}

TEST(DecompilerLaunch, LocateGuiDecompiledCPathNothingExistsReturnsPreferred)
{
	EXPECT_EQ(
		m::locateGuiDecompiledCPath("/nonexistent/app.exe", "/nonexistent-out").generic_string(),
		"/nonexistent-out/app.gui-decompiled.c");
	EXPECT_EQ(
		m::locateGuiDecompiledCPath("/nonexistent/app.exe", "").generic_string(), "/nonexistent/app.gui-decompiled.c");
}

TEST(DecompilerLaunch, BuildsBaselineArguments)
{
	m::DecompilerLaunchRequest req;
	req.binaryPath = "C:/bin/sample.exe";
	req.outputPath = "C:/bin/sample.gui-decompiled.c";
	req.arch = "x86-64";

	const auto args = buildArgs(req);
	ASSERT_FALSE(args.empty());
	EXPECT_EQ(args.at(0), req.binaryPath);
	EXPECT_TRUE(contains(args, "-o"));
	EXPECT_TRUE(contains(args, req.outputPath));
	EXPECT_TRUE(contains(args, "-f"));
	EXPECT_TRUE(contains(args, "plain"));
	EXPECT_TRUE(contains(args, "-s"));
	EXPECT_TRUE(contains(args, "-a"));
	EXPECT_TRUE(contains(args, "x86-64"));
	EXPECT_FALSE(contains(args, "--backend-no-opts"));
	EXPECT_FALSE(contains(args, "--print-after-all"));
	EXPECT_FALSE(contains(args, "--select-decode-only"));
	EXPECT_FALSE(contains(args, "--try-emulation"));
	EXPECT_FALSE(contains(args, "--max-memory"));
	EXPECT_FALSE(contains(args, "--backend-keep-library-funcs"));
	EXPECT_FALSE(contains(args, "--backend-keep-all-brackets"));
	EXPECT_FALSE(contains(args, "--ar-name"));
	EXPECT_FALSE(contains(args, "--no-memory-limit"));
	EXPECT_FALSE(contains(args, "-m"));
	EXPECT_FALSE(contains(args, "--backend-disabled-opts"));
	EXPECT_FALSE(contains(args, "--backend-enabled-opts"));
	EXPECT_FALSE(contains(args, "--ar-index"));
}

TEST(DecompilerLaunch, BaselineArgvIsExactlyTheQtOne)
{
	m::DecompilerLaunchRequest req;
	req.binaryPath = "C:/bin/sample.exe";
	req.outputPath = "C:/bin/sample.gui-decompiled.c";
	req.arch = "x86-64";
	const std::vector<std::string> expected = {
		"C:/bin/sample.exe",
		"-o",
		"C:/bin/sample.gui-decompiled.c",
		"-f",
		"plain",
		"-s",
		"--output-lang",
		"c",
		"-a",
		"x86-64",
		"--profile",
		"balanced"};
	EXPECT_EQ(buildArgs(req, true), expected);
}

TEST(DecompilerLaunch, FullArgvOrderMatchesQt)
{
	TempDir tmp;
	const fs::path cfg = tmp.file("extra.json");
	const fs::path pdb = tmp.file("x.pdb");
	const fs::path sig = tmp.file("x.yara");
	writeFile(cfg, "{}");
	writeFile(pdb, "pdb");
	writeFile(sig, "rule");

	m::DecompilerLaunchRequest req;
	req.binaryPath = "/b";
	req.outputPath = "/o.c";
	req.arch = " arm ";
	req.decompiler.extraConfigPath = cfg.string();
	req.decompiler.outputLang = "python";
	req.decompiler.decompileProfile = "quality";
	req.disableStaticCodeDetection = true;
	req.backendNoOpts = true;
	req.printAfterAll = true;
	req.printBeforeAll = true;
	req.emitCfg = true;
	req.emitCg = true;
	req.keepUnreachableFuncs = true;
	req.selectedFunctions = {"main", "f"};
	req.selectedRanges = {"0x1-0x2", "0x3-0x4"};
	req.selectDecodeOnly = true;
	req.entryPoint = 0xABC;
	req.pdbPath = pdb.string();
	req.varRenamer = "Simple";
	req.staticCodeSigFile = sig.string();
	req.cleanup = true;
	req.tryEmulation = true;
	req.maxMemoryBytes = 1024;
	req.keepLibraryFuncs = true;
	req.keepAllBrackets = true;
	req.noTimeVaryingInfo = true;
	req.noVarRenaming = true;
	req.noCompoundOperators = true;
	req.noSymbolicNames = true;
	req.callInfoObtainer = "PESSIM";
	req.endian = "Big";
	req.bitSize = 64;
	req.rawSectionVma = 0x1000;
	req.rawMode = true;
	req.noMemoryLimit = true;
	req.backendDisabledOpts = " a ";
	req.backendEnabledOpts = "b";
	req.arIndex = 3;
	req.silent = false;

	const std::vector<std::string> expected = {
		"--config",
		cfg.string(),
		"/b",
		"-o",
		"/o.c",
		"-f",
		"plain",
		"--output-lang",
		"python",
		"-a",
		"arm",
		"--profile",
		"quality",
		"--disable-static-code-detection",
		"--backend-no-opts",
		"--print-after-all",
		"--print-before-all",
		"--backend-emit-cfg",
		"--backend-emit-cg",
		"-k",
		"--select-functions",
		"main,f",
		"--select-ranges",
		"0x1-0x2,0x3-0x4",
		"--select-decode-only",
		"--raw-entry-point",
		"0xabc",
		"-p",
		pdb.string(),
		"--backend-var-renamer",
		"simple",
		"--static-code-sigfile",
		sig.string(),
		"--cleanup",
		"--try-emulation",
		"--max-memory",
		"1024",
		"--backend-keep-library-funcs",
		"--backend-keep-all-brackets",
		"--backend-no-time-varying-info",
		"--backend-no-var-renaming",
		"--backend-no-compound-operators",
		"--backend-no-symbolic-names",
		"--backend-call-info-obtainer",
		"pessim",
		"-e",
		"big",
		"-b",
		"64",
		"--raw-section-vma",
		"0x1000",
		"-m",
		"raw",
		"--no-memory-limit",
		"--backend-disabled-opts",
		"a",
		"--backend-enabled-opts",
		"b",
		"--ar-index",
		"3"};
	EXPECT_EQ(buildArgs(req), expected);
}

TEST(DecompilerLaunch, MissingExtraConfigIsAWarningNotAnError)
{
	auto req = tmpRequest();
	req.decompiler.extraConfigPath = "/nonexistent/extra-config.json";
	auto r = m::buildDecompilerArguments(req);
	ASSERT_TRUE(r.has_value());
	EXPECT_FALSE(contains(r->args, "--config"));
	EXPECT_NE(r->warning.find("/nonexistent/extra-config.json"), std::string::npos);
}

TEST(DecompilerLaunch, FastModeAddsFlagsAndLlvmPassesJson)
{
	auto req = tmpRequest();
	req.fastDecompile = true;

	auto r = m::buildDecompilerArguments(req, true);
	ASSERT_TRUE(r.has_value()) << r.error();
	const auto& args = r->args;
	EXPECT_TRUE(contains(args, "--backend-no-opts"));
	EXPECT_TRUE(contains(args, "--disable-static-code-detection"));
	EXPECT_EQ(args.at(static_cast<std::size_t>(indexOf(args, "--profile") + 1)), "fast");
	const int idx = indexOf(args, "--llvm-passes-json");
	ASSERT_GE(idx, 0);
	ASSERT_NE(r->llvmPassesFile, nullptr);
	EXPECT_FALSE(r->llvmPassesFile->path().empty());
	EXPECT_EQ(args.at(static_cast<std::size_t>(idx + 1)), r->llvmPassesFile->path().string());

	// The temp file holds the fast preset as a JSON array, and goes away with its owner.
	const fs::path passesPath = r->llvmPassesFile->path();
	auto text = forge::os::read_file(passesPath.string());
	ASSERT_TRUE(static_cast<bool>(text));
	const auto doc = forge::json::parse(text.value);
	ASSERT_TRUE(doc.is_arr());
	EXPECT_FALSE(doc.arr().empty());
	EXPECT_EQ(doc.arr().front().str(), "retdec-provider-init");
	r->llvmPassesFile.reset();
	EXPECT_FALSE(fs::exists(passesPath));
}

TEST(DecompilerLaunch, CustomLlvmPassesDropDisabledAndProfile)
{
	auto req = tmpRequest();
	req.decompiler.useCustomLlvmPasses = true;
	req.decompiler.llvmPassesDisabled = {"retdec-decoder"};

	auto r = m::buildDecompilerArguments(req, true);
	ASSERT_TRUE(r.has_value()) << r.error();
	EXPECT_FALSE(contains(r->args, "--profile"));
	ASSERT_NE(r->llvmPassesFile, nullptr);
	const auto doc = forge::json::parse(forge::os::read_file(r->llvmPassesFile->path().string()).value);
	ASSERT_TRUE(doc.is_arr());
	for (const auto& v: doc.arr())
		EXPECT_NE(v.str(), "retdec-decoder");

	auto preset = m::loadLlvmPassPreset(false);
	ASSERT_TRUE(preset.has_value()) << preset.error();
	EXPECT_EQ(
		doc.arr().size(),
		static_cast<std::size_t>(
			std::count_if(preset->begin(), preset->end(), [](const std::string& s) { return s != "retdec-decoder"; })));
}

TEST(DecompilerLaunch, NoPassesFileWhenNotRequested)
{
	auto req = tmpRequest();
	req.fastDecompile = true;
	auto r = m::buildDecompilerArguments(req, false);
	ASSERT_TRUE(r.has_value());
	EXPECT_FALSE(contains(r->args, "--llvm-passes-json"));
	EXPECT_EQ(r->llvmPassesFile, nullptr);
}

TEST(DecompilerLaunch, PrintAfterAllFlag)
{
	auto req = tmpRequest();
	req.printAfterAll = true;
	EXPECT_TRUE(contains(buildArgs(req), "--print-after-all"));
}

TEST(DecompilerLaunch, EmitCfgAndDisablePatternsFlags)
{
	auto req = tmpRequest();
	req.emitCfg = true;
	req.disableStaticCodeDetection = true;

	const auto args = buildArgs(req);
	EXPECT_TRUE(contains(args, "--backend-emit-cfg"));
	EXPECT_TRUE(contains(args, "--disable-static-code-detection"));
	EXPECT_FALSE(contains(args, "--backend-no-opts"));
	EXPECT_FALSE(contains(args, "--backend-emit-cg"));
}

TEST(DecompilerLaunch, SelectRangesPrintBeforeAndKeepUnreachable)
{
	auto req = tmpRequest();
	req.selectedRanges = {"0x401000-0x401200"};
	req.printBeforeAll = true;
	req.keepUnreachableFuncs = true;
	req.emitCg = true;

	const auto args = buildArgs(req);
	EXPECT_TRUE(contains(args, "--select-ranges"));
	const int idx = indexOf(args, "--select-ranges");
	ASSERT_GE(idx, 0);
	ASSERT_LT(idx + 1, static_cast<int>(args.size()));
	EXPECT_EQ(args.at(static_cast<std::size_t>(idx + 1)), "0x401000-0x401200");
	EXPECT_TRUE(contains(args, "--print-before-all"));
	EXPECT_TRUE(contains(args, "-k"));
	EXPECT_TRUE(contains(args, "--backend-emit-cg"));
}

TEST(DecompilerLaunch, EntryPointPdbRenamerCleanupAndSigfile)
{
	TempDir tmp;
	const fs::path pdb = tmp.file("a.pdb");
	const fs::path sig = tmp.file("a.yar");
	writeFile(pdb, "pdb");
	writeFile(sig, "rule x { condition: true }");

	auto req = tmpRequest();
	req.entryPoint = 0x401000;
	req.pdbPath = pdb.string();
	req.varRenamer = "simple";
	req.staticCodeSigFile = sig.string();
	req.cleanup = true;

	const auto args = buildArgs(req);
	EXPECT_TRUE(contains(args, "--raw-entry-point"));
	EXPECT_TRUE(contains(args, "0x401000"));
	EXPECT_TRUE(contains(args, "-p"));
	EXPECT_TRUE(contains(args, "--backend-var-renamer"));
	EXPECT_TRUE(contains(args, "simple"));
	EXPECT_TRUE(contains(args, "--static-code-sigfile"));
	EXPECT_TRUE(contains(args, "--cleanup"));
	EXPECT_FALSE(contains(args, "readable"));
}

TEST(DecompilerLaunch, MissingPdbSigfileAndReadableRenamerAreOmitted)
{
	auto req = tmpRequest();
	req.pdbPath = "/nonexistent/a.pdb";
	req.staticCodeSigFile = "/nonexistent/a.yar";
	req.varRenamer = "Readable";
	const auto args = buildArgs(req);
	EXPECT_FALSE(contains(args, "-p"));
	EXPECT_FALSE(contains(args, "--static-code-sigfile"));
	EXPECT_FALSE(contains(args, "--backend-var-renamer"));
}

TEST(DecompilerLaunch, TryEmulationMaxMemoryAndKeepLibraryFuncs)
{
	auto req = tmpRequest();
	req.tryEmulation = true;
	req.maxMemoryBytes = 2147483648ull;
	req.keepLibraryFuncs = true;

	const auto args = buildArgs(req);
	EXPECT_TRUE(contains(args, "--try-emulation"));
	EXPECT_TRUE(contains(args, "--backend-keep-library-funcs"));
	const int idx = indexOf(args, "--max-memory");
	ASSERT_GE(idx, 0);
	ASSERT_LT(idx + 1, static_cast<int>(args.size()));
	EXPECT_EQ(args.at(static_cast<std::size_t>(idx + 1)), "2147483648");
}

TEST(DecompilerLaunch, BackendStyleRawArchiveAndEndianFlags)
{
	auto req = tmpRequest();
	req.keepAllBrackets = true;
	req.noTimeVaryingInfo = true;
	req.noVarRenaming = true;
	req.noCompoundOperators = true;
	req.noSymbolicNames = true;
	req.callInfoObtainer = "optim";
	req.arName = "member.o";
	req.endian = "little";
	req.bitSize = 32;
	req.rawSectionVma = 0x80000000ull;
	req.rawMode = true;
	req.noMemoryLimit = true;

	const auto args = buildArgs(req);
	EXPECT_TRUE(contains(args, "--backend-keep-all-brackets"));
	EXPECT_TRUE(contains(args, "--backend-no-time-varying-info"));
	EXPECT_TRUE(contains(args, "--backend-no-var-renaming"));
	EXPECT_TRUE(contains(args, "--backend-no-compound-operators"));
	EXPECT_TRUE(contains(args, "--backend-no-symbolic-names"));
	EXPECT_TRUE(contains(args, "--backend-call-info-obtainer"));
	EXPECT_TRUE(contains(args, "optim"));
	EXPECT_TRUE(contains(args, "--ar-name"));
	EXPECT_TRUE(contains(args, "member.o"));
	EXPECT_TRUE(contains(args, "-e"));
	EXPECT_TRUE(contains(args, "little"));
	EXPECT_TRUE(contains(args, "-b"));
	EXPECT_TRUE(contains(args, "32"));
	EXPECT_TRUE(contains(args, "--raw-section-vma"));
	EXPECT_TRUE(contains(args, "0x80000000"));
	EXPECT_TRUE(contains(args, "-m"));
	EXPECT_TRUE(contains(args, "raw"));
	EXPECT_TRUE(contains(args, "--no-memory-limit"));
}

TEST(DecompilerLaunch, InvalidChoicesAreOmitted)
{
	auto req = tmpRequest();
	req.callInfoObtainer = "fast";
	req.endian = "middle";
	req.bitSize = 8;
	const auto args = buildArgs(req);
	EXPECT_FALSE(contains(args, "--backend-call-info-obtainer"));
	EXPECT_FALSE(contains(args, "-e"));
	EXPECT_FALSE(contains(args, "-b"));
}

TEST(DecompilerLaunch, BackendOptsAndArIndex)
{
	auto req = tmpRequest();
	req.backendDisabledOpts = "x";
	req.backendEnabledOpts = "y";
	req.arIndex = 2;

	const auto args = buildArgs(req);
	const int disIdx = indexOf(args, "--backend-disabled-opts");
	ASSERT_GE(disIdx, 0);
	EXPECT_EQ(args.at(static_cast<std::size_t>(disIdx + 1)), "x");
	const int enIdx = indexOf(args, "--backend-enabled-opts");
	ASSERT_GE(enIdx, 0);
	EXPECT_EQ(args.at(static_cast<std::size_t>(enIdx + 1)), "y");
	const int arIdx = indexOf(args, "--ar-index");
	ASSERT_GE(arIdx, 0);
	EXPECT_EQ(args.at(static_cast<std::size_t>(arIdx + 1)), "2");
	EXPECT_FALSE(contains(args, "--ar-name"));

	auto named = tmpRequest();
	named.arName = "member.o";
	named.arIndex = 2;
	const auto namedArgs = buildArgs(named);
	EXPECT_TRUE(contains(namedArgs, "--ar-name"));
	EXPECT_TRUE(contains(namedArgs, "member.o"));
	EXPECT_FALSE(contains(namedArgs, "--ar-index"));
}

TEST(DecompilerLaunch, BackendNoOptsStandalone)
{
	auto req = tmpRequest();
	req.backendNoOpts = true;
	req.fastDecompile = false;
	const auto args = buildArgs(req);
	EXPECT_EQ(std::count(args.begin(), args.end(), "--backend-no-opts"), 1);

	req.fastDecompile = true;
	const auto fastArgs = buildArgs(req);
	EXPECT_EQ(std::count(fastArgs.begin(), fastArgs.end(), "--backend-no-opts"), 1);
}

TEST(DecompilerLaunch, VerboseOmitsSilentFlag)
{
	auto req = tmpRequest();
	req.silent = false;
	EXPECT_FALSE(contains(buildArgs(req), "-s"));
}

TEST(DecompilerLaunch, SelectDecodeOnlyFlag)
{
	auto req = tmpRequest();
	req.selectedFunctions = {"main"};
	req.selectDecodeOnly = true;
	const auto args = buildArgs(req);
	EXPECT_TRUE(contains(args, "--select-functions"));
	EXPECT_TRUE(contains(args, "--select-decode-only"));
}

TEST(DecompilerLaunch, SelectFunctionsFlag)
{
	auto req = tmpRequest();
	req.selectedFunctions = {"main", "function_401000"};
	const auto args = buildArgs(req);
	const int idx = indexOf(args, "--select-functions");
	ASSERT_GE(idx, 0);
	ASSERT_LT(idx + 1, static_cast<int>(args.size()));
	EXPECT_EQ(args.at(static_cast<std::size_t>(idx + 1)), "main,function_401000");
}

TEST(DecompilerLaunch, InteractiveNeuralEnvWhenModelExists)
{
	TempDir tmp;
	const fs::path model = tmp.file("model.gguf");
	writeFile(model, "GGUF");

	m::AppSettings st;
	st.ml.modelPath = model.string();
	st.ml.inferenceDevice = m::MLSettings::InferenceDevice::CPU;
	st.ml.contextLength = 2048;
	st.ml.maxNewTokens = 128;
	st.ml.temperature = 0.55;
	st.ml.topP = 0.8;
	st.ml.topK = 15;
	st.analysis.threadCount = 4;

	const auto env = m::buildDecompilerProcessEnvironment(st, true);
	EXPECT_EQ(env.at("RETDEC_NEURAL_REFINE"), "1");
	EXPECT_EQ(fs::path(env.at("RETDEC_NEURAL_MODEL")), fs::absolute(model).lexically_normal());
	EXPECT_EQ(env.at("RETDEC_NEURAL_N_GPU_LAYERS"), "0");
	EXPECT_EQ(env.at("RETDEC_NEURAL_CTX"), "2048");
	EXPECT_EQ(env.at("RETDEC_NEURAL_MAX_TOKENS"), "128");
	EXPECT_EQ(env.at("RETDEC_NEURAL_TOP_K"), "15");
	EXPECT_EQ(env.at("RETDEC_NEURAL_THREADS"), "4");
	EXPECT_EQ(env.at("RETDEC_NEURAL_TEMPERATURE"), "0.5500");
	EXPECT_EQ(env.at("RETDEC_NEURAL_TOP_P"), "0.8000");

	// Headless: nothing overridden, the child sees the GUI's own environment.
	const auto headless = m::buildDecompilerProcessEnvironment(st, false);
	EXPECT_TRUE(headless.empty());
	EXPECT_EQ(
		m::processEnvironmentValue(headless, "RETDEC_NEURAL_REFINE"), forge::os::getenv("RETDEC_NEURAL_REFINE", ""));
}

TEST(DecompilerLaunch, InteractiveNeuralGpuLayersAndMissingModel)
{
	TempDir tmp;
	const fs::path model = tmp.file("m.gguf");
	writeFile(model, "GGUF");
	m::AppSettings st;
	st.ml.modelPath = model.string();
	st.ml.inferenceDevice = m::MLSettings::InferenceDevice::Auto;
	st.ml.contextLength = 256; // below 512: not published
	auto env = m::buildDecompilerProcessEnvironment(st, true);
	EXPECT_EQ(env.at("RETDEC_NEURAL_N_GPU_LAYERS"), "-1");
	EXPECT_FALSE(env.contains("RETDEC_NEURAL_CTX"));
	EXPECT_FALSE(env.contains("RETDEC_NEURAL_THREADS"));

	st.ml.modelPath = tmp.file("missing.gguf").string();
	env = m::buildDecompilerProcessEnvironment(st, true);
	EXPECT_FALSE(env.contains("RETDEC_NEURAL_REFINE"));
	EXPECT_FALSE(env.contains("RETDEC_NEURAL_MODEL"));
}

TEST(DecompilerLaunch, InteractiveDisablesOclWhenGpuOff)
{
	m::AppSettings st;
	st.cuda.useGPU = false;
	const auto env = m::buildDecompilerProcessEnvironment(st, true);
	EXPECT_EQ(env.at("RETDEC_OCL_HOST"), "0");
	EXPECT_EQ(m::processEnvironmentValue(env, "RETDEC_OCL_HOST"), "0");
}

TEST(DecompilerLaunch, InteractiveOclCacheAndProfileEnv)
{
	m::AppSettings st;
	st.cuda.kernelCacheDir = "/tmp/ocl-cache-test/";
	st.cuda.enableProfiling = true;
	const auto env = m::buildDecompilerProcessEnvironment(st, true);
	EXPECT_NE(env.at("RETDEC_OCL_CACHE_DIR").find("ocl-cache-test"), std::string::npos);
	EXPECT_EQ(env.at("RETDEC_PROFILE_JSON"), "1");

	const auto headless = m::buildDecompilerProcessEnvironment(st, false);
	EXPECT_EQ(
		m::processEnvironmentValue(headless, "RETDEC_PROFILE_JSON"), forge::os::getenv("RETDEC_PROFILE_JSON", ""));
	EXPECT_EQ(
		m::processEnvironmentValue(headless, "RETDEC_OCL_CACHE_DIR"), forge::os::getenv("RETDEC_OCL_CACHE_DIR", ""));
}

TEST(DecompilerLaunch, InteractiveNeuralSha256)
{
	TempDir tmp;
	const fs::path model = tmp.file("model.gguf");
	writeFile(model, "GGUF");
	m::AppSettings st;
	st.ml.modelPath = model.string();
	st.ml.modelSha256 = " abc123def456 ";
	st.ml.batchRefine = true;

	const auto env = m::buildDecompilerProcessEnvironment(st, true);
	EXPECT_EQ(env.at("RETDEC_NEURAL_REFINE"), "1");
	EXPECT_EQ(env.at("RETDEC_NEURAL_MODEL_SHA256"), "abc123def456");
	EXPECT_FALSE(env.contains("RETDEC_NEURAL_BATCH"));

	const auto headless = m::buildDecompilerProcessEnvironment(st, false);
	EXPECT_EQ(
		m::processEnvironmentValue(headless, "RETDEC_NEURAL_MODEL_SHA256"),
		forge::os::getenv("RETDEC_NEURAL_MODEL_SHA256", ""));
}

TEST(DecompilerLaunch, IncrementalDiagnosticsReadsNewWarningLines)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	writeFile(log, "info: starting\nwarning: first\n");

	uint64_t offset = 0;
	auto first = m::scanDecompilerLogDiagnosticsIncremental(log, &offset);
	ASSERT_EQ(first.size(), 1u);
	EXPECT_EQ(first[0].severity, m::Severity::Warning);
	EXPECT_EQ(first[0].message, "warning: first");
	EXPECT_EQ(first[0].source, "retdec-decompiler");
	EXPECT_GT(offset, 0u);

	writeFile(log, "error: second\npartial warning", true);
	auto second = m::scanDecompilerLogDiagnosticsIncremental(log, &offset);
	ASSERT_EQ(second.size(), 1u);
	EXPECT_EQ(second[0].severity, m::Severity::Error);
	// The partial last line waits for its newline.
	EXPECT_TRUE(m::scanDecompilerLogDiagnosticsIncremental(log, &offset).empty());
	writeFile(log, "\n", true);
	EXPECT_EQ(m::scanDecompilerLogDiagnosticsIncremental(log, &offset).size(), 1u);
	EXPECT_TRUE(m::scanDecompilerLogDiagnosticsIncremental(log, &offset).empty());
}

TEST(DecompilerLaunch, IncrementalDiagnosticsRespectsMaxEntries)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	writeFile(log, "warning: 1\nwarning: 2\nwarning: 3\n");
	uint64_t offset = 0;
	EXPECT_EQ(m::scanDecompilerLogDiagnosticsIncremental(log, &offset, 2).size(), 2u);
	EXPECT_EQ(m::scanDecompilerLogDiagnosticsIncremental(log, &offset, 2).size(), 1u);
}

TEST(DecompilerLaunch, FullDiagnosticsScan)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	writeFile(log, "ok\r\n[ERROR] boom\r\nWarning: careful\r\n  \r\nlast line warning");
	const auto d = m::scanDecompilerLogDiagnostics(log);
	ASSERT_EQ(d.size(), 3u);
	EXPECT_EQ(d[0].severity, m::Severity::Error);
	EXPECT_EQ(d[0].message, "[ERROR] boom");
	EXPECT_EQ(d[1].severity, m::Severity::Warning);
	EXPECT_EQ(d[2].message, "last line warning");
	EXPECT_EQ(m::scanDecompilerLogDiagnostics(log, 1).size(), 1u);
	EXPECT_TRUE(m::scanDecompilerLogDiagnostics(tmp.file("missing.log")).empty());
}

TEST(DecompilerLaunch, AppendLogIncrementalFromOffset)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	writeFile(log, "line one\n");

	uint64_t offset = 0;
	EXPECT_EQ(m::readDecompilerLogIncremental(log, &offset), "line one\n");
	EXPECT_GT(offset, 0u);

	writeFile(log, "line two\n", true);
	EXPECT_EQ(m::readDecompilerLogIncremental(log, &offset), "line two\n");
	EXPECT_TRUE(m::readDecompilerLogIncremental(log, &offset).empty());

	// At most maxBytesPerTick per call.
	writeFile(log, "abcdefgh", true);
	EXPECT_EQ(m::readDecompilerLogIncremental(log, &offset, 3), "abc");
	EXPECT_EQ(m::readDecompilerLogIncremental(log, &offset, 100), "defgh");
}

TEST(DecompilerLaunch, AppendLogTailOnlyForHugeFiles)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	std::string body = "first line\n" + std::string(600 * 1024, 'x') + "\nlast line\n";
	writeFile(log, body);

	const auto out = m::readDecompilerLogForConsole(log);
	EXPECT_FALSE(out.notice.empty());
	EXPECT_NE(out.notice.find("showing last 512 KiB"), std::string::npos);
	EXPECT_LE(out.text.size(), 512u * 1024u);
	EXPECT_EQ(out.text.find("first line"), std::string::npos);
	EXPECT_NE(out.text.find("last line"), std::string::npos);

	const auto small = m::readDecompilerLogForConsole(log, 10 * 1024 * 1024);
	EXPECT_TRUE(small.notice.empty());
	EXPECT_EQ(small.text, body);

	EXPECT_EQ(m::scanDecompilerLogDiagnostics(log).size(), 0u);
	EXPECT_TRUE(m::readDecompilerLogForConsole(tmp.file("missing.log")).text.empty());
}

TEST(DecompilerLaunch, LogTailForHistory)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	writeFile(log, "short");
	EXPECT_EQ(m::readDecompilerLogTail(log), "short");
	writeFile(log, std::string(5000, 'a') + "END");
	const std::string tail = m::readDecompilerLogTail(log);
	EXPECT_TRUE(tail.starts_with("…\n"));
	EXPECT_TRUE(tail.ends_with("END"));
	EXPECT_EQ(tail.size(), std::string("…\n").size() + 4000u);
}

TEST(DecompilerLaunch, PollLogProgressReturnsNothingWhenEmpty)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	writeFile(log, "");
	uint64_t offset = 0;
	EXPECT_FALSE(m::pollDecompileLogProgress(log, &offset).has_value());
	EXPECT_EQ(offset, 0u);
	EXPECT_FALSE(m::pollDecompileLogProgress(tmp.file("missing.log"), &offset).has_value());
}

TEST(DecompilerLaunch, PollLogProgressParsesRunningPhase)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	writeFile(
		log, "Running phase: Initialization ( 0.01s )\nRunning phase: Input binary to LLVM IR decoding ( 1.23s )\n");

	uint64_t offset = 0;
	const auto prog = m::pollDecompileLogProgress(log, &offset);
	ASSERT_TRUE(prog.has_value());
	EXPECT_EQ(prog->stage, "Binary loading");
	EXPECT_GT(prog->percent, 0);
	EXPECT_LT(prog->percent, 100);
	EXPECT_FALSE(m::pollDecompileLogProgress(log, &offset).has_value());
}

TEST(DecompilerLaunch, PollLogProgressIncrementalRead)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	writeFile(log, "Running phase: Initialization ( 0.01s )\n");

	uint64_t offset = 0;
	const auto prog = m::pollDecompileLogProgress(log, &offset);
	ASSERT_TRUE(prog.has_value());
	EXPECT_EQ(prog->stage, "Binary loading");

	writeFile(log, "Running phase: Class hierarchy analysis ( 0.50s )\n", true);
	const auto prog2 = m::pollDecompileLogProgress(log, &offset);
	ASSERT_TRUE(prog2.has_value());
	EXPECT_EQ(prog2->stage, "RTTI reconstruction");
	EXPECT_GT(prog2->percent, prog->percent);
}

TEST(DecompilerLaunch, PollLogProgressParsesBackendProgressBar)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	writeFile(log, "[progress] [########----------------] 33% (3/9) elapsed 1.0s, eta 2.0s\n");

	uint64_t offset = 0;
	const auto prog = m::pollDecompileLogProgress(log, &offset);
	ASSERT_TRUE(prog.has_value());
	EXPECT_EQ(prog->stage, "Code generation");
	EXPECT_GE(prog->percent, 65);
	EXPECT_LE(prog->percent, 99);
	EXPECT_EQ(prog->percent, 65 + (33 * 34) / 100);
}

TEST(DecompilerLaunch, PollLogProgressSubPhaseAnsiAndLongestKeyword)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	// " -> " sub-phase with ANSI colours: "simple types" beats "type" (longer keyword).
	writeFile(log, "\x1b[1;32mRunning\x1b[0m -> Simple types recovery ( 0.2 s )\n");
	uint64_t offset = 0;
	const auto prog = m::pollDecompileLogProgress(log, &offset);
	ASSERT_TRUE(prog.has_value());
	EXPECT_EQ(prog->stage, "Type inference");
	EXPECT_EQ(prog->percent, (7 * 100) / 14);
}

TEST(DecompilerLaunch, PollLogProgressKeepsTheFurthestStage)
{
	TempDir tmp;
	const fs::path log = tmp.file("decompile.log");
	writeFile(log, "Running phase: Dead code removal\nRunning phase: Initialization\nunrelated\n");
	uint64_t offset = 0;
	const auto prog = m::pollDecompileLogProgress(log, &offset);
	ASSERT_TRUE(prog.has_value());
	EXPECT_EQ(prog->stage, "Dead code elimination");
}

TEST(DecompilerLaunch, SemanticDetectionsOmitConcurrencyWhenAnalysisFlagOff)
{
	m::AppSettings st;
	st.analysis.enableConcurrency = false;
	const auto cfg = configWithDetection("worker", "concurrency", "thread pool", 0.9);
	EXPECT_TRUE(m::semanticDetectionDiagnostics(cfg, st).empty());
}

TEST(DecompilerLaunch, SemanticDetectionsOmitPatternBelowRecoveryFloor)
{
	m::AppSettings st;
	st.recovery.patternConfidence = 0.9;
	const auto cfg = configWithDetection("copy_block", "pattern", "memcpy idiom", 0.5);
	EXPECT_TRUE(m::semanticDetectionDiagnostics(cfg, st).empty());
}

TEST(DecompilerLaunch, SemanticDetectionsProduceDiagnostics)
{
	m::AppSettings st;
	const auto cfg = configWithDetection("encrypt", "crypto", "AES-128", 0.95);
	const auto d = m::semanticDetectionDiagnostics(cfg, st);
	ASSERT_EQ(d.size(), 1u);
	EXPECT_EQ(d[0].severity, m::Severity::Info);
	EXPECT_EQ(d[0].message, "AES-128 (confidence 95%)");
	EXPECT_EQ(d[0].function, "encrypt");
	EXPECT_EQ(d[0].source, "semantic");
	EXPECT_EQ(d[0].address, 0x401000u);

	const auto w = m::semanticDetectionDiagnostics(configWithDetection("f", "stl", "std::vector", 0.7), st);
	ASSERT_EQ(w.size(), 1u);
	EXPECT_EQ(w[0].severity, m::Severity::Warning);

	st.recovery.detectCrypto = false;
	EXPECT_TRUE(m::semanticDetectionDiagnostics(cfg, st).empty());
	EXPECT_TRUE(m::semanticDetectionDiagnostics(cfg, m::AppSettings{}, 0).empty());
}

TEST(DecompilerLaunch, ClassifyLogLine)
{
	EXPECT_EQ(m::classifyDecompilerLogLine("Error: x"), m::Severity::Error);
	EXPECT_EQ(m::classifyDecompilerLogLine("  WARNING y"), m::Severity::Warning);
	EXPECT_EQ(m::classifyDecompilerLogLine("[warn] z"), m::Severity::Warning);
	EXPECT_FALSE(m::classifyDecompilerLogLine("Running phase: x").has_value());
	EXPECT_FALSE(m::classifyDecompilerLogLine("   ").has_value());
}
