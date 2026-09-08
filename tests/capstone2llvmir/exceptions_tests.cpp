/**
 * @file tests/capstone2llvmir/exceptions_tests.cpp
 * @brief Tests for the capstone2llvmir exception classes.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "retdec/capstone2llvmir/exceptions.h"

namespace retdec {
namespace capstone2llvmir {
namespace tests {

/**
 * @brief Tests that what() hands back a pointer that outlives the call.
 *
 * Four of these classes used to `return someTemporary.c_str()`, so the pointer
 * dangled the moment what() returned and every catch site that logged the
 * message read freed memory. The tests below read through the returned pointer
 * after what() has returned -- which is what any real handler does, and what
 * the defect made undefined. Under ASan the old code fails them with
 * heap-use-after-free; without a sanitizer it usually still returns the right
 * bytes, which is why nothing noticed.
 */
class Capstone2LlvmIrExceptionsTests : public ::testing::Test {};

TEST_F(Capstone2LlvmIrExceptionsTests, CapstoneErrorWhatSurvivesTheCall)
{
	CapstoneError e(CS_ERR_MEM);
	const char* w = e.what();
	EXPECT_EQ(std::string(w), e.getMessage());
	EXPECT_GT(std::strlen(w), 0u);
}

TEST_F(Capstone2LlvmIrExceptionsTests, CapstoneErrorWhatIsStableAcrossCalls)
{
	CapstoneError e(CS_ERR_ARCH);
	const char* first = e.what();
	const std::string copy(first);
	// A second call must not invalidate what the first returned.
	(void)e.what();
	EXPECT_EQ(std::string(first), copy);
}

TEST_F(Capstone2LlvmIrExceptionsTests, ModeSettingErrorWhatSurvivesTheCall)
{
	ModeSettingError e(CS_ARCH_X86, CS_MODE_ARM, ModeSettingError::eType::BASIC_MODE);
	const char* w = e.what();
	EXPECT_EQ(std::string(w), e.getMessage());
	EXPECT_NE(std::string(w).find("Basic mode"), std::string::npos);
}

TEST_F(Capstone2LlvmIrExceptionsTests, UnexpectedOperandsErrorWhatSurvivesTheCall)
{
	cs_insn insn{};
	insn.address = 0x401000;
	std::strcpy(insn.mnemonic, "mov");
	std::strcpy(insn.op_str, "eax, ebx");

	UnexpectedOperandsError e(&insn, "why");
	const char* w = e.what();
	const std::string s(w);
	EXPECT_NE(s.find("Unexpected operand"), std::string::npos);
	EXPECT_NE(s.find("401000"), std::string::npos);
	EXPECT_NE(s.find("mov"), std::string::npos);
	EXPECT_NE(s.find("why"), std::string::npos);
}

TEST_F(Capstone2LlvmIrExceptionsTests, UnhandledInstructionErrorWhatSurvivesTheCall)
{
	cs_insn insn{};
	insn.address = 0x8048000;
	std::strcpy(insn.mnemonic, "nop");
	std::strcpy(insn.op_str, "");

	UnhandledInstructionError e(&insn);
	const char* w = e.what();
	const std::string s(w);
	EXPECT_NE(s.find("Unhandled instruction"), std::string::npos);
	EXPECT_NE(s.find("8048000"), std::string::npos);
	EXPECT_NE(s.find("nop"), std::string::npos);
}

/**
 * The message is built in the constructor, so the cs_insn is read while the
 * caller still owns it rather than whenever a handler happens to ask. Capstone
 * frees instructions with cs_free(), and an exception outliving its cs_insn is
 * the normal case: the translator throws, the caller unwinds past the buffer.
 */
TEST_F(Capstone2LlvmIrExceptionsTests, InstructionErrorDoesNotReadTheInsnLater)
{
	std::string message;
	{
		auto* insn = new cs_insn{};
		insn->address = 0xdead;
		std::strcpy(insn->mnemonic, "hlt");
		std::strcpy(insn->op_str, "");

		UnhandledInstructionError e(insn);
		delete insn; // the exception must not need it any more
		message = e.what();
	}
	EXPECT_NE(message.find("Unhandled instruction"), std::string::npos);
	EXPECT_NE(message.find("dead"), std::string::npos);
	EXPECT_NE(message.find("hlt"), std::string::npos);
}

TEST_F(Capstone2LlvmIrExceptionsTests, GenericErrorWhatSurvivesTheCall)
{
	GenericError e("something went wrong");
	EXPECT_EQ(std::string(e.what()), "something went wrong");
}

/**
 * All of them are caught as std::exception in practice, which is the path that
 * matters: the handler holds only the base reference.
 */
TEST_F(Capstone2LlvmIrExceptionsTests, MessagesReadCorrectlyThroughStdException)
{
	try
	{
		throw CapstoneError(CS_ERR_HANDLE);
	}
	catch (const std::exception& e)
	{
		const char* w = e.what();
		EXPECT_GT(std::strlen(w), 0u);
		EXPECT_EQ(std::string(w), CapstoneError(CS_ERR_HANDLE).getMessage());
	}
}

} // namespace tests
} // namespace capstone2llvmir
} // namespace retdec
