/**
 * @file tests/bin2llvmir/optimizations/syscalls/powerpc_syscalls_tests.cpp
 * @brief Tests for the Linux PowerPC syscall tables.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 */

#include <gtest/gtest.h>

#include "retdec/bin2llvmir/optimizations/syscalls/syscalls.h"

namespace retdec {
namespace bin2llvmir {
namespace tests {

TEST(PowerPcSyscallTests, commonNumbersMatchTheKernelTable)
{
	for (bool is64 : {false, true})
	{
		EXPECT_EQ("exit", SyscallFixer::powerPcSyscallName(1, is64));
		EXPECT_EQ("write", SyscallFixer::powerPcSyscallName(4, is64));
		EXPECT_EQ("timer_create", SyscallFixer::powerPcSyscallName(240, is64));
		EXPECT_EQ("clone3", SyscallFixer::powerPcSyscallName(435, is64));
	}
}

TEST(PowerPcSyscallTests, thirtyTwoBitOnlyEntriesAreAbsentOn64)
{
	EXPECT_EQ("mmap2", SyscallFixer::powerPcSyscallName(192, false));
	EXPECT_EQ("", SyscallFixer::powerPcSyscallName(192, true));
}

TEST(PowerPcSyscallTests, unknownNumberHasNoName)
{
	EXPECT_EQ("", SyscallFixer::powerPcSyscallName(100000, false));
	EXPECT_EQ("", SyscallFixer::powerPcSyscallName(100000, true));
}

} // namespace tests
} // namespace bin2llvmir
} // namespace retdec
