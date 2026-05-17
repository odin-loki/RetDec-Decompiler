/**
 * @file tests/bin2llvmir/optimizations/simple_types/simple_types_tests.cpp
 * @brief Tests for @c SimpleTypesAnalysis.
 * @copyright (c) 2026, MIT license
 */

#include "bin2llvmir/utils/llvmir_tests.h"
#include "retdec/bin2llvmir/optimizations/simple_types/simple_types.h"
#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/config/config.h"

using namespace ::testing;
using namespace llvm;

namespace retdec {
namespace bin2llvmir {
namespace tests {

class SimpleTypesAnalysisTests: public LlvmIrTests
{
protected:
	void SetUp() override
	{
		LlvmIrTests::SetUp();
		SimpleTypesAnalysis::resetAlternationForTests();
	}

	/// Minimal x86 module environment required by @c SimpleTypesAnalysis::runOnModule.
	void addConfigAbiAndFileImage()
	{
		// bitSize must be a JSON number (not a string) or getByteSize() stays 0 and FileImage ctor throws.
		auto json = config::Config::fromJsonString(R"({
			"architecture": {
				"bitSize": 32,
				"endian": "little",
				"name": "x86"
			},
			"mainAddress": "0x1000"
		})");
		Config* conf = ConfigProvider::addConfig(module.get(), json);
		ASSERT_NE(nullptr, conf);
		ASSERT_NE(nullptr, AbiProvider::addAbi(module.get(), conf));
		// Non-empty raw image: FileImage ctor requires segments / byte length (see fileimage.cpp).
		auto fmt = createFormat();
		int8_t oneByte = 0;
		fmt->appendData(oneByte);
		ASSERT_NE(nullptr, FileImageProvider::addFileImage(module.get(), fmt, conf));
	}

	SimpleTypesAnalysis pass;
};

TEST_F(SimpleTypesAnalysisTests, RunOnMinimalModuleCompletes)
{
	parseInput(R"(
define void @f() {
entry:
  ret void
}
)");
	addConfigAbiAndFileImage();
	EXPECT_FALSE(pass.runOnModule(*module));
	EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}

TEST_F(SimpleTypesAnalysisTests, TwoRunsAlternateFullAndPartialPaths)
{
	parseInput(R"(
define void @g() {
entry:
  ret void
}
)");
	addConfigAbiAndFileImage();
	EXPECT_FALSE(pass.runOnModule(*module));
	EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
	// Second invocation uses the partial (non-RDA) path; should not abort.
	EXPECT_FALSE(pass.runOnModule(*module));
	EXPECT_FALSE(llvm::verifyModule(*module, &llvm::errs()));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
