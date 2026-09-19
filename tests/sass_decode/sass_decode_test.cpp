/**
 * @file tests/sass_decode/sass_decode_test.cpp
 * @brief Unit tests for cubin/fatbin load and documented SASS subset decode.
 *
 * No CUDA toolkit blobs. Instruction bytes are copied from NVIDIA CUDA
 * Binary Utilities printed encodings.
 */

#include "retdec/sass_decode/cubin_loader.h"
#include "retdec/sass_decode/sass_decoder.h"
#include "retdec/sass_decode/sass_lifter.h"
#include "retdec/sass_decode/sass_types.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace retdec::sass_decode;

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x)
{
	v.push_back(uint8_t(x));
	v.push_back(uint8_t(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x)
{
	for (int i = 0; i < 4; ++i)
	{
		v.push_back(uint8_t(x >> (8 * i)));
	}
}
void put64(std::vector<uint8_t>& v, uint64_t x)
{
	for (int i = 0; i < 8; ++i)
	{
		v.push_back(uint8_t(x >> (8 * i)));
	}
}
void putAt16(std::vector<uint8_t>& v, size_t off, uint16_t x)
{
	v[off] = uint8_t(x);
	v[off + 1] = uint8_t(x >> 8);
}
void putAt32(std::vector<uint8_t>& v, size_t off, uint32_t x)
{
	for (int i = 0; i < 4; ++i)
	{
		v[off + i] = uint8_t(x >> (8 * i));
	}
}
void putAt64(std::vector<uint8_t>& v, size_t off, uint64_t x)
{
	for (int i = 0; i < 8; ++i)
	{
		v[off + i] = uint8_t(x >> (8 * i));
	}
}

void le64bytes(std::vector<uint8_t>& v, uint64_t w)
{
	put64(v, w);
}

// NVIDIA CUDA Binary Utilities 12.8.2 / 13.3 printed words (little-endian store).
void appendExitSm70(std::vector<uint8_t>& v)
{
	le64bytes(v, 0x000000000000794dULL);
	le64bytes(v, 0x000fea0003800000ULL);
}
void appendNopSm70(std::vector<uint8_t>& v)
{
	le64bytes(v, 0x0000000000007918ULL);
	le64bytes(v, 0x000fc00000000000ULL);
}
void appendIadd3(std::vector<uint8_t>& v)
{
	// IADD3 R9, R2, R5, RZ  — CUDA Binary Utilities 12.8.2
	le64bytes(v, 0x0000000502097210ULL);
	le64bytes(v, 0x004fd00007ffe0ffULL);
}
void appendS2r(std::vector<uint8_t>& v)
{
	// S2R R9, SR_TID.X — CUDA Binary Utilities 13.3
	le64bytes(v, 0x0000000000097919ULL);
	le64bytes(v, 0x000e2e0000002100ULL);
}
void appendLdg(std::vector<uint8_t>& v)
{
	le64bytes(v, 0x0000000002027381ULL);
	le64bytes(v, 0x000ea800001ee900ULL);
}
void appendStg(std::vector<uint8_t>& v)
{
	le64bytes(v, 0x0000000906007386ULL);
	le64bytes(v, 0x000fe2000010e900ULL);
}
void appendFadd(std::vector<uint8_t>& v)
{
	le64bytes(v, 0x0000000502097221ULL);
	le64bytes(v, 0x004fca0000000000ULL);
}
void appendImadMov1282(std::vector<uint8_t>& v)
{
	// IMAD.MOV.U32 R1, RZ, RZ, c[0x0][0x28] — CUDA Binary Utilities 12.8.2
	le64bytes(v, 0x00000a00ff017624ULL);
	le64bytes(v, 0x000fd000078e00ffULL);
}
void appendMov1282(std::vector<uint8_t>& v)
{
	// MOV R3, c[0x0][0x164] — CUDA Binary Utilities 12.8.2
	le64bytes(v, 0x0000590000037a02ULL);
	le64bytes(v, 0x000fe20000000f00ULL);
}
void appendBra1282(std::vector<uint8_t>& v)
{
	// BRA 0xd0 — CUDA Binary Utilities 12.8.2
	le64bytes(v, 0xfffffff000007947ULL);
	le64bytes(v, 0x000fc0000383ffffULL);
}
void appendShfl1282(std::vector<uint8_t>& v)
{
	// @!PT SHFL.IDX PT, RZ, RZ, RZ, RZ — CUDA Binary Utilities 12.8.2
	le64bytes(v, 0x000000fffffff389ULL);
	le64bytes(v, 0x000fe200000e00ffULL);
}
void appendImadWide133(std::vector<uint8_t>& v)
{
	// IMAD.WIDE R2, R9, 0x4, R2 — CUDA Binary Utilities 13.3
	le64bytes(v, 0x0000000409027825ULL);
	le64bytes(v, 0x004fcc00078e0202ULL);
}
void appendImadUr133(std::vector<uint8_t>& v)
{
	// IMAD R9, R0, UR6, R9 — CUDA Binary Utilities 13.3
	le64bytes(v, 0x0000000600097c24ULL);
	le64bytes(v, 0x001fce000f8e0209ULL);
}
void appendS2ur133(std::vector<uint8_t>& v)
{
	// S2UR UR6, SR_CTAID.X — CUDA Binary Utilities 13.3
	le64bytes(v, 0x00000000000679c3ULL);
	le64bytes(v, 0x000e220000002500ULL);
}
void appendLdcu133(std::vector<uint8_t>& v)
{
	// LDCU.64 UR4, c[0x0][0x358] — CUDA Binary Utilities 13.3
	le64bytes(v, 0x00006b00ff0477acULL);
	le64bytes(v, 0x000e6e0008000a00ULL);
}
void appendLdg7981(std::vector<uint8_t>& v)
{
	// LDG.E R2, desc[UR4][R2.64] — CUDA Binary Utilities 13.3
	le64bytes(v, 0x0000000402027981ULL);
	le64bytes(v, 0x002ea2000c1e1900ULL);
}
void appendStg7986(std::vector<uint8_t>& v)
{
	// STG.E desc[UR4][R6.64], R9 — CUDA Binary Utilities 13.3
	le64bytes(v, 0x0000000906007986ULL);
	le64bytes(v, 0x000fe2000c101904ULL);
}
void appendBra133(std::vector<uint8_t>& v)
{
	// BRA 0x110 — CUDA Binary Utilities 13.3
	le64bytes(v, 0xfffffffc00fc7947ULL);
	le64bytes(v, 0x000fc0000383ffffULL);
}
void appendLdc64_133(std::vector<uint8_t>& v)
{
	// LDC.64 R2, c[0x0][0x380] — CUDA Binary Utilities 13.3
	le64bytes(v, 0x0000e000ff027b82ULL);
	le64bytes(v, 0x000eb00000000a00ULL);
}
void appendImadMovR2_1282(std::vector<uint8_t>& v)
{
	// IMAD.MOV.U32 R2, RZ, RZ, c[0x0][0x160] — CUDA Binary Utilities 12.8.2
	le64bytes(v, 0x00005800ff027624ULL);
	le64bytes(v, 0x000fe200078e00ffULL);
}
void appendLdgR5_1282(std::vector<uint8_t>& v)
{
	// LDG.E.SYS R5, [R4] — CUDA Binary Utilities 12.8.2
	le64bytes(v, 0x0000000004057381ULL);
	le64bytes(v, 0x000ea200001ee900ULL);
}
void appendImadWideR4_133(std::vector<uint8_t>& v)
{
	// IMAD.WIDE R4, R9, 0x4, R4 — CUDA Binary Utilities 13.3
	le64bytes(v, 0x0000000409047825ULL);
	le64bytes(v, 0x008fcc00078e0204ULL);
}

std::vector<uint8_t> makeCubin(bool elf64, uint32_t sm, const std::string& secName, const std::vector<uint8_t>& text)
{
	std::vector<uint8_t> out;
	const uint16_t shnum = 3;
	const uint16_t shstrndx = 2;
	const uint16_t ehsize = elf64 ? 64 : 52;
	const uint16_t shentsize = elf64 ? 64 : 40;

	out.resize(ehsize, 0);
	out[0] = 0x7f;
	out[1] = 'E';
	out[2] = 'L';
	out[3] = 'F';
	out[4] = elf64 ? 2 : 1;
	out[5] = 1;
	out[6] = 1;
	out[7] = 0;
	out[8] = 7; // CUDA ABI v1
	putAt16(out, 16, 2); // ET_EXEC
	putAt16(out, 18, kEmCuda);
	putAt32(out, 20, 1);
	if (elf64)
	{
		putAt16(out, 52, ehsize);
		putAt16(out, 58, shentsize);
		putAt16(out, 60, shnum);
		putAt16(out, 62, shstrndx);
		putAt32(out, 48, sm); // e_flags
	}
	else
	{
		putAt16(out, 40, ehsize);
		putAt16(out, 46, shentsize);
		putAt16(out, 48, shnum);
		putAt16(out, 50, shstrndx);
		putAt32(out, 36, sm);
	}

	const uint64_t textOff = out.size();
	out.insert(out.end(), text.begin(), text.end());

	std::string shstr;
	shstr.push_back('\0');
	const uint32_t textNameOff = static_cast<uint32_t>(shstr.size());
	shstr += secName;
	shstr.push_back('\0');
	const uint32_t shstrNameOff = static_cast<uint32_t>(shstr.size());
	shstr += ".shstrtab";
	shstr.push_back('\0');
	const uint64_t shstrOff = out.size();
	out.insert(out.end(), shstr.begin(), shstr.end());

	while (out.size() % 8)
	{
		out.push_back(0);
	}
	const uint64_t shoff = out.size();

	auto emitShdr = [&](uint32_t name, uint32_t type, uint64_t flags, uint64_t off, uint64_t sz) {
		std::vector<uint8_t> sh(shentsize, 0);
		auto w32 = [&](size_t o, uint32_t x) {
			for (int i = 0; i < 4; ++i)
				sh[o + i] = uint8_t(x >> (8 * i));
		};
		auto w64 = [&](size_t o, uint64_t x) {
			for (int i = 0; i < 8; ++i)
				sh[o + i] = uint8_t(x >> (8 * i));
		};
		if (elf64)
		{
			w32(0, name);
			w32(4, type);
			w64(8, flags);
			w64(24, off);
			w64(32, sz);
			w64(48, 1); // addralign
		}
		else
		{
			w32(0, name);
			w32(4, type);
			w32(8, uint32_t(flags));
			w32(16, uint32_t(off));
			w32(20, uint32_t(sz));
			w32(32, 1);
		}
		out.insert(out.end(), sh.begin(), sh.end());
	};

	emitShdr(0, 0, 0, 0, 0);
	emitShdr(textNameOff, 1 /* PROGBITS */, 6 /* ALLOC|EXEC */, textOff, text.size());
	emitShdr(shstrNameOff, 3 /* STRTAB */, 0, shstrOff, shstr.size());

	if (elf64)
	{
		putAt64(out, 40, shoff);
	}
	else
	{
		putAt32(out, 32, uint32_t(shoff));
	}
	return out;
}

std::vector<uint8_t> makeFatbin(const std::vector<uint8_t>& payload)
{
	std::vector<uint8_t> out;
	put32(out, kFatbinMagic);
	put16(out, 1);  // version
	put16(out, 16); // headerSize
	put64(out, payload.size());
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

} // namespace

TEST(SassTypes, EmCudaMatchesElfio)
{
	EXPECT_EQ(kEmCuda, 190);
}

TEST(SassTypes, SmFromEflagsAbiV1)
{
	EXPECT_EQ(smFromEflags(0x50, 7), 80u);
	EXPECT_EQ(smFromEflags(0x46, 7), 70u);
	EXPECT_EQ(smFromEflags(0x14, 7), 20u);
	EXPECT_TRUE(isKnownSm(80));
	EXPECT_EQ(smName(80), "sm_80");
}

TEST(SassTypes, SmFromEflagsAbiV2Shifted)
{
	EXPECT_EQ(smFromEflags(0x5000, 8), 80u);
}

TEST(SassDecoder, ExitNopFromNvidiaListing)
{
	SassDecoder d(80, 64);
	std::vector<uint8_t> bytes;
	appendExitSm70(bytes);
	appendNopSm70(bytes);
	auto ins = d.decodeStream(bytes);
	ASSERT_EQ(ins.size(), 2u);
	EXPECT_EQ(ins[0].opcode, SassOpcode::Exit);
	EXPECT_TRUE(ins[0].decoded);
	EXPECT_EQ(ins[0].size, 16u);
	EXPECT_EQ(ins[0].pred, 7);
	EXPECT_EQ(ins[1].opcode, SassOpcode::Nop);
	EXPECT_EQ(ins[1].pc, 16u);
}

TEST(SassDecoder, Iadd3OperandsFromNvidiaListing)
{
	SassDecoder d(80, 64);
	std::vector<uint8_t> bytes;
	appendIadd3(bytes);
	auto in = d.decodeBytes(bytes.data(), bytes.size(), 0);
	EXPECT_EQ(in.opcode, SassOpcode::Iadd3);
	EXPECT_EQ(in.dest, 9);
	EXPECT_EQ(in.src0, 2);
	EXPECT_EQ(in.src1, 5);
	EXPECT_EQ(in.src2, 255);
}

TEST(SassDecoder, S2rLdgStgFadd)
{
	SassDecoder d(80, 64);
	std::vector<uint8_t> bytes;
	appendS2r(bytes);
	appendLdg(bytes);
	appendStg(bytes);
	appendFadd(bytes);
	auto ins = d.decodeStream(bytes);
	ASSERT_EQ(ins.size(), 4u);
	EXPECT_EQ(ins[0].opcode, SassOpcode::S2r);
	EXPECT_EQ(ins[0].dest, 9);
	EXPECT_EQ(ins[1].opcode, SassOpcode::Ldg);
	EXPECT_EQ(ins[1].dest, 2);
	EXPECT_EQ(ins[2].opcode, SassOpcode::Stg);
	EXPECT_EQ(ins[2].src0, 6);
	EXPECT_EQ(ins[2].src1, 9);
	EXPECT_EQ(ins[3].opcode, SassOpcode::Fadd);
	EXPECT_EQ(ins[3].dest, 9);
}

TEST(SassDecoder, ImadMovAndMovFromNvidia1282)
{
	SassDecoder d(80, 64);
	std::vector<uint8_t> bytes;
	appendImadMov1282(bytes);
	appendMov1282(bytes);
	auto ins = d.decodeStream(bytes);
	ASSERT_EQ(ins.size(), 2u);
	EXPECT_EQ(ins[0].opcode, SassOpcode::Imad);
	EXPECT_EQ(ins[0].dest, 1);
	EXPECT_EQ(ins[0].src0, 255);
	EXPECT_EQ(ins[1].opcode, SassOpcode::Mov);
	EXPECT_EQ(ins[1].dest, 3);
	EXPECT_EQ(sassOpcodeName(ins[0].opcode), std::string("IMAD"));
	EXPECT_EQ(sassOpcodeName(ins[1].opcode), std::string("MOV"));
}

TEST(SassDecoder, BraShflFromNvidia1282)
{
	SassDecoder d(70, 64);
	std::vector<uint8_t> bytes;
	appendBra1282(bytes);
	appendShfl1282(bytes);
	auto ins = d.decodeStream(bytes);
	ASSERT_EQ(ins.size(), 2u);
	EXPECT_EQ(ins[0].opcode, SassOpcode::Bra);
	EXPECT_TRUE(ins[0].decoded);
	EXPECT_EQ(ins[1].opcode, SassOpcode::Shfl);
	EXPECT_EQ(ins[1].dest, 255);
	EXPECT_EQ(sassOpcodeName(ins[1].opcode), std::string("SHFL"));
}

TEST(SassDecoder, ImadWideS2urLdcuFromNvidia133)
{
	SassDecoder d(80, 64);
	std::vector<uint8_t> bytes;
	appendImadWide133(bytes);
	appendImadUr133(bytes);
	appendS2ur133(bytes);
	appendLdcu133(bytes);
	auto ins = d.decodeStream(bytes);
	ASSERT_EQ(ins.size(), 4u);
	EXPECT_EQ(ins[0].opcode, SassOpcode::ImadWide);
	EXPECT_EQ(ins[0].dest, 2);
	EXPECT_EQ(ins[0].src0, 9);
	EXPECT_EQ(ins[0].src1, 4);
	EXPECT_EQ(ins[0].src2, 2);
	EXPECT_EQ(ins[1].opcode, SassOpcode::Imad);
	EXPECT_EQ(ins[1].dest, 9);
	EXPECT_EQ(ins[1].src0, 0);
	EXPECT_EQ(ins[1].src1, 6);
	EXPECT_EQ(ins[1].src2, 9);
	EXPECT_EQ(ins[2].opcode, SassOpcode::S2ur);
	EXPECT_EQ(ins[2].dest, 6);
	EXPECT_EQ(ins[3].opcode, SassOpcode::Ldcu);
	EXPECT_EQ(ins[3].dest, 4);
	EXPECT_EQ(sassOpcodeName(ins[0].opcode), std::string("IMAD.WIDE"));
	EXPECT_EQ(sassOpcodeName(ins[2].opcode), std::string("S2UR"));
	EXPECT_EQ(sassOpcodeName(ins[3].opcode), std::string("LDCU"));
}

TEST(SassDecoder, LdgStgBraVariantsFromNvidia133)
{
	SassDecoder d(80, 64);
	std::vector<uint8_t> bytes;
	appendLdg7981(bytes);
	appendStg7986(bytes);
	appendBra133(bytes);
	auto ins = d.decodeStream(bytes);
	ASSERT_EQ(ins.size(), 3u);
	EXPECT_EQ(ins[0].opcode, SassOpcode::Ldg);
	EXPECT_EQ(ins[0].dest, 2);
	EXPECT_EQ(ins[1].opcode, SassOpcode::Stg);
	EXPECT_EQ(ins[1].src0, 6);
	EXPECT_EQ(ins[1].src1, 9);
	EXPECT_EQ(ins[2].opcode, SassOpcode::Bra);
}

TEST(SassDecoder, UnknownOpcodeStaysUnknown)
{
	SassDecoder d(80, 64);
	auto in = d.decodeWord(0x00000000000000ffULL, 0, 0);
	EXPECT_EQ(in.opcode, SassOpcode::Unknown);
	EXPECT_FALSE(in.decoded);
}

TEST(SassDecoder, ExtraPrintedVariantsFromNvidiaListings)
{
	SassDecoder d(80, 64);
	std::vector<uint8_t> bytes;
	appendLdc64_133(bytes);
	appendImadMovR2_1282(bytes);
	appendLdgR5_1282(bytes);
	appendImadWideR4_133(bytes);
	auto ins = d.decodeStream(bytes);
	ASSERT_EQ(ins.size(), 4u);
	EXPECT_EQ(ins[0].opcode, SassOpcode::Ldc);
	EXPECT_EQ(ins[0].dest, 2);
	EXPECT_EQ(ins[1].opcode, SassOpcode::Imad);
	EXPECT_EQ(ins[1].dest, 2);
	EXPECT_EQ(ins[1].src0, 255);
	EXPECT_EQ(ins[2].opcode, SassOpcode::Ldg);
	EXPECT_EQ(ins[2].dest, 5);
	EXPECT_EQ(ins[2].src0, 4);
	EXPECT_EQ(ins[3].opcode, SassOpcode::ImadWide);
	EXPECT_EQ(ins[3].dest, 4);
	EXPECT_EQ(ins[3].src0, 9);
	EXPECT_EQ(ins[3].src1, 4);
	EXPECT_EQ(ins[3].src2, 4);
}

TEST(SassDecoder, IsaTableMnemonicsWithoutPrintedWordStayUnknown)
{
	// CUDA Binary Utilities 12.8 / 13.x ISA tables name FMUL, FFMA, ISETP,
	// SHL, SHR, LOP3, FSETP, MUFU, LEA but print no 16-byte encoding word.
	// Low-8 values that NVIDIA did not print must stay Unknown.
	SassDecoder d(80, 64);
	const uint8_t unprinted[] = {0x20, 0x22, 0x23, 0x30, 0x31, 0x12, 0x13, 0x11};
	for (uint8_t op : unprinted)
	{
		auto in = d.decodeWord(uint64_t(op), 0, 0);
		EXPECT_EQ(in.opcode, SassOpcode::Unknown) << unsigned(op);
		EXPECT_FALSE(in.decoded) << unsigned(op);
	}
}

TEST(SassDecoder, PreVoltaEightByteExit)
{
	SassDecoder d(50, 32);
	EXPECT_EQ(d.instrSize(), 8u);
	// CUDA Binary Utilities 9.1 Fermi EXIT
	auto in = d.decodeWord(0x8000000000001de7ULL, 0, 0);
	EXPECT_EQ(in.opcode, SassOpcode::Exit);
	EXPECT_EQ(in.size, 8u);
}

TEST(SassDecoder, Sm70AndSm80ShareVoltaWordSize)
{
	EXPECT_EQ(sassInstrSize(70), 16u);
	EXPECT_EQ(sassInstrSize(80), 16u);
	EXPECT_EQ(sassInstrSize(20), 8u);
}

TEST(CubinLoader, RejectsNonCudaElf)
{
	std::vector<uint8_t> text(16, 0);
	auto elf = makeCubin(true, 80, ".text.k", text);
	elf[18] = 62; // EM_X86_64
	elf[19] = 0;
	EXPECT_FALSE(looksLikeCubin(elf.data(), elf.size()));
	auto mod = loadCubin(elf.data(), elf.size());
	EXPECT_FALSE(mod.ok());
}

TEST(CubinLoader, Elf64Sm80LoadsText)
{
	std::vector<uint8_t> text;
	appendIadd3(text);
	appendExitSm70(text);
	auto cubin = makeCubin(true, 0x50, ".text.vectorAdd", text);
	ASSERT_TRUE(looksLikeCubin(cubin.data(), cubin.size()));
	auto mod = loadCubin(cubin.data(), cubin.size());
	ASSERT_TRUE(mod.ok()) << mod.error;
	EXPECT_EQ(mod.sm, 80u);
	EXPECT_EQ(mod.pointerBits, 64u);
	EXPECT_TRUE(mod.elf64);
	ASSERT_EQ(mod.kernels.size(), 1u);
	EXPECT_EQ(mod.kernels[0].name, "vectorAdd");
	EXPECT_EQ(mod.kernels[0].bytes.size(), 32u);
}

TEST(CubinLoader, Elf32Is32BitPointers)
{
	std::vector<uint8_t> text;
	appendExitSm70(text);
	auto cubin = makeCubin(false, 0x50, ".text.k32", text);
	auto mod = loadCubin(cubin.data(), cubin.size());
	ASSERT_TRUE(mod.ok()) << mod.error;
	EXPECT_FALSE(mod.elf64);
	EXPECT_EQ(mod.pointerBits, 32u);
	ASSERT_EQ(mod.kernels.size(), 1u);
	EXPECT_EQ(mod.kernels[0].name, "k32");
}

TEST(CubinLoader, PointerBitsOverride)
{
	std::vector<uint8_t> text;
	appendExitSm70(text);
	auto cubin = makeCubin(true, 0x50, ".text.k", text);
	SassConfig cfg;
	cfg.pointerBits = 32;
	auto mod = loadCubin(cubin.data(), cubin.size(), cfg);
	ASSERT_TRUE(mod.ok()) << mod.error;
	EXPECT_EQ(mod.pointerBits, 32u);
}

TEST(FatbinLoader, WrapsCubin)
{
	std::vector<uint8_t> text;
	appendExitSm70(text);
	auto cubin = makeCubin(true, 0x50, ".text.k", text);
	auto fat = makeFatbin(cubin);
	ASSERT_TRUE(looksLikeFatbin(fat.data(), fat.size()));
	auto mod = loadFatbin(fat.data(), fat.size());
	ASSERT_TRUE(mod.ok()) << mod.error;
	EXPECT_EQ(mod.sm, 80u);
	ASSERT_FALSE(mod.kernels.empty());
}

TEST(FatbinLoader, ExtractsPtxWithoutCubin)
{
	const std::string ptx =
		".version 8.0\n"
		".target sm_80\n"
		".address_size 64\n"
		".visible .entry foo(.param .u32 n)\n"
		"{\n"
		"  ret;\n"
		"}\n";
	std::vector<uint8_t> payload(ptx.begin(), ptx.end());
	payload.push_back(0);
	auto fat = makeFatbin(payload);
	auto mod = loadFatbin(fat.data(), fat.size());
	ASSERT_TRUE(mod.ok()) << mod.error;
	EXPECT_NE(mod.ptxText.find(".target sm_80"), std::string::npos);
}

TEST(SassLifter, ExitAndIadd3ToCudaC)
{
	std::vector<uint8_t> text;
	appendIadd3(text);
	appendExitSm70(text);
	auto cubin = makeCubin(true, 0x50, ".text.vectorAdd", text);
	auto c = decompileCudaBinary(cubin.data(), cubin.size());
	EXPECT_NE(c.find("__global__ void vectorAdd"), std::string::npos);
	EXPECT_NE(c.find("r9 = r2 + r5 + 0"), std::string::npos);
	EXPECT_NE(c.find("return;"), std::string::npos);
	EXPECT_NE(c.find("64-bit GPU pointers"), std::string::npos);
	EXPECT_NE(c.find("NOT Production"), std::string::npos);
}

TEST(SassLifter, ThirtyTwoBitPointersInLoad)
{
	std::vector<uint8_t> text;
	appendLdg(text);
	appendExitSm70(text);
	auto cubin = makeCubin(false, 0x50, ".text.k", text);
	auto c = decompileCudaBinary(cubin.data(), cubin.size());
	EXPECT_NE(c.find("uint32_t"), std::string::npos);
	EXPECT_NE(c.find("32-bit GPU pointers"), std::string::npos);
}

TEST(SassLifter, ImadWideAndS2urToCudaC)
{
	std::vector<uint8_t> text;
	appendS2ur133(text);
	appendImadWide133(text);
	appendLdg7981(text);
	appendFadd(text);
	appendStg7986(text);
	appendExitSm70(text);
	auto cubin = makeCubin(true, 0x50, ".text.add", text);
	auto c = decompileCudaBinary(cubin.data(), cubin.size());
	EXPECT_NE(c.find("__global__ void add"), std::string::npos);
	EXPECT_NE(c.find("blockIdx.x"), std::string::npos);
	EXPECT_NE(c.find("IMAD.WIDE"), std::string::npos);
	EXPECT_NE(c.find("r2 = r9 * 4u + r2"), std::string::npos);
	EXPECT_NE(c.find("*(float*)&r9 = *(float*)&r2 + *(float*)&r5"), std::string::npos);
	EXPECT_NE(c.find("uint64_t"), std::string::npos);
	EXPECT_NE(c.find("64-bit GPU pointers"), std::string::npos);
	EXPECT_NE(c.find("return;"), std::string::npos);
	EXPECT_NE(c.find("NOT Production"), std::string::npos);
}

TEST(SassLifter, ShflAndBraFromNvidiaListings)
{
	std::vector<uint8_t> text;
	appendShfl1282(text);
	appendBra1282(text);
	appendExitSm70(text);
	auto cubin = makeCubin(true, 0x50, ".text.k", text);
	auto c = decompileCudaBinary(cubin.data(), cubin.size());
	EXPECT_NE(c.find("SHFL.IDX"), std::string::npos);
	EXPECT_NE(c.find("goto L_bra_"), std::string::npos);
	EXPECT_NE(c.find("return;"), std::string::npos);
}

TEST(SassLifter, FatbinPtxIsLabelledNotSass)
{
	const std::string ptx =
		".version 8.0\n"
		".target sm_80\n"
		".visible .entry foo()\n"
		"{\n"
		"  ret;\n"
		"}\n";
	std::vector<uint8_t> payload(ptx.begin(), ptx.end());
	payload.push_back(0);
	auto fat = makeFatbin(payload);
	auto c = decompileCudaBinary(fat.data(), fat.size());
	EXPECT_NE(c.find("embedded PTX (not SASS)"), std::string::npos);
}

TEST(SassLifter, UnknownWordIsHexComment)
{
	SassModule mod;
	mod.sm = 80;
	mod.pointerBits = 64;
	mod.elf64 = true;
	mod.eMachine = kEmCuda;
	SassKernel k;
	k.name = "u";
	SassDecoder d(80, 64);
	k.instrs.push_back(d.decodeWord(0x11111111111111ffULL, 0x2222222222222222ULL, 0));
	mod.kernels.push_back(k);
	auto c = SassLifter{}.liftModule(mod);
	EXPECT_NE(c.find("sass"), std::string::npos);
	EXPECT_NE(c.find("0x11111111111111ff"), std::string::npos);
}

TEST(LoadCudaBinary, EmptyIsError)
{
	auto mod = loadCudaBinary(nullptr, 0);
	EXPECT_FALSE(mod.ok());
}
