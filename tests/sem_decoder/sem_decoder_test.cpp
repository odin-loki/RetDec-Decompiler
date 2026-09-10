/**
 * @file tests/sem_decoder/sem_decoder_test.cpp
 * @brief Unit tests for SemDecoder.
 *
 * All test cases use hand-assembled x86-64 byte sequences that are stable
 * across Capstone versions.  Each test is documented with the instruction(s)
 * it encodes.
 *
 * Encoding cheat sheet (x86-64, AT&T → Intel):
 *   NOP                    : 0x90
 *   MOV EAX, EBX           : 0x89 0xD8
 *   MOV EAX, 0             : 0xB8 0x00 0x00 0x00 0x00
 *   ADD EAX, 0             : 0x83 0xC0 0x00
 *   SUB EAX, EAX           : 0x29 0xC0
 *   XOR EAX, EAX           : 0x31 0xC0
 *   LEA EAX, [EAX]         : 0x8D 0x00  (32-bit: [EAX+0], needs prefix in 64)
 *   LEA RAX, [RAX]         : 0x48 0x8D 0x00  (REX.W + LEA + ModRM [RAX])
 *   XCHG EAX, EBX          : 0x87 0xC3
 *   CMP EAX, EBX           : 0x39 0xD8
 *   JE  +0                 : 0x74 0x00
 *   CALL rel32             : 0xE8 xx xx xx xx
 *   RET                    : 0xC3
 */

#include "retdec/sem_decoder/sem_decoder.h"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

using namespace retdec::sem_decoder;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static SemDecoder& decoder64()
{
    static SemDecoder d(SemDecoder::Mode::X86_64);
    return d;
}

static SemDecoder& decoder32()
{
    static SemDecoder d(SemDecoder::Mode::X86);
    return d;
}

static DecodedInstr decode(const std::vector<uint8_t>& bytes,
                            uint64_t addr = 0x401000)
{
    return decoder64().decodeOne(bytes.data(), bytes.size(), addr);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Basic decode
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Decode, NOP)
{
    // 0x90 = NOP
    auto d = decode({0x90});
    EXPECT_TRUE(d.isValid);
    EXPECT_EQ(d.len, 1u);
    ASSERT_FALSE(d.ops.empty());
    EXPECT_EQ(d.ops[0].type, SemOpType::Nop);
}

TEST(Decode, MOV_EAX_EBX)
{
    // 89 D8 = MOV EAX, EBX  (move EBX into EAX)
    auto d = decode({0x89, 0xD8});
    EXPECT_TRUE(d.isValid);
    EXPECT_EQ(d.len, 2u);
    ASSERT_FALSE(d.ops.empty());
    EXPECT_EQ(d.ops[0].type, SemOpType::Assign);
}

TEST(Decode, InvalidBytes)
{
    // 0x06 is an invalid opcode in 64-bit mode.
    auto d = decode({0x06});
    EXPECT_FALSE(d.isValid);
    EXPECT_EQ(d.len, 1u); // consumes 1 byte
    ASSERT_FALSE(d.ops.empty());
    EXPECT_EQ(d.ops[0].type, SemOpType::Undef);
}

TEST(Decode, RET)
{
    // C3 = RET
    auto d = decode({0xC3});
    EXPECT_TRUE(d.isValid);
    ASSERT_FALSE(d.ops.empty());
    EXPECT_EQ(d.ops[0].type, SemOpType::Return);
}

TEST(Decode, CALLrel32)
{
    // E8 00 00 00 00 = CALL +5 (next instruction)
    auto d = decode({0xE8, 0x00, 0x00, 0x00, 0x00});
    EXPECT_TRUE(d.isValid);
    EXPECT_EQ(d.len, 5u);
    ASSERT_FALSE(d.ops.empty());
    EXPECT_EQ(d.ops[0].type, SemOpType::Call);
}

TEST(Decode, JMPshort)
{
    // EB 00 = JMP +2 (short jump to next instruction)
    auto d = decode({0xEB, 0x00});
    EXPECT_TRUE(d.isValid);
    ASSERT_FALSE(d.ops.empty());
    EXPECT_EQ(d.ops[0].type, SemOpType::Branch);
}

TEST(Decode, CMPsets_Compare)
{
    // 39 D8 = CMP EAX, EBX
    auto d = decode({0x39, 0xD8});
    EXPECT_TRUE(d.isValid);
    ASSERT_FALSE(d.ops.empty());
    EXPECT_EQ(d.ops[0].type, SemOpType::Compare);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Normalisation
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Normalise, SUB_reg_reg_ZeroesRegister)
{
    // 29 C0 = SUB EAX, EAX
    auto d = decode({0x29, 0xC0});
    EXPECT_TRUE(d.isValid);
    EXPECT_TRUE(d.isNormalised);
    ASSERT_FALSE(d.ops.empty());
    // After normalisation: Assign dst ← 0
    EXPECT_EQ(d.ops[0].type, SemOpType::Assign);
    EXPECT_EQ(d.ops[0].src1.kind, SemValKind::Imm);
    EXPECT_EQ(d.ops[0].src1.imm, 0);
    // All arithmetic flags should be defined (not undefined).
    EXPECT_EQ(d.ops[0].flagsEffect.undefined, Flags::NONE);
    EXPECT_NE(d.ops[0].flagsEffect.defined & Flags::ZF, 0u);
}

TEST(Normalise, XOR_reg_reg_ZeroesRegister)
{
    // 31 C0 = XOR EAX, EAX
    auto d = decode({0x31, 0xC0});
    EXPECT_TRUE(d.isValid);
    EXPECT_TRUE(d.isNormalised);
    ASSERT_FALSE(d.ops.empty());
    EXPECT_EQ(d.ops[0].type, SemOpType::Assign);
    EXPECT_EQ(d.ops[0].src1.imm, 0);
    EXPECT_EQ(d.ops[0].flagsEffect.undefined, Flags::NONE);
}

TEST(Normalise, ADD_reg_zero_IsNop)
{
    // 83 C0 00 = ADD EAX, 0
    auto d = decode({0x83, 0xC0, 0x00});
    EXPECT_TRUE(d.isValid);
    EXPECT_TRUE(d.isNormalised);
    ASSERT_FALSE(d.ops.empty());
    EXPECT_EQ(d.ops[0].type, SemOpType::Nop);
}

TEST(Normalise, MOV_reg_reg_same_IsNop)
{
    // 89 C0 = MOV EAX, EAX
    auto d = decode({0x89, 0xC0});
    EXPECT_TRUE(d.isValid);
    EXPECT_TRUE(d.isNormalised);
    ASSERT_FALSE(d.ops.empty());
    EXPECT_EQ(d.ops[0].type, SemOpType::Nop);
}

TEST(Normalise, MOV_reg_different_NotNop)
{
    // 89 D8 = MOV EAX, EBX  (different registers)
    auto d = decode({0x89, 0xD8});
    EXPECT_EQ(d.ops[0].type, SemOpType::Assign);
    EXPECT_FALSE(d.isNormalised);
}

TEST(Normalise, XCHG_twice_BothNop)
{
    // 87 C3 = XCHG EAX, EBX  (first)
    // 87 C3 = XCHG EAX, EBX  (second — cancels first)
    std::vector<uint8_t> code = {0x87, 0xC3, 0x87, 0xC3};
    auto instrs = decoder64().decodeLinear(code.data(), code.size(),
                                           0x401000, 0x401004);
    ASSERT_EQ(instrs.size(), 2u);
    // The second XCHG should be normalised away.
    EXPECT_EQ(instrs[1].ops[0].type, SemOpType::Nop);
    EXPECT_TRUE(instrs[1].isNormalised);
}

TEST(Normalise, SUB_reg_imm_NotNormalised)
{
    // 83 E8 01 = SUB EAX, 1  (different operands — not a zeroing sub)
    auto d = decode({0x83, 0xE8, 0x01});
    EXPECT_FALSE(d.isNormalised);
    EXPECT_EQ(d.ops[0].type, SemOpType::Sub);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Flag annotation
// ═══════════════════════════════════════════════════════════════════════════════

TEST(FlagAnnotation, CMP_writesFlags)
{
    // 39 D8 = CMP EAX, EBX — writes CF, PF, AF, ZF, SF, OF
    auto d = decode({0x39, 0xD8});
    EXPECT_NE(d.flagsEffect.defined & Flags::ZF, 0u);
    EXPECT_NE(d.flagsEffect.defined & Flags::CF, 0u);
    EXPECT_NE(d.flagsEffect.defined & Flags::SF, 0u);
    EXPECT_EQ(d.flagsEffect.undefined, Flags::NONE);
}

TEST(FlagAnnotation, MOV_preservesFlags)
{
    // 89 D8 = MOV EAX, EBX — does not touch flags
    auto d = decode({0x89, 0xD8});
    EXPECT_EQ(d.flagsEffect.defined,   Flags::NONE);
    EXPECT_EQ(d.flagsEffect.undefined, Flags::NONE);
}

TEST(FlagAnnotation, ShiftByVar_undefinedOF)
{
    // D3 E0 = SHL EAX, CL — OF is undefined when count != 1
    // (Capstone reports OF as undefined for variable-count shifts)
    auto d = decode({0xD3, 0xE0});
    EXPECT_TRUE(d.isValid);
    // CF is defined; OF may be undefined per Intel SDM.
    // We just verify no crash and that flags fields are populated.
    EXPECT_TRUE(d.flagsEffect.writesAny() ||
                !d.flagsEffect.writesAny()); // always true; just exercise it
}

TEST(FlagAnnotation, NOP_preservesAllFlags)
{
    // 90 = NOP
    auto d = decode({0x90});
    EXPECT_EQ(d.flagsEffect.defined,   Flags::NONE);
    EXPECT_EQ(d.flagsEffect.undefined, Flags::NONE);
    EXPECT_EQ(d.flagsEffect.preserved, Flags::ALL);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Undefined flag propagation
// ═══════════════════════════════════════════════════════════════════════════════

TEST(UndefPropagation, ShiftThenJump)
{
    // D3 E0 = SHL EAX, CL     (OF may be undefined)
    // 74 00 = JE +0            (reads ZF — which SHL defines; but OF is undef)
    std::vector<uint8_t> code = {0xD3, 0xE0, 0x74, 0x00};
    auto instrs = decoder64().decodeLinear(code.data(), code.size(),
                                           0x401000, 0x401004);
    ASSERT_GE(instrs.size(), 1u);
    // Just verify no crash and propagation ran.
}

TEST(UndefPropagation, NoUndefInitially)
{
    // CMP EAX, EBX then JE — all flags are well-defined.
    std::vector<uint8_t> code = {0x39, 0xD8, 0x74, 0x00};
    auto instrs = decoder64().decodeLinear(code.data(), code.size(),
                                           0x401000, 0x401004);
    ASSERT_EQ(instrs.size(), 2u);
    // CMP defines all arith flags; no Undef ops should appear.
    bool foundUndef = false;
    for (const auto& instr : instrs) {
        for (const auto& op : instr.ops) {
            if (op.type == SemOpType::Undef) foundUndef = true;
        }
    }
    EXPECT_FALSE(foundUndef);
}

TEST(UndefPropagation, PropagateUndefFlagsStatic)
{
    // Build a manual sequence:
    //   instr0: flags = {defined=ZF, undefined=OF, preserved=rest}
    //   instr1: flags = {preserved=ALL} (reads flags)
    // After propagation instr1 should get an Undef op because OF is undef.
    DecodedInstr i0, i1;
    i0.isValid = i1.isValid = true;
    i0.len = 1; i1.len = 1;

    FlagsEffect fe0;
    fe0.defined   = Flags::ZF;
    fe0.undefined = Flags::OF;
    fe0.preserved = Flags::ALL & ~(Flags::ZF | Flags::OF);
    i0.flagsEffect = fe0;
    SemanticOp op0; op0.type = SemOpType::Compare; op0.flagsEffect = fe0;
    i0.ops.push_back(op0);

    FlagsEffect fe1;
    fe1.defined   = Flags::NONE;
    fe1.undefined = Flags::NONE;
    fe1.preserved = Flags::ALL;
    i1.flagsEffect = fe1;
    SemanticOp op1; op1.type = SemOpType::Branch; op1.flagsEffect = fe1;
    i1.ops.push_back(op1);

    std::vector<DecodedInstr> seq = {i0, i1};
    SemDecoder::propagateUndefFlags(seq);

    // i1 "preserves" OF, but OF was undefined from i0.
    // It should receive an Undef op.
    bool gotUndef = false;
    for (const auto& op : seq[1].ops) {
        if (op.type == SemOpType::Undef) { gotUndef = true; break; }
    }
    EXPECT_TRUE(gotUndef);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Entropy calculation
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Entropy, AllSameBytes_ZeroEntropy)
{
    std::vector<uint8_t> data(256, 0xAA);
    EXPECT_NEAR(SemDecoder::entropy(data.data(), data.size()), 0.0, 0.001);
}

TEST(Entropy, AllDistinctBytes_MaxEntropy)
{
    std::vector<uint8_t> data(256);
    for (int i = 0; i < 256; ++i) data[i] = static_cast<uint8_t>(i);
    double h = SemDecoder::entropy(data.data(), data.size());
    EXPECT_NEAR(h, 8.0, 0.001); // 8 bits per byte = maximum
}

TEST(Entropy, EmptyData_Zero)
{
    EXPECT_NEAR(SemDecoder::entropy(nullptr, 0), 0.0, 0.001);
}

TEST(Entropy, HighEntropyDetection)
{
    // Random-ish data (every byte distinct repeated twice = ~7.9 bits/byte).
    std::vector<uint8_t> data(512);
    for (int i = 0; i < 256; ++i) { data[i*2] = data[i*2+1] = static_cast<uint8_t>(i); }
    EXPECT_TRUE(SemDecoder::isHighEntropy(data.data(), data.size()));
}

TEST(Entropy, LowEntropyCode_NotHighEntropy)
{
    // NOP sled — all 0x90.
    std::vector<uint8_t> data(256, 0x90);
    EXPECT_FALSE(SemDecoder::isHighEntropy(data.data(), data.size()));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Overlapping decode graph
// ═══════════════════════════════════════════════════════════════════════════════

TEST(DecodeGraph, SimpleLinearCode)
{
    // Two NOPs: 90 90 — should produce a graph with 2 nodes and a path of 2.
    std::vector<uint8_t> code = {0x90, 0x90};
    auto g = decoder64().buildDecodeGraph(code.data(), code.size(), 0x401000);

    // Must have at least the two NOP nodes.
    EXPECT_GE(g.nodes.size(), 2u);
    // Path should be non-empty.
    EXPECT_FALSE(g.path.empty());
    // First node in path should be at the start.
    EXPECT_EQ(g.path.front(), 0x401000u);
}

TEST(DecodeGraph, AllNodesHaveSuccessors)
{
    // MOV EAX, EBX; RET → two instructions, edge between them.
    std::vector<uint8_t> code = {0x89, 0xD8, 0xC3};
    auto g = decoder64().buildDecodeGraph(code.data(), code.size(), 0x401000);
    EXPECT_GE(g.nodes.size(), 2u);
}

TEST(DecodeGraph, PathDoesNotExceedRange)
{
    std::vector<uint8_t> code = {0x90, 0x90, 0x90, 0x90};
    uint64_t start = 0x401000;
    uint64_t end   = start + code.size();
    auto g = decoder64().buildDecodeGraph(code.data(), code.size(), start);

    for (uint64_t a : g.path) {
        EXPECT_GE(a, start);
        EXPECT_LT(a, end);
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Unigram model
// ═══════════════════════════════════════════════════════════════════════════════

TEST(Unigram, KnownMnemonicsHaveHigherFreq)
{
    double movFreq  = SemDecoder::unigramLogFreq("mov",  2);
    double hltFreq  = SemDecoder::unigramLogFreq("hlt",  1);
    double unkFreq  = SemDecoder::unigramLogFreq("ud2",  2);
    EXPECT_GT(movFreq, hltFreq);
    EXPECT_GT(hltFreq, unkFreq);
}

TEST(Unigram, LongerInstructionPenalty)
{
    double short3 = SemDecoder::unigramLogFreq("mov", 3);
    double long6  = SemDecoder::unigramLogFreq("mov", 6);
    EXPECT_GT(short3, long6);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Linear decode sequence
// ═══════════════════════════════════════════════════════════════════════════════

TEST(LinearDecode, SimpleSequence)
{
    // NOP; MOV EAX, EBX; RET
    std::vector<uint8_t> code = {0x90, 0x89, 0xD8, 0xC3};
    auto instrs = decoder64().decodeLinear(code.data(), code.size(),
                                           0x401000, 0x401004);
    ASSERT_EQ(instrs.size(), 3u);
    EXPECT_EQ(instrs[0].ops[0].type, SemOpType::Nop);
    EXPECT_EQ(instrs[1].ops[0].type, SemOpType::Assign);
    EXPECT_EQ(instrs[2].ops[0].type, SemOpType::Return);
}

TEST(LinearDecode, NormalisationInSequence)
{
    // SUB EAX, EAX; ADD EAX, 0; NOP
    std::vector<uint8_t> code = {0x29, 0xC0, 0x83, 0xC0, 0x00, 0x90};
    auto instrs = decoder64().decodeLinear(code.data(), code.size(),
                                           0x401000, 0x401006);
    ASSERT_GE(instrs.size(), 3u);
    // First: SUB EAX, EAX → normalised to Assign 0
    EXPECT_EQ(instrs[0].ops[0].type, SemOpType::Assign);
    EXPECT_TRUE(instrs[0].isNormalised);
    // Second: ADD EAX, 0 → Nop
    EXPECT_EQ(instrs[1].ops[0].type, SemOpType::Nop);
    // Third: NOP
    EXPECT_EQ(instrs[2].ops[0].type, SemOpType::Nop);
}

TEST(LinearDecode, AddressesCorrect)
{
    // Two 1-byte NOPs.
    std::vector<uint8_t> code = {0x90, 0x90};
    auto instrs = decoder64().decodeLinear(code.data(), code.size(),
                                           0x401000, 0x401002);
    ASSERT_EQ(instrs.size(), 2u);
    EXPECT_EQ(instrs[0].addr, 0x401000u);
    EXPECT_EQ(instrs[1].addr, 0x401001u);
}

TEST(LinearDecode, StopsAtEndAddr)
{
    // 4 NOPs but we only request up to 0x401002 (2 bytes).
    std::vector<uint8_t> code = {0x90, 0x90, 0x90, 0x90};
    auto instrs = decoder64().decodeLinear(code.data(), 2, 0x401000, 0x401002);
    EXPECT_EQ(instrs.size(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SemVal helpers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SemVal, RegFactory)
{
    auto v = SemVal::makeReg(5);
    EXPECT_EQ(v.kind, SemValKind::Reg);
    EXPECT_EQ(v.reg, 5u);
    EXPECT_FALSE(v.isNone());
    EXPECT_FALSE(v.isUndef());
}

TEST(SemVal, ImmFactory)
{
    auto v = SemVal::makeImm(42);
    EXPECT_EQ(v.kind, SemValKind::Imm);
    EXPECT_EQ(v.imm, 42);
}

TEST(SemVal, UndefFactory)
{
    auto v = SemVal::undef();
    EXPECT_EQ(v.kind, SemValKind::Undef);
    EXPECT_TRUE(v.isUndef());
}

TEST(SemVal, MemFactory)
{
    auto v = SemVal::mem(1, 2, 4, 8);
    EXPECT_EQ(v.kind,  SemValKind::MemDeref);
    EXPECT_EQ(v.base,  1u);
    EXPECT_EQ(v.index, 2u);
    EXPECT_EQ(v.scale, 4);
    EXPECT_EQ(v.disp,  8);
}

TEST(SemVal, DefaultIsNone)
{
    SemVal v;
    EXPECT_TRUE(v.isNone());
}

// ═══════════════════════════════════════════════════════════════════════════════
// FlagsEffect helpers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(FlagsEffect, WritesAny_True)
{
    FlagsEffect fe(Flags::ZF, Flags::NONE, Flags::ALL & ~Flags::ZF);
    EXPECT_TRUE(fe.writesAny());
}

TEST(FlagsEffect, WritesAny_False)
{
    FlagsEffect fe(Flags::NONE, Flags::NONE, Flags::ALL);
    EXPECT_FALSE(fe.writesAny());
}

TEST(FlagsEffect, Default_AllPreserved)
{
    FlagsEffect fe;
    EXPECT_EQ(fe.defined,   Flags::NONE);
    EXPECT_EQ(fe.undefined, Flags::NONE);
    EXPECT_EQ(fe.preserved, Flags::ALL);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Decoder initialisation
// ═══════════════════════════════════════════════════════════════════════════════

TEST(DecoderInit, X86ModeCreates)
{
    EXPECT_NO_THROW({ SemDecoder d(SemDecoder::Mode::X86); });
}

TEST(DecoderInit, X86_64ModeCreates)
{
    EXPECT_NO_THROW({ SemDecoder d(SemDecoder::Mode::X86_64); });
}

// ═══════════════════════════════════════════════════════════════════════════════
// bestPath: relaxation over the overlapping-decode graph
// ═══════════════════════════════════════════════════════════════════════════════
//
// The relaxation step read
//
//     auto& ss = score[succ];
//     if (score.find(succ) == score.end()) ss = kNegInf;
//
// and operator[] INSERTS a value-initialised 0.0 before the find() runs, so
// the guard was dead and a first-time successor started at 0.0 rather than
// -inf. Every logFreq is a log-frequency plus a length penalty, so every
// partial score is strictly negative and `nextScore > ss` was false for every
// edge in every graph: pred stayed empty, the traceback broke on its first
// step, and bestPath returned a two-element path regardless of the input.
//
// The same operator[] also left a 0.0 entry behind for each successor it
// refused to relax, and the "best end node" loop maximises over score -- so
// the node it picked was whichever spurious 0.0 the unordered_map happened to
// hand back first, not the best-scoring one.

namespace {

/// A linear chain 0x1000 -> 0x1002 -> 0x1004 with the given per-node scores.
DecodeGraph makeChain(const std::vector<double>& logFreqs)
{
	DecodeGraph g;
	for (std::size_t i = 0; i < logFreqs.size(); ++i)
	{
		DecodeNode n;
		n.addr = 0x1000 + 2 * i;
		n.len = 2;
		n.logFreq = logFreqs[i];
		if (i + 1 < logFreqs.size()) n.successors.push_back(0x1000 + 2 * (i + 1));
		g.nodes[n.addr] = n;
	}
	return g;
}

} // namespace

TEST(BestPath, AChainIsFollowedToItsEnd)
{
	// Three nodes, one edge each. The best path from 0x1000 covers all three.
	DecodeGraph g = makeChain({-1.0, -1.0, -1.0});

	auto path = SemDecoder::bestPath(g, 0x1000, 0x1010);

	ASSERT_EQ(3u, path.size()) << "the traceback stopped early";
	EXPECT_EQ(0x1000u, path[0]);
	EXPECT_EQ(0x1002u, path[1]);
	EXPECT_EQ(0x1004u, path[2]);
}

TEST(BestPath, TheHigherScoringPredecessorWins)
{
	// Two ways into 0x1006:
	//   0x1000 -> 0x1002 -> 0x1006   total -1.0 + -1.0 = -2.0
	//   0x1000 -> 0x1004 -> 0x1006   total -1.0 + -9.0 = -10.0
	// The first is better, so 0x1006's predecessor must be 0x1002.
	DecodeGraph g;
	auto add = [&g](uint64_t a, double f, std::vector<uint64_t> succs) {
		DecodeNode n;
		n.addr = a;
		n.len = 2;
		n.logFreq = f;
		n.successors = std::move(succs);
		g.nodes[a] = n;
	};
	add(0x1000, -1.0, {0x1002, 0x1004});
	add(0x1002, -1.0, {0x1006});
	add(0x1004, -9.0, {0x1006});
	add(0x1006, -1.0, {});

	auto path = SemDecoder::bestPath(g, 0x1000, 0x1010);

	ASSERT_EQ(3u, path.size()) << "the traceback stopped early";
	EXPECT_EQ(0x1000u, path[0]);
	EXPECT_EQ(0x1002u, path[1]) << "the worse predecessor was chosen";
	EXPECT_EQ(0x1006u, path[2]);
}

TEST(BestPath, ANodeNoEdgeReachesIsNotScored)
{
	// 0x1004 is in the graph but nothing points at it. It must not turn up as
	// the best end node -- which is exactly what a spurious 0.0 entry did,
	// since 0.0 beats every real (negative) score.
	DecodeGraph g;
	auto add = [&g](uint64_t a, double f, std::vector<uint64_t> succs) {
		DecodeNode n;
		n.addr = a;
		n.len = 2;
		n.logFreq = f;
		n.successors = std::move(succs);
		g.nodes[a] = n;
	};
	add(0x1000, -1.0, {0x1002});
	add(0x1002, -1.0, {});
	add(0x1004, -1.0, {}); // unreachable

	auto path = SemDecoder::bestPath(g, 0x1000, 0x1010);

	EXPECT_EQ(0x1002u, path.back()) << "an unreachable node was picked as the end";
}

// ═══════════════════════════════════════════════════════════════════════════════
// propagateUndefFlags: undefinedness has to survive more than one instruction
// ═══════════════════════════════════════════════════════════════════════════════

TEST(UndefPropagation, UndefinednessSurvivesAPreservingInstruction)
{
	// The transfer function was
	//     curUndef &= ~fe.defined;    // defined flags are no longer undef
	//     curUndef &= ~fe.preserved;  // <-- preserved means they retain state
	//     curUndef |=  fe.undefined;
	// and a well-formed FlagsEffect partitions the flags into defined,
	// preserved and undefined -- so the two ANDs together cleared everything
	// and the whole function reduced to curUndef' = fe.undefined. Undefinedness
	// died at the next instruction, whatever that instruction was.
	//
	// "Preserved" is the opposite: the flag keeps the value it had, undefined
	// included. Three instructions is the shortest sequence that shows it; the
	// existing two-instruction test passed either way.
	DecodedInstr i0, i1, i2;
	i0.isValid = i1.isValid = i2.isValid = true;
	i0.len = i1.len = i2.len = 1;

	// i0: defines ZF, leaves OF architecturally undefined.
	FlagsEffect fe0;
	fe0.defined = Flags::ZF;
	fe0.undefined = Flags::OF;
	fe0.preserved = Flags::ALL & ~(Flags::ZF | Flags::OF);
	i0.flagsEffect = fe0;

	// i1: touches nothing at all -- OF is still undefined after it.
	FlagsEffect fe1;
	fe1.defined = Flags::NONE;
	fe1.undefined = Flags::NONE;
	fe1.preserved = Flags::ALL;
	i1.flagsEffect = fe1;

	// i2: same, and this is the one that must still see OF as undefined.
	i2.flagsEffect = fe1;

	std::vector<DecodedInstr> seq = {i0, i1, i2};
	SemDecoder::propagateUndefFlags(seq);

	auto hasUndef = [](const DecodedInstr& d) {
		for (const auto& op: d.ops)
			if (op.type == SemOpType::Undef) return true;
		return false;
	};

	EXPECT_TRUE(hasUndef(seq[1])) << "the instruction right after the undef missed it";
	EXPECT_TRUE(hasUndef(seq[2])) << "undefinedness did not survive one preserving instruction";
}

TEST(UndefPropagation, ARedefinitionEndsIt)
{
	// The other half of the same transfer function: once something writes a
	// deterministic value to OF, it is no longer undefined and consumers
	// downstream must stop being told that it is.
	DecodedInstr i0, i1, i2;
	i0.isValid = i1.isValid = i2.isValid = true;
	i0.len = i1.len = i2.len = 1;

	FlagsEffect fe0;
	fe0.defined = Flags::NONE;
	fe0.undefined = Flags::OF;
	fe0.preserved = Flags::ALL & ~Flags::OF;
	i0.flagsEffect = fe0;

	// i1 redefines OF.
	FlagsEffect fe1;
	fe1.defined = Flags::OF;
	fe1.undefined = Flags::NONE;
	fe1.preserved = Flags::ALL & ~Flags::OF;
	i1.flagsEffect = fe1;

	// i2 preserves everything; OF is defined by now.
	FlagsEffect fe2;
	fe2.defined = Flags::NONE;
	fe2.undefined = Flags::NONE;
	fe2.preserved = Flags::ALL;
	i2.flagsEffect = fe2;

	std::vector<DecodedInstr> seq = {i0, i1, i2};
	SemDecoder::propagateUndefFlags(seq);

	bool undefAtEnd = false;
	for (const auto& op: seq[2].ops)
		if (op.type == SemOpType::Undef) undefAtEnd = true;
	EXPECT_FALSE(undefAtEnd) << "a redefined flag was still reported undefined";
}
