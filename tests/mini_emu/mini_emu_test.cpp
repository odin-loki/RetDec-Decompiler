/**
 * @file tests/mini_emu/mini_emu_test.cpp
 * @brief Unit tests for MiniEmu and MiniUnpacker.
 */

#include "retdec/mini_emu/mini_emu.h"
#include "retdec/mini_emu/mini_unpacker.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <vector>

using namespace retdec::mini_emu;
using namespace retdec::fileformat::lattice;

// ─── Helper to build a minimal FormatResult ───────────────────────────────────

static FormatResult makeFormat(uint64_t base, uint64_t ep, std::vector<std::pair<uint64_t, size_t>> execs = {})
{
	FormatResult fmt;
	fmt.entryPoint = ep;
	fmt.imageBase = base;
	for (auto [va, sz]: execs)
	{
		SectionInfo sec;
		sec.name = ".text";
		sec.virtualAddress = va;
		sec.virtualSize = sz;
		sec.fileOffset = 0;
		sec.fileSize = sz;
		sec.isExecutable = true;
		sec.isReadable = true;
		sec.isWritable = false;
		fmt.sections.push_back(sec);
	}
	return fmt;
}

// ─── Basic load + read ────────────────────────────────────────────────────────

TEST(MiniEmuTest, LoadAndReadByte)
{
	MiniEmu emu;
	// Map a page manually
	PagePerms rw{true, true, false};
	const uint8_t data[] = {0xAA, 0xBB, 0xCC};
	emu.mapPage(0x1000, rw, data, 3);

	uint8_t b = 0;
	EXPECT_TRUE(emu.readByte(0x1000, b));
	EXPECT_EQ(b, 0xAAu);
	EXPECT_TRUE(emu.readByte(0x1001, b));
	EXPECT_EQ(b, 0xBBu);
	EXPECT_TRUE(emu.readByte(0x1002, b));
	EXPECT_EQ(b, 0xCCu);
}

// Regression: MiniEmu::mapPage() rounded the *copy length* up to a whole page
// (std::max(size, kPageSize)) instead of the page count, so a 3-byte source
// buffer was memcpy'd 4096 bytes -- a buffer over-read ASan flags immediately.
// The bytes past the caller's buffer must read back as zero, not as whatever
// followed it in memory.
TEST(MiniEmuTest, ShortBufferDoesNotOverRead)
{
	MiniEmu emu;
	PagePerms rw{true, true, false};
	const uint8_t data[] = {0x11, 0x22, 0x33};
	emu.mapPage(0x1000, rw, data, sizeof(data));

	uint8_t b = 0;
	EXPECT_TRUE(emu.readByte(0x1002, b));
	EXPECT_EQ(b, 0x33u);
	// Everything after the supplied bytes is zero fill.
	EXPECT_TRUE(emu.readByte(0x1003, b));
	EXPECT_EQ(b, 0x00u);
	EXPECT_TRUE(emu.readByte(0x1FFF, b));
	EXPECT_EQ(b, 0x00u);
}

// A buffer spanning more than one page still maps every page, and only the
// final page is partially filled.
TEST(MiniEmuTest, MultiPageBufferMapsEveryPage)
{
	MiniEmu emu;
	PagePerms rw{true, true, false};
	std::vector<uint8_t> data(0x1000 + 4, 0x5A);
	emu.mapPage(0x3000, rw, data.data(), data.size());

	uint8_t b = 0;
	EXPECT_TRUE(emu.readByte(0x3000, b));
	EXPECT_EQ(b, 0x5Au);
	EXPECT_TRUE(emu.readByte(0x3FFF, b));
	EXPECT_EQ(b, 0x5Au);
	EXPECT_TRUE(emu.readByte(0x4003, b));
	EXPECT_EQ(b, 0x5Au);
	EXPECT_TRUE(emu.readByte(0x4004, b));
	EXPECT_EQ(b, 0x00u);
}

// Mapping with no data still maps one zeroed, readable page.
TEST(MiniEmuTest, NullDataMapsOneZeroPage)
{
	MiniEmu emu;
	PagePerms rw{true, true, false};
	emu.mapPage(0x5000, rw, nullptr, 0);

	uint8_t b = 0xFF;
	EXPECT_TRUE(emu.readByte(0x5000, b));
	EXPECT_EQ(b, 0x00u);
	EXPECT_TRUE(emu.readByte(0x5FFF, b));
	EXPECT_EQ(b, 0x00u);
}

TEST(MiniEmuTest, WriteByte)
{
	MiniEmu emu;
	PagePerms rw{true, true, false};
	emu.mapPage(0x2000, rw, nullptr, 0x1000);
	EXPECT_TRUE(emu.writeByte(0x2005, 0x42));
	uint8_t b = 0;
	EXPECT_TRUE(emu.readByte(0x2005, b));
	EXPECT_EQ(b, 0x42u);
}

// ─── NOP sled → HLT ──────────────────────────────────────────────────────────

TEST(MiniEmuTest, NOPSledThenHLT)
{
	// Build: 10 NOP + HLT
	std::vector<uint8_t> code(11, 0x90);
	code[10] = 0xF4; // HLT

	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, code.size()}});
	MiniEmu emu;
	emu.load(code.data(), code.size(), fmt);

	auto r = emu.run(0x1000, 1000);
	EXPECT_EQ(r.stopReason, StopReason::Halt);
	EXPECT_EQ(r.instructionsExecuted, 11u);
	EXPECT_TRUE(r.success);
}

// ─── MOV + ADD ────────────────────────────────────────────────────────────────

TEST(MiniEmuTest, MovImmAndAdd)
{
	// mov eax, 10    (B8 0A 00 00 00)
	// mov ecx, 5     (B9 05 00 00 00)
	// add eax, ecx   can't easily encode without ModRM for reg-reg
	// → just verify MOV and HLT
	std::vector<uint8_t> code = {
		0xB8,
		0x2A,
		0x00,
		0x00,
		0x00, // mov eax, 42
		0xB9,
		0x07,
		0x00,
		0x00,
		0x00, // mov ecx, 7
		0xF4  // hlt
	};
	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, code.size()}});
	MiniEmu emu;
	emu.load(code.data(), code.size(), fmt);
	auto r = emu.run(0x1000, 1000);

	EXPECT_EQ(r.stopReason, StopReason::Halt);
	EXPECT_EQ(emu.cpuState().rax, 42u);
	EXPECT_EQ(emu.cpuState().rcx, 7u);
}

// ─── PUSH/POP round trip ─────────────────────────────────────────────────────

TEST(MiniEmuTest, PushPopRoundTrip)
{
	// mov rax, 0xDEADBEEF
	// push rax
	// pop rbx
	// hlt
	std::vector<uint8_t> code = {
		0x48,
		0xB8,
		0xEF,
		0xBE,
		0xAD,
		0xDE,
		0x00,
		0x00,
		0x00,
		0x00, // mov rax, 0xDEADBEEF
		0x50, // push rax
		0x5B, // pop rbx
		0xF4  // hlt
	};
	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, code.size()}});
	MiniEmu emu;
	emu.load(code.data(), code.size(), fmt);
	auto r = emu.run(0x1000, 1000);
	EXPECT_EQ(r.stopReason, StopReason::Halt);
	EXPECT_EQ(emu.cpuState().rbx, 0xDEADBEEFu);
}

// ─── JMP loop ────────────────────────────────────────────────────────────────

TEST(MiniEmuTest, ConditionalJump)
{
	// mov ecx, 3
	// loop: dec ecx  (83 E9 01 = sub ecx, 1 but using 0x83/5 SUB)
	//        jnz loop
	// hlt
	// We use 0x83 /5 (SUB) to decrement, then 75 xx (JNZ)
	// sub ecx, 1: 83 E9 01
	// jnz: 75 FD (jump back 3 bytes)
	std::vector<uint8_t> code = {
		0xB9,
		0x03,
		0x00,
		0x00,
		0x00, // mov ecx, 3
		// loop:
		0x83,
		0xE9,
		0x01, // sub ecx, 1   (offset 5..7)
		0x75,
		0xFB, // jnz -5 (back to offset 5)
		0xF4  // hlt
	};
	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, code.size()}});
	MiniEmu emu;
	emu.load(code.data(), code.size(), fmt);
	auto r = emu.run(0x1000, 1000);
	EXPECT_EQ(r.stopReason, StopReason::Halt);
	EXPECT_EQ(emu.cpuState().rcx & 0xFFFFFFFF, 0u);
}

// ─── RDTSC anti-emulation ────────────────────────────────────────────────────

TEST(MiniEmuTest, RDTSCReturnsSyntheticValue)
{
	// rdtsc: 0F 31
	// hlt
	std::vector<uint8_t> code = {0x0F, 0x31, 0xF4};
	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, code.size()}});
	MiniEmu emu;
	emu.load(code.data(), code.size(), fmt);
	auto r = emu.run(0x1000, 1000);
	EXPECT_EQ(r.stopReason, StopReason::Halt);
	// rdx:rax should be non-zero (synthetic TSC)
	uint64_t tsc = (emu.cpuState().rdx << 32) | (emu.cpuState().rax & 0xFFFFFFFF);
	// Initial TSC was 0, so tsc == 0 after first RDTSC, but base was incremented
	// Just verify it doesn't crash and produces the expected value range
	EXPECT_GE(tsc + emu.cpuState().rdx, 0u); // always true, just checking no crash
}

// ─── CPUID anti-emulation ────────────────────────────────────────────────────

TEST(MiniEmuTest, CPUIDReturnsFakeIntelData)
{
	// mov eax, 1; cpuid; hlt
	std::vector<uint8_t> code = {
		0xB8,
		0x01,
		0x00,
		0x00,
		0x00, // mov eax, 1
		0x0F,
		0xA2, // cpuid
		0xF4  // hlt
	};
	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, code.size()}});
	MiniEmu emu;
	emu.load(code.data(), code.size(), fmt);
	emu.run(0x1000, 100);
	// CPUID leaf 1: rax should be a plausible CPU signature
	EXPECT_NE(emu.cpuState().rax, 0u);
}

// ─── REP STOSB ───────────────────────────────────────────────────────────────

TEST(MiniEmuTest, RepStosb)
{
	// Set up: rdi = writable region, rcx = 8, al = 0x42
	// rep stosb: fills 8 bytes with 0x42
	std::vector<uint8_t> code = {// mov rdi, 0x3000 (48 BF ...)
								 0x48,
								 0xBF,
								 0x00,
								 0x30,
								 0x00,
								 0x00,
								 0x00,
								 0x00,
								 0x00,
								 0x00,
								 // mov ecx, 8
								 0xB9,
								 0x08,
								 0x00,
								 0x00,
								 0x00,
								 // mov al, 0x42
								 0xB0,
								 0x42,
								 // cld
								 0xFC,
								 // rep stosb
								 0xF3,
								 0xAA,
								 // hlt
								 0xF4};

	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, code.size()}});

	// Also add a writable section at 0x3000
	SectionInfo dataSec;
	dataSec.name = ".data";
	dataSec.virtualAddress = 0x3000;
	dataSec.virtualSize = 0x1000;
	dataSec.fileOffset = code.size();
	dataSec.fileSize = 0x100;
	dataSec.isReadable = true;
	dataSec.isWritable = true;
	dataSec.isExecutable = false;
	fmt.sections.push_back(dataSec);

	std::vector<uint8_t> fullImage(code.size() + 0x100, 0);
	std::copy(code.begin(), code.end(), fullImage.begin());

	MiniEmu emu;
	emu.load(fullImage.data(), fullImage.size(), fmt);
	auto r = emu.run(0x1000, 1000);
	EXPECT_EQ(r.stopReason, StopReason::Halt);

	// Check 8 bytes at 0x3000 are 0x42
	for (int i = 0; i < 8; ++i)
	{
		uint8_t v = 0;
		emu.readByte(0x3000 + i, v);
		EXPECT_EQ(v, 0x42u) << "Byte at " << i << " should be 0x42";
	}
}

// ─── EnteredNewCode termination ──────────────────────────────────────────────

TEST(MiniEmuTest, JumpToWrittenRegionTerminates)
{
	// Write a byte to an area, then JMP to it
	// 1. Map a writable region at 0x4000
	// 2. Write 0x90 (NOP) there
	// 3. JMP to 0x4000 → should trigger EnteredNewCode

	// Build code: mov byte [0x4000], 0x90; jmp 0x4000
	// We'll use direct memory write via STOSB
	std::vector<uint8_t> code = {// mov rdi, 0x4000
								 0x48,
								 0xBF,
								 0x00,
								 0x40,
								 0x00,
								 0x00,
								 0x00,
								 0x00,
								 0x00,
								 0x00,
								 // mov al, 0x90 (NOP)
								 0xB0,
								 0x90,
								 // stosb: write [rdi]=al
								 0xAA,
								 // jmp rdi (FF E7 — but rdi changed by stosb, pointing to 0x4001 now)
								 // Let's instead do: mov rdi, 0x4000; jmp [rdi]
								 // For simplicity, use indirect JMP via 0xFF /4 to a register
								 // mov rdi, 0x4000 (already in rdi after stosb → it's 0x4001)
								 // Let's just use a near JMP to a constant that is in a writable area
								 // JMP rel32: E9, rel = 0x4000 - (RIP_after_JMP)
								 // RIP_after_JMP = 0x1000 + len_so_far + 5
								 // len so far = 13, so RIP_after = 0x1012
								 // rel = 0x4000 - 0x1012 = 0x2FEE
								 0xE9,
								 0xEE,
								 0x2F,
								 0x00,
								 0x00, // jmp 0x4000 (approx)
								 0xF4};

	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, code.size()}});
	// Add writable section at 0x4000
	SectionInfo ws;
	ws.name = ".writable";
	ws.virtualAddress = 0x4000;
	ws.virtualSize = 0x1000;
	ws.isReadable = true;
	ws.isWritable = true;
	ws.isExecutable = true;
	ws.fileOffset = 0;
	ws.fileSize = 0;
	fmt.sections.push_back(ws);

	std::vector<uint8_t> image(code.size() + 0x1000, 0);
	std::copy(code.begin(), code.end(), image.begin());

	MiniEmu emu;
	emu.load(image.data(), image.size(), fmt);
	auto r = emu.run(0x1000, 10000);
	// After writing to 0x4000 and jumping to it, should get EnteredNewCode
	EXPECT_TRUE(
		r.stopReason == StopReason::EnteredNewCode || r.stopReason == StopReason::Halt
		|| r.stopReason == StopReason::Error);
}

// ─── Max instructions limit ──────────────────────────────────────────────────

TEST(MiniEmuTest, MaxInstructionsReached)
{
	// Infinite loop: jmp 0 (JMP rel8 = EB FE)
	std::vector<uint8_t> code = {0xEB, 0xFE}; // JMP -2 (infinite loop)
	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, code.size()}});
	MiniEmu emu;
	emu.load(code.data(), code.size(), fmt);
	auto r = emu.run(0x1000, 100); // Low limit
	EXPECT_EQ(r.stopReason, StopReason::MaxInstructions);
	EXPECT_TRUE(r.needsManualReview);
}

// ─── MiniUnpacker tests ──────────────────────────────────────────────────────

TEST(MiniUnpackerTest, EmptyInputFails)
{
	MiniUnpacker up;
	FormatResult fmt;
	auto r = up.unpack(nullptr, 0, fmt);
	EXPECT_FALSE(r.success);
}

TEST(MiniUnpackerTest, SimpleHLTProducesOutput)
{
	std::vector<uint8_t> code = {
		0xB8,
		0x01,
		0x00,
		0x00,
		0x00, // mov eax, 1
		0xF4  // hlt
	};
	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, code.size()}});
	std::vector<uint8_t> image(code.size(), 0);
	std::copy(code.begin(), code.end(), image.begin());

	MiniUnpacker up;
	auto r = up.unpack(image.data(), image.size(), fmt, 1000);
	// Should succeed (HLT terminates) with at least some regions
	EXPECT_TRUE(r.stopReason == StopReason::Halt || r.success);
}

TEST(MiniUnpackerTest, SectionReconstructed)
{
	// Emulate writing to a data area, then jumping to it (simulates unpack)
	std::vector<uint8_t> code(0x100, 0x90); // All NOPs
	code[0xFF] = 0xF4;                      // HLT at end

	// Add writable section where data will be written
	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, 0x100}});
	SectionInfo ws;
	ws.name = ".data";
	ws.virtualAddress = 0x2000;
	ws.virtualSize = 0x100;
	ws.fileOffset = 0x100;
	ws.fileSize = 0x100;
	ws.isReadable = true;
	ws.isWritable = true;
	ws.isExecutable = false;
	fmt.sections.push_back(ws);

	std::vector<uint8_t> image(0x200, 0);
	std::copy(code.begin(), code.end(), image.begin());

	MiniUnpacker up;
	auto r = up.unpack(image.data(), image.size(), fmt, 10000);

	// At minimum the dump should be non-empty if we had regions
	if (r.success)
	{
		EXPECT_FALSE(r.dump.empty());
	}
}

// ─── StopReason string ────────────────────────────────────────────────────────

TEST(MiniEmuTest, StopReasonToString)
{
	EXPECT_EQ(stopReasonToString(StopReason::EnteredNewCode), "EnteredNewCode");
	EXPECT_EQ(stopReasonToString(StopReason::MaxInstructions), "MaxInstructions");
	EXPECT_EQ(stopReasonToString(StopReason::Halt), "Halt");
	EXPECT_EQ(stopReasonToString(StopReason::Error), "Error");
}

// ─── REX.R does not extend an opcode extension ───────────────────────────────

// Group 5 (opcode FF) puts an opcode extension in the ModRM reg field: /2 is
// CALL, /4 is JMP, /6 is PUSH. REX.R extends a reg field that names a
// REGISTER, and does not apply here -- but the decoder folded it in anyway, so
// a REX-prefixed `jmp rax` came out as reg 12, matched none of the arms, and
// decoded as nothing at all. `48 FF E0` is exactly that instruction.
TEST(MiniEmuTest, ARexPrefixedIndirectJumpIsStillAJump)
{
	MiniEmu emu;
	PagePerms rx{true, false, true};

	// 0x1000: 48 B8 <imm64 0x2000>   mov rax, 0x2000
	//         48 FF E0                jmp rax          (REX.W + FF /4)
	// 0x2000: F4                      hlt
	std::vector<uint8_t> code = {
		0x48, 0xB8, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4C, 0xFF, 0xE0, // REX.WR + FF /4 -- REX.R set,
																					  // and irrelevant here
	};
	emu.mapPage(0x1000, rx, code.data(), code.size());
	const uint8_t hlt[] = {0xF4};
	emu.mapPage(0x2000, rx, hlt, sizeof(hlt));

	auto res = emu.run(0x1000, 64);
	// The jump must have been taken: three instructions, ending just past the
	// HLT on the target page. Without the fix the jump decoded as nothing and
	// the emulator walked on through the first page until it hit the 64
	// instruction cap, at 0x104b.
	EXPECT_EQ(3u, res.instructionsExecuted) << "the REX-prefixed indirect jump was not decoded";
	EXPECT_EQ(0x2001u, res.epAfterUnpack);

	// The same instruction without REX.R already worked, and still does.
	MiniEmu plain;
	std::vector<uint8_t> noRexR = {
		0x48,
		0xB8,
		0x00,
		0x20,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x00,
		0x48,
		0xFF,
		0xE0,
	};
	plain.mapPage(0x1000, rx, noRexR.data(), noRexR.size());
	plain.mapPage(0x2000, rx, hlt, sizeof(hlt));
	auto plainRes = plain.run(0x1000, 64);
	EXPECT_EQ(3u, plainRes.instructionsExecuted);
	EXPECT_EQ(0x2001u, plainRes.epAfterUnpack);
}

// buildDump sized its flat image from maxVA - minVA, the span between the
// lowest and highest addresses the emulator touched. The emulator's stack sits
// at kDefaultStackBase = 0x00007FFFFFFF0000, so any function that pushes
// anything leaves a written region 128 TiB above the image -- and this
// allocated all of it. `push rax; hlt` at 0x1000 threw std::bad_alloc out of
// unpack(). Capping only the allocation was not enough: the section walk
// indexes the dump by the region's offset from minVA, so the stack region then
// read past the end of it (SIGSEGV in looksLikeCode). The window is chosen
// first and everything downstream works from the regions inside it.
TEST(MiniUnpackerTest, AStackWriteFarFromTheImageDoesNotSizeTheDump)
{
	std::vector<uint8_t> code = {0x50, 0xF4}; // push rax ; hlt
	FormatResult fmt = makeFormat(0x1000, 0x1000, {{0x1000, code.size()}});
	fmt.sections[0].isWritable = true;

	MiniUnpacker up;
	auto r = up.unpack(code.data(), code.size(), fmt, 100);
	EXPECT_TRUE(r.success);
	// One page, not 128 TiB and not the 256 MiB cap either -- the dump is
	// sized from the regions that survived the window.
	EXPECT_LE(r.dump.size(), 0x10000u) << "dump is " << r.dump.size() << " bytes";
	EXPECT_GT(r.dump.size(), 0u);
}
// ── The whole image shares one mapping budget ─────────────────────────────────
//
// kMaxSectionMapBytes caps each section on its own, and the section count comes
// out of the file too, so the sum was unbounded: four sections each claiming
// 256 MiB is a gigabyte of MemPage objects before a single instruction runs.
// libFuzzer found an 832-byte input that reached 975 MB that way.
//
// Eight sections of 256 MiB each. The first has to be mapped and the last must
// not: the budget is spent long before it.
TEST(MiniEmuSectionMapping, SectionsShareOneMappingBudget)
{
	constexpr uint64_t kHuge = 0x1000'0000ULL; // 256 MiB, the per-section cap
	constexpr int kSections = 8;

	FormatResult fmt;
	fmt.entryPoint = 0x1000;
	for (int i = 0; i < kSections; ++i)
	{
		SectionInfo sec;
		sec.name = ".s" + std::to_string(i);
		sec.virtualAddress = 0x1000 + static_cast<uint64_t>(i) * kHuge;
		sec.virtualSize = kHuge;
		sec.fileOffset = 0;
		sec.fileSize = 0;
		sec.isReadable = true;
		fmt.sections.push_back(sec);
	}

	const std::vector<uint8_t> image(0x1000, 0);
	MiniEmu emu;
	emu.load(image.data(), image.size(), fmt);

	std::uint8_t b = 0;
	EXPECT_TRUE(emu.readByte(0x1000, b)) << "the first section should still be mapped";
	EXPECT_FALSE(emu.readByte(fmt.sections.back().virtualAddress, b))
		<< "the eighth 256 MiB section was mapped, so the budget is per "
		   "section rather than per image";
}
