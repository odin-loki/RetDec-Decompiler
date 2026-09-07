/**
 * @file src/jvm_parser/jvm_lifter.cpp
 * @brief JVM bytecode → BcCFG lifter.
 *
 * All 201 JVM opcodes (Java 1.0–21) are handled.
 * Deprecated jsr/ret are emitted as Goto (structurally handled by CFG).
 */

#include "retdec/jvm_parser/jvm_lifter.h"
#include "retdec/jvm_parser/jvm_signature.h"

#include "retdec/utils/branch_target.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>

namespace retdec {
namespace jvm_parser {

using namespace bc_module;
using namespace bc_module::types;

// ─── JVM opcode constants ─────────────────────────────────────────────────────

static constexpr uint8_t OP_NOP = 0x00;
static constexpr uint8_t OP_ACONST_NULL = 0x01;
static constexpr uint8_t OP_ICONST_M1 = 0x02;
static constexpr uint8_t OP_ICONST_0 = 0x03;
static constexpr uint8_t OP_ICONST_5 = 0x08;
static constexpr uint8_t OP_LCONST_0 = 0x09;
static constexpr uint8_t OP_LCONST_1 = 0x0A;
static constexpr uint8_t OP_FCONST_0 = 0x0B;
static constexpr uint8_t OP_FCONST_2 = 0x0D;
static constexpr uint8_t OP_DCONST_0 = 0x0E;
static constexpr uint8_t OP_DCONST_1 = 0x0F;
static constexpr uint8_t OP_BIPUSH = 0x10;
static constexpr uint8_t OP_SIPUSH = 0x11;
static constexpr uint8_t OP_LDC = 0x12;
static constexpr uint8_t OP_LDC_W = 0x13;
static constexpr uint8_t OP_LDC2_W = 0x14;
static constexpr uint8_t OP_ILOAD = 0x15;
static constexpr uint8_t OP_LLOAD = 0x16;
static constexpr uint8_t OP_FLOAD = 0x17;
static constexpr uint8_t OP_DLOAD = 0x18;
static constexpr uint8_t OP_ALOAD = 0x19;
static constexpr uint8_t OP_ILOAD_0 = 0x1A;
static constexpr uint8_t OP_ALOAD_3 = 0x2D;
static constexpr uint8_t OP_IALOAD = 0x2E;
static constexpr uint8_t OP_LALOAD = 0x2F;
static constexpr uint8_t OP_FALOAD = 0x30;
static constexpr uint8_t OP_DALOAD = 0x31;
static constexpr uint8_t OP_AALOAD = 0x32;
static constexpr uint8_t OP_BALOAD = 0x33;
static constexpr uint8_t OP_CALOAD = 0x34;
static constexpr uint8_t OP_SALOAD = 0x35;
static constexpr uint8_t OP_ISTORE = 0x36;
static constexpr uint8_t OP_LSTORE = 0x37;
static constexpr uint8_t OP_FSTORE = 0x38;
static constexpr uint8_t OP_DSTORE = 0x39;
static constexpr uint8_t OP_ASTORE = 0x3A;
static constexpr uint8_t OP_ISTORE_0 = 0x3B;
static constexpr uint8_t OP_ASTORE_3 = 0x4E;
static constexpr uint8_t OP_IASTORE = 0x4F;
static constexpr uint8_t OP_LASTORE = 0x50;
static constexpr uint8_t OP_FASTORE = 0x51;
static constexpr uint8_t OP_DASTORE = 0x52;
static constexpr uint8_t OP_AASTORE = 0x53;
static constexpr uint8_t OP_BASTORE = 0x54;
static constexpr uint8_t OP_CASTORE = 0x55;
static constexpr uint8_t OP_SASTORE = 0x56;
static constexpr uint8_t OP_POP = 0x57;
static constexpr uint8_t OP_POP2 = 0x58;
static constexpr uint8_t OP_DUP = 0x59;
static constexpr uint8_t OP_DUP_X1 = 0x5A;
static constexpr uint8_t OP_DUP_X2 = 0x5B;
static constexpr uint8_t OP_DUP2 = 0x5C;
static constexpr uint8_t OP_DUP2_X1 = 0x5D;
static constexpr uint8_t OP_DUP2_X2 = 0x5E;
static constexpr uint8_t OP_SWAP = 0x5F;
static constexpr uint8_t OP_IADD = 0x60;
static constexpr uint8_t OP_LADD = 0x61;
static constexpr uint8_t OP_FADD = 0x62;
static constexpr uint8_t OP_DADD = 0x63;
static constexpr uint8_t OP_ISUB = 0x64;
static constexpr uint8_t OP_LSUB = 0x65;
static constexpr uint8_t OP_FSUB = 0x66;
static constexpr uint8_t OP_DSUB = 0x67;
static constexpr uint8_t OP_IMUL = 0x68;
static constexpr uint8_t OP_LMUL = 0x69;
static constexpr uint8_t OP_FMUL = 0x6A;
static constexpr uint8_t OP_DMUL = 0x6B;
static constexpr uint8_t OP_IDIV = 0x6C;
static constexpr uint8_t OP_LDIV = 0x6D;
static constexpr uint8_t OP_FDIV = 0x6E;
static constexpr uint8_t OP_DDIV = 0x6F;
static constexpr uint8_t OP_IREM = 0x70;
static constexpr uint8_t OP_LREM = 0x71;
static constexpr uint8_t OP_FREM = 0x72;
static constexpr uint8_t OP_DREM = 0x73;
static constexpr uint8_t OP_INEG = 0x74;
static constexpr uint8_t OP_LNEG = 0x75;
static constexpr uint8_t OP_FNEG = 0x76;
static constexpr uint8_t OP_DNEG = 0x77;
static constexpr uint8_t OP_ISHL = 0x78;
static constexpr uint8_t OP_LSHL = 0x79;
static constexpr uint8_t OP_ISHR = 0x7A;
static constexpr uint8_t OP_LSHR = 0x7B;
static constexpr uint8_t OP_IUSHR = 0x7C;
static constexpr uint8_t OP_LUSHR = 0x7D;
static constexpr uint8_t OP_IAND = 0x7E;
static constexpr uint8_t OP_LAND = 0x7F;
static constexpr uint8_t OP_IOR = 0x80;
static constexpr uint8_t OP_LOR = 0x81;
static constexpr uint8_t OP_IXOR = 0x82;
static constexpr uint8_t OP_LXOR = 0x83;
static constexpr uint8_t OP_IINC = 0x84;
static constexpr uint8_t OP_I2L = 0x85;
static constexpr uint8_t OP_I2F = 0x86;
static constexpr uint8_t OP_I2D = 0x87;
static constexpr uint8_t OP_L2I = 0x88;
static constexpr uint8_t OP_L2F = 0x89;
static constexpr uint8_t OP_L2D = 0x8A;
static constexpr uint8_t OP_F2I = 0x8B;
static constexpr uint8_t OP_F2L = 0x8C;
static constexpr uint8_t OP_F2D = 0x8D;
static constexpr uint8_t OP_D2I = 0x8E;
static constexpr uint8_t OP_D2L = 0x8F;
static constexpr uint8_t OP_D2F = 0x90;
static constexpr uint8_t OP_I2B = 0x91;
static constexpr uint8_t OP_I2C = 0x92;
static constexpr uint8_t OP_I2S = 0x93;
static constexpr uint8_t OP_LCMP = 0x94;
static constexpr uint8_t OP_FCMPL = 0x95;
static constexpr uint8_t OP_FCMPG = 0x96;
static constexpr uint8_t OP_DCMPL = 0x97;
static constexpr uint8_t OP_DCMPG = 0x98;
static constexpr uint8_t OP_IFEQ = 0x99;
static constexpr uint8_t OP_IFNE = 0x9A;
static constexpr uint8_t OP_IFLT = 0x9B;
static constexpr uint8_t OP_IFGE = 0x9C;
static constexpr uint8_t OP_IFGT = 0x9D;
static constexpr uint8_t OP_IFLE = 0x9E;
static constexpr uint8_t OP_IF_ICMPEQ = 0x9F;
static constexpr uint8_t OP_IF_ICMPNE = 0xA0;
static constexpr uint8_t OP_IF_ICMPLT = 0xA1;
static constexpr uint8_t OP_IF_ICMPGE = 0xA2;
static constexpr uint8_t OP_IF_ICMPGT = 0xA3;
static constexpr uint8_t OP_IF_ICMPLE = 0xA4;
static constexpr uint8_t OP_IF_ACMPEQ = 0xA5;
static constexpr uint8_t OP_IF_ACMPNE = 0xA6;
static constexpr uint8_t OP_GOTO = 0xA7;
static constexpr uint8_t OP_JSR = 0xA8;
static constexpr uint8_t OP_RET = 0xA9;
static constexpr uint8_t OP_TABLESWITCH = 0xAA;
static constexpr uint8_t OP_LOOKUPSWITCH = 0xAB;
static constexpr uint8_t OP_IRETURN = 0xAC;
static constexpr uint8_t OP_LRETURN = 0xAD;
static constexpr uint8_t OP_FRETURN = 0xAE;
static constexpr uint8_t OP_DRETURN = 0xAF;
static constexpr uint8_t OP_ARETURN = 0xB0;
static constexpr uint8_t OP_RETURN = 0xB1;
static constexpr uint8_t OP_GETSTATIC = 0xB2;
static constexpr uint8_t OP_PUTSTATIC = 0xB3;
static constexpr uint8_t OP_GETFIELD = 0xB4;
static constexpr uint8_t OP_PUTFIELD = 0xB5;
static constexpr uint8_t OP_INVOKEVIRTUAL = 0xB6;
static constexpr uint8_t OP_INVOKESPECIAL = 0xB7;
static constexpr uint8_t OP_INVOKESTATIC = 0xB8;
static constexpr uint8_t OP_INVOKEINTERFACE = 0xB9;
static constexpr uint8_t OP_INVOKEDYNAMIC = 0xBA;
static constexpr uint8_t OP_NEW = 0xBB;
static constexpr uint8_t OP_NEWARRAY = 0xBC;
static constexpr uint8_t OP_ANEWARRAY = 0xBD;
static constexpr uint8_t OP_ARRAYLENGTH = 0xBE;
static constexpr uint8_t OP_ATHROW = 0xBF;
static constexpr uint8_t OP_CHECKCAST = 0xC0;
static constexpr uint8_t OP_INSTANCEOF = 0xC1;
static constexpr uint8_t OP_MONITORENTER = 0xC2;
static constexpr uint8_t OP_MONITOREXIT = 0xC3;
static constexpr uint8_t OP_WIDE = 0xC4;
static constexpr uint8_t OP_MULTIANEWARRAY = 0xC5;
static constexpr uint8_t OP_IFNULL = 0xC6;
static constexpr uint8_t OP_IFNONNULL = 0xC7;
static constexpr uint8_t OP_GOTO_W = 0xC8;
static constexpr uint8_t OP_JSR_W = 0xC9;

// A pc that is not a branch target.
//
// A JVM Code attribute's code_length is a u4 (JVMS 4.7.3) and every leader is
// an offset strictly inside code[], so the branch resolution below never
// produces UINT32_MAX: kCodeSizeCap keeps the code size it checks against at or
// below UINT32_MAX, and btgt::relative only succeeds for a target strictly
// below that. buildBlocks() looks every target up with pcToBlock_.count(), so a
// branch that did not resolve simply gets no edge instead of an edge to a block
// that is not there.
static constexpr uint32_t kNoBranchTarget = UINT32_MAX;

// The largest code size the leader set can address. Leaders are uint32_t
// because code_length is a u4; clamping here is what makes the narrowing cast
// of a resolved target exact for any code[] a caller hands us.
static constexpr uint64_t kCodeSizeCap = UINT32_MAX;

// ─── Instruction size table ───────────────────────────────────────────────────
// Returns the total byte size of the instruction including opcode, per JVMS 6.5.
// Returns 0 for variable-length instructions (tableswitch/lookupswitch/wide) and
// for the opcodes JVMS 6.2 leaves unassigned.
//
// 23 of these entries used to disagree with JVMS 6.5, and nothing failed loudly:
// findLeaders() below writes `if (sz <= 0) sz = 1;`, so a wrong size simply puts
// the scan cursor on an operand byte, which is then decoded as an opcode, and
// every leader found from there on is wrong. ESBMC refuted
// `tblAsWritten[op] == tblJvms[op]` over a symbolic op < 256; the first
// counterexample was op = 0x30 (faload: the table said 2, JVMS says 1).
// Enumerated, the 23 were two shifted rows and one shifted-by-four row:
//
//   0x2E-0x34 iaload..caload      said 2, are 1 (no operand at all)
//   0x36-0x3A istore..astore      said 1, are 2 (they carry a local index)
//   0x99      ifeq                said 1, is  3
//   0xB2-0xB5 getstatic..putfield said 1, are 3 (a u2 constant-pool index)
//   0xB9      invokeinterface     said 3, is  5 (index, count, and a zero byte)
//   0xBC      newarray            said 3, is  2 (a one-byte atype)
//   0xBD      anewarray           said 1, is  3
//   0xBE-0xBF arraylength, athrow said 3, are 1
//   0xC9      jsr_w               said 3, is  5
//
// getfield and getstatic are in every non-trivial method, so the desynchronised
// walk was the normal case rather than a corner one: on `getstatic #2; goto L`
// the old table advanced 1 byte from the getstatic and read its index high byte
// 0x00 as `nop`, then its low byte 0x02 as `iconst_m1`, and the goto that
// followed was never seen as a branch, so L never became a leader and the block
// containing it was never split.
static int instrSize(uint8_t op)
{
	static const int8_t tbl[256] = {
		//  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
		1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, // 00-0F
		2, 3, 2, 3, 3, 2, 2, 2,
		2, 2, 1, 1, 1, 1, 1, 1, // 10-1F (0x15-0x19 loads take an index)
		1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, // 20-2F (0x2E-0x2F iaload/laload: no operand)
		1, 1, 1, 1, 1, 1, 2, 2,
		2, 2, 2, 1, 1, 1, 1, 1, // 30-3F (0x36-0x3A istore..astore take an index)
		1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, // 40-4F
		1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, // 50-5F
		1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, // 60-6F
		1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, // 70-7F
		1, 1, 1, 1, 3, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, // 80-8F (0x84 = iinc = 3)
		1, 1, 1, 1, 1, 1, 1, 1,
		1, 3, 3, 3, 3, 3, 3, 3, // 90-9F (0x99-0x9F = ifeq..if_icmpeq = 3)
		3, 3, 3, 3, 3, 3, 3, 3,
		3, 2, 0, 0, 1, 1, 1, 1, // A0-AF (0xA7=goto=3,0xA8=jsr=3,0xA9=ret=2,0xAA-0xAB=var)
		1, 1, 3, 3, 3, 3, 3, 3,
		3, 5, 5, 3, 2, 3, 1, 1, // B0-BF (0xB9=invokeinterface=5,0xBA=invokedynamic=5,0xBC=newarray=2)
		3, 3, 1, 1, 0, 4, 3, 3,
		5, 5, 0, 0, 0, 0, 0, 0, // C0-CF (0xC4=wide=var,0xC5=multianewarray=4,0xC6-7=3,0xC8-9=5)
		0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, // D0-DF
		0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0, // E0-EF
		0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0 // F0-FF
	};
	return static_cast<int>(tbl[op]);
}

// ─── JvmLifter constructor ────────────────────────────────────────────────────

JvmLifter::JvmLifter(const ConstPool& pool, LiftOptions opts): pool_(pool), opts_(std::move(opts)) {}

// ─── Pass 1: Find leaders ─────────────────────────────────────────────────────

std::vector<uint32_t> JvmLifter::findLeaders(const std::vector<uint8_t>& bc, const std::vector<ExceptionEntry>& eh)
{
	std::set<uint32_t> leaders;
	leaders.insert(0);

	// The code size every leader in this function is judged against. Clamping
	// to kCodeSizeCap is what makes the narrowing cast of a resolved target
	// exact; see the note on kCodeSizeCap above.
	const uint64_t codeSize = std::min<uint64_t>(bc.size(), kCodeSizeCap);

	// An exception_table entry's start_pc and handler_pc are u2 fields copied
	// verbatim out of the class file, and nothing in jvm_attr's parser checks
	// either against code_length. These two were the last leader sites still
	// inserting a file value without a range check, so a handler_pc past the
	// end of code[] became a leader and buildBlocks() then created a block at a
	// pc no instruction falls into -- exactly the failure branch_target.h names,
	// "it is not that the arithmetic is unchecked, it is that the unchecked
	// answer reaches the CFG". Demonstrated by
	// JvmLifter.ExceptionHandlerPcPastTheEndOfCodeIsNotALeader: code {0x00,
	// 0xB1} with handlerPc = 900 used to come back as two blocks, the second
	// one empty.
	//
	// btgt::absolute is the trivial half of the same rule the eight relative
	// sites below use, with the same write-only-on-success discipline: a pc
	// outside [0, codeSize) is not a leader, because there is no instruction
	// there for it to lead.
	for (const auto& e: eh)
	{
		uint64_t target = 0;
		if (utils::btgt::absolute(e.startPc, codeSize, target)) leaders.insert(static_cast<uint32_t>(target));
		if (utils::btgt::absolute(e.handlerPc, codeSize, target)) leaders.insert(static_cast<uint32_t>(target));
	}

	for (size_t pc = 0; pc < bc.size();)
	{
		uint8_t op = bc[pc];
		int sz = instrSize(op);

		// code[] is copied verbatim out of the class file, so nothing guarantees
		// that an instruction's operands are inside it: a method body can end
		// mid-instruction. Every read below is bounded against bc.size(), and
		// the scan stops at the first operand that does not fit, because from
		// there on the stream is not decodable.
		auto fits = [&](size_t off, size_t n) -> bool { return off <= bc.size() && n <= bc.size() - off; };
		auto readS2 = [&](size_t off) -> int16_t {
			return static_cast<int16_t>((static_cast<uint16_t>(bc[off]) << 8) | bc[off + 1]);
		};
		auto readS4 = [&](size_t off) -> int32_t {
			return static_cast<int32_t>(
				(static_cast<uint32_t>(bc[off]) << 24) | (static_cast<uint32_t>(bc[off + 1]) << 16)
				| (static_cast<uint32_t>(bc[off + 2]) << 8) | static_cast<uint32_t>(bc[off + 3]));
		};
		// A branch displacement is signed file data measured from the start of
		// the instruction, and every one of the eight sites below used to write
		// `leaders.insert(static_cast<uint32_t>(pc + off))`. `pc` is a size_t,
		// so the int32_t converts to uint64 first: probe_jvm275.cpp asserted
		// `leader < codeLen` under `codeLen in (0, 0xFFFF], pc < codeLen` and
		// ESBMC refuted it twice. Forward, codeLen = 33012, pc = 193,
		// def = 94928897 gives leader 94929090, which lands 94896078 bytes past
		// the end of the method. Backward, codeLen = 32773, pc = 32769,
		// def = -1073774593 has true target 32769 - 1073774593 = -1073741824,
		// which wraps and narrows to 0xC0000000 = 3 GiB: a branch to before the
		// code became a leader 3 GiB in, and buildBlocks then created a block
		// there that no instruction falls into.
		//
		// btgt::relative forms the sum only where it exists, splits on the sign
		// of the displacement so it is exact for every base and every
		// displacement including INT64_MIN, and writes its output only when the
		// target lands inside [0, codeSize). A target that does not is not a
		// leader at all -- there is no instruction there to lead a block.
		auto addRelativeLeader = [&](size_t from, int32_t off) {
			uint64_t target = 0;
			if (utils::btgt::relative(from, off, codeSize, target)) leaders.insert(static_cast<uint32_t>(target));
		};
		// A switch declares its own entry count (hi-lo+1, or npairs) and both
		// numbers come out of the file. An entry costs entrySize bytes, so a
		// table claiming more entries than code[] can still supply is malformed
		// by construction: clamp to what is actually there instead of reading
		// past the end. A well-formed table is always fully present, so this
		// never changes what a real class file decodes to.
		auto tableEntries = [&](size_t tableStart, int64_t declared, size_t entrySize) -> size_t {
			if (declared <= 0 || tableStart > bc.size()) return 0;
			int64_t avail = static_cast<int64_t>((bc.size() - tableStart) / entrySize);
			return static_cast<size_t>(std::min(declared, avail));
		};

		if (op == OP_TABLESWITCH)
		{
			size_t pad = (4 - ((pc + 1) & 3)) & 3;
			size_t base = pc + 1 + pad;
			if (!fits(base, 12)) break;
			int32_t def = readS4(base);
			int32_t lo = readS4(base + 4);
			int32_t hi = readS4(base + 8);
			addRelativeLeader(pc, def);
			// Widened: hi - lo + 1 overflows int32_t for a hostile lo/hi pair.
			int64_t declared = static_cast<int64_t>(hi) - static_cast<int64_t>(lo) + 1;
			size_t n = tableEntries(base + 12, declared, 4);
			for (size_t i = 0; i < n; ++i)
			{
				int32_t off = readS4(base + 12 + i * 4);
				addRelativeLeader(pc, off);
			}
			size_t total = 1 + pad + 12 + n * 4;
			pc += total;
			if (pc < bc.size()) leaders.insert(static_cast<uint32_t>(pc));
			continue;
		}
		else if (op == OP_LOOKUPSWITCH)
		{
			size_t pad = (4 - ((pc + 1) & 3)) & 3;
			size_t base = pc + 1 + pad;
			if (!fits(base, 8)) break;
			int32_t def = readS4(base);
			int32_t npairs = readS4(base + 4);
			addRelativeLeader(pc, def);
			size_t n = tableEntries(base + 8, npairs, 8);
			for (size_t i = 0; i < n; ++i)
			{
				int32_t off = readS4(base + 8 + i * 8 + 4);
				addRelativeLeader(pc, off);
			}
			size_t total = 1 + pad + 8 + n * 8;
			pc += total;
			if (pc < bc.size()) leaders.insert(static_cast<uint32_t>(pc));
			continue;
		}
		else if (op == OP_WIDE)
		{
			if (!fits(pc + 1, 1)) break;
			uint8_t wop = bc[pc + 1];
			if (wop == OP_IINC)
			{
				sz = 6;
			}
			else
			{
				sz = 4;
			}
		}
		else if (op == OP_GOTO || op == OP_JSR)
		{
			if (pc + 2 < bc.size())
			{
				int16_t off = readS2(pc + 1);
				addRelativeLeader(pc, off);
			}
			if (pc + 3 < bc.size()) leaders.insert(static_cast<uint32_t>(pc + 3));
			sz = 3;
		}
		else if (op == OP_GOTO_W || op == OP_JSR_W)
		{
			if (pc + 4 < bc.size())
			{
				int32_t off = readS4(pc + 1);
				addRelativeLeader(pc, off);
			}
			if (pc + 5 < bc.size()) leaders.insert(static_cast<uint32_t>(pc + 5));
			sz = 5;
		}
		else if (op >= OP_IFEQ && op <= OP_IF_ACMPNE)
		{
			if (pc + 2 < bc.size())
			{
				int16_t off = readS2(pc + 1);
				addRelativeLeader(pc, off);
			}
			if (pc + 3 < bc.size()) leaders.insert(static_cast<uint32_t>(pc + 3));
			sz = 3;
		}
		else if (op == OP_IFNULL || op == OP_IFNONNULL)
		{
			if (pc + 2 < bc.size())
			{
				int16_t off = readS2(pc + 1);
				addRelativeLeader(pc, off);
			}
			if (pc + 3 < bc.size()) leaders.insert(static_cast<uint32_t>(pc + 3));
			sz = 3;
		}
		else if (op >= OP_IRETURN && op <= OP_RETURN)
		{
			if (pc + 1 < bc.size()) leaders.insert(static_cast<uint32_t>(pc + 1));
			sz = 1;
		}
		else if (op == OP_ATHROW)
		{
			if (pc + 1 < bc.size()) leaders.insert(static_cast<uint32_t>(pc + 1));
			sz = 1;
		}

		if (sz <= 0) sz = 1;
		pc += static_cast<size_t>(sz);
	}

	return std::vector<uint32_t>(leaders.begin(), leaders.end());
}

// ─── Pass 2: Build blocks ─────────────────────────────────────────────────────

void JvmLifter::buildBlocks(
	BcCFG& cfg, const std::vector<uint8_t>& bc, const std::vector<uint32_t>& leaders, const ConstPool& pool)
{
	// Create one block per leader.
	pcToBlock_.clear();
	for (uint32_t ldr: leaders)
	{
		auto& blk = cfg.addBlock();
		pcToBlock_[ldr] = blk.id;
	}

	// Fill each block with instructions.
	for (size_t li = 0; li < leaders.size(); ++li)
	{
		uint32_t start = leaders[li];
		uint32_t end = (li + 1 < leaders.size()) ? leaders[li + 1] : static_cast<uint32_t>(bc.size());
		auto& blk = cfg.block(pcToBlock_[start]);
		uint32_t pc = start;
		uint32_t instrId = 0;
		while (pc < end && pc < bc.size())
		{
			uint32_t thisPC = pc;
			BcInstruction instr = decodeInstr(bc, pc, end, instrId++, leaders);
			instr.offset = thisPC;
			if (opts_.mapLineNumbers)
			{
				auto it = lineMap_.find(thisPC);
				if (it != lineMap_.end()) instr.line = it->second;
			}
			blk.instrs.push_back(std::move(instr));
		}

		// Add fall-through / branch edges.
		if (!blk.instrs.empty())
		{
			const auto& last = blk.instrs.back();
			switch (last.opcode)
			{
			case BcOpcode::Return:
			case BcOpcode::ReturnValue:
			case BcOpcode::Throw: break; // No fall-through.
			case BcOpcode::Goto: {
				uint32_t target = last.blockOp(0);
				if (pcToBlock_.count(target)) cfg.addEdge(blk.id, pcToBlock_[target]);
				break;
			}
			case BcOpcode::TableSwitch:
			case BcOpcode::LookupSwitch: {
				const auto& sw = std::get<BcSwitchTable>(last.operands[0]);
				if (pcToBlock_.count(sw.defaultBlock)) cfg.addEdge(blk.id, pcToBlock_[sw.defaultBlock]);
				for (const auto& [k, tgt]: sw.cases)
					if (pcToBlock_.count(tgt)) cfg.addEdge(blk.id, pcToBlock_[tgt]);
				break;
			}
			default: {
				// Conditional branches: true target in operands, fall-through = next leader.
				if (last.operands.size() == 1)
				{
					if (const auto* bo = std::get_if<BcBlockOperand>(&last.operands[0]))
					{
						uint32_t trueTgt = bo->blockId;
						if (pcToBlock_.count(trueTgt)) cfg.addEdge(blk.id, pcToBlock_[trueTgt]);
					}
				}
				// Fall-through to next block.
				if (li + 1 < leaders.size())
				{
					uint32_t nextLdr = leaders[li + 1];
					if (pcToBlock_.count(nextLdr)) cfg.addEdge(blk.id, pcToBlock_[nextLdr]);
				}
				break;
			}
			}
		}
	}
}

// ─── Pass 3: Wire exception edges ────────────────────────────────────────────

// An exception_table entry names a half-open protected region [start_pc, end_pc)
// inside code[] (JVMS 4.7.3), but start_pc and end_pc are u2 fields copied
// verbatim out of the class file and jvm_attr's parser checks neither against
// code_length. wireExceptions() below assigns them straight into
// BcExceptionHandler::startOffset/endOffset, so without this filter a class file
// with end_pc < start_pc reaches every consumer that asks how long the region is
// the obvious way: `endOffset - startOffset` in uint32, which for
// start_pc = 4, end_pc = 0 is 4294967292 rather than a diagnosable error.
//
// btgt::region settles both halves at once -- it succeeds only when the region
// exists and fits, and then start <= end <= codeSize. The length has to exist
// before region can be asked about it, hence the ordering test first: region
// takes a length, not an end, so it cannot make that test on our behalf.
static bool protectedRegionFits(const ExceptionEntry& e, uint64_t codeSize)
{
	if (e.startPc > e.endPc) return false;
	uint64_t end = 0;
	return utils::btgt::region(e.startPc, static_cast<uint64_t>(e.endPc) - e.startPc, codeSize, end);
}

// Every entry reaching here has already been through protectedRegionFits() in
// lift(), which is where the check lives because code[] is not a parameter of
// this pass. An entry that failed it is not wired at all, so startPc/endPc are
// copied below only once they are known to describe a region inside code[].
void JvmLifter::wireExceptions(
	BcCFG& cfg, const std::vector<ExceptionEntry>& eh, const std::vector<uint32_t>& /*leaders*/, const ConstPool& pool)
{
	for (const auto& e: eh)
	{
		uint32_t handlerBlock = UINT32_MAX;
		auto it = pcToBlock_.find(e.handlerPc);
		if (it != pcToBlock_.end()) handlerBlock = it->second;
		if (handlerBlock == UINT32_MAX) continue;

		BcExceptionHandler beh;
		beh.startOffset = e.startPc;
		beh.endOffset = e.endPc;
		beh.handlerBlock = handlerBlock;
		beh.isFinally = (e.catchType == 0);
		if (e.catchType != 0)
		{
			beh.catchType = bc_module::types::Class(pool_.className(e.catchType));
		}
		cfg.addExceptionHandler(beh);

		// Mark handler block as EH entry.
		cfg.block(handlerBlock).isExceptionHandler = true;
	}
}

// ─── Per-instruction decoder ──────────────────────────────────────────────────

BcInstruction JvmLifter::decodeInstr(
	const std::vector<uint8_t>& bc,
	uint32_t& pc,
	uint32_t nextLeader,
	uint32_t instrId,
	const std::vector<uint32_t>& leaders)
{
	BcInstruction i;
	i.id = instrId;
	uint8_t op = bc[pc++];

	// buildBlocks() only guarantees that the opcode byte is inside code[]; its
	// operands live further in, and code[] is attacker-controlled, so a method
	// body can stop mid-instruction. An operand read that does not fit yields 0
	// and parks pc at the end of the stream, which ends the enclosing decode
	// loop rather than walking off the buffer.
	auto avail = [&]() -> size_t { return pc < bc.size() ? bc.size() - pc : 0; };
	auto readU1 = [&]() -> uint8_t {
		if (avail() < 1)
		{
			pc = static_cast<uint32_t>(bc.size());
			return 0;
		}
		return bc[pc++];
	};
	auto readS1 = [&]() -> int8_t { return static_cast<int8_t>(readU1()); };
	auto readU2 = [&]() -> uint16_t {
		if (avail() < 2)
		{
			pc = static_cast<uint32_t>(bc.size());
			return 0;
		}
		uint16_t v = (static_cast<uint16_t>(bc[pc]) << 8) | bc[pc + 1];
		pc += 2;
		return v;
	};
	auto readS2 = [&]() -> int16_t { return static_cast<int16_t>(readU2()); };
	auto readS4 = [&]() -> int32_t {
		if (avail() < 4)
		{
			pc = static_cast<uint32_t>(bc.size());
			return 0;
		}
		int32_t v = static_cast<int32_t>(
			(static_cast<uint32_t>(bc[pc]) << 24) | (static_cast<uint32_t>(bc[pc + 1]) << 16)
			| (static_cast<uint32_t>(bc[pc + 2]) << 8) | static_cast<uint32_t>(bc[pc + 3]));
		pc += 4;
		return v;
	};

	// Helper: resolve a branch offset relative to the start of this instruction.
	//
	// This was `static_cast<uint32_t>(static_cast<int32_t>(instrPc) + offset)`,
	// an addition of two int32_t values both derived from the file, and that is
	// undefined behaviour rather than merely a wrong answer: a Code attribute's
	// code_length is a u4 (JVMS 4.7.3) so instrPc genuinely reaches values that
	// cast negative, and goto_w/jsr_w carry a full int32 displacement.
	// probe_jvm495.cpp encoded the old body verbatim and --overflow-check alone
	// refuted it with no assertion needed: ESBMC reported "arithmetic overflow
	// on add" for instrPc = 1073741825, offset = 1073741825, whose true sum
	// 2147483650 exceeds INT32_MAX by 3. For that class file the program had no
	// defined behaviour at all.
	//
	// btgt::relative does the same computation with no signed overflow anywhere
	// and answers whether the target is inside code[] at the same time. The
	// second half is the one an ordinary-sized class file reaches, because the
	// old body had no range check at all: a displacement leaving the method was
	// written into the instruction's BcBlockOperand as if it were a pc inside
	// it. `goto +100` at pc 0 of a four-byte method recorded 100, and `goto -8`
	// at pc 1 recorded 0xFFFFFFF9. Now both resolve to kNoBranchTarget, which
	// buildBlocks()'s pcToBlock_.count() lookups miss, so the branch
	// contributes no CFG edge and nothing downstream can read the stored value
	// as an offset into the method.
	//
	// The int32 overflow needs no 2 GiB method either: a goto_w at pc 1 with an
	// offset of INT32_MAX is seven bytes of code and its sum is 2147483648.
	// What does need a code array over 2 GiB is the truncated sum coming back
	// down onto the pc of a real leader, since that requires instrPc + offset
	// to reach 2^32 with offset below 2^31. So
	// JvmLifter.OutOfRangeBranchTargetIsNotRecordedAsAPc pins the range check
	// and the overflowing goto_w, not the collision.
	// JvmLifter.InRangeBranchesStillResolve guards the other direction.
	auto branchPc = [&](int32_t offset, uint32_t instrPc) -> uint32_t {
		uint64_t target = 0;
		if (!utils::btgt::relative(instrPc, offset, std::min<uint64_t>(bc.size(), kCodeSizeCap), target))
			return kNoBranchTarget;
		return static_cast<uint32_t>(target);
	};

	uint32_t instrPc = pc - 1; // PC of this opcode.
	(void)nextLeader;
	(void)leaders;

	switch (op)
	{
	case OP_NOP:
		i.opcode = BcOpcode::Nop;
		i.effect = {0, 0};
		break;
	case OP_ACONST_NULL:
		i.opcode = BcOpcode::PushNull;
		i.effect = {0, 1};
		break;
	case OP_ICONST_M1:
		i.opcode = BcOpcode::PushInt;
		i.operands.push_back(BcIntOperand{-1});
		i.effect = {0, 1};
		break;
	case OP_ICONST_0: // 0x03-0x08
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x07:
	case OP_ICONST_5:
		i.opcode = BcOpcode::PushInt;
		i.operands.push_back(BcIntOperand{op - 3});
		i.effect = {0, 1};
		break;
	case OP_LCONST_0:
		i.opcode = BcOpcode::PushLong;
		i.operands.push_back(BcIntOperand{0});
		i.effect = {0, 1};
		break;
	case OP_LCONST_1:
		i.opcode = BcOpcode::PushLong;
		i.operands.push_back(BcIntOperand{1});
		i.effect = {0, 1};
		break;
	case OP_FCONST_0:
		i.opcode = BcOpcode::PushFloat;
		i.operands.push_back(BcFloatOperand{0.f});
		i.effect = {0, 1};
		break;
	case 0x0C:
		i.opcode = BcOpcode::PushFloat;
		i.operands.push_back(BcFloatOperand{1.f});
		i.effect = {0, 1};
		break;
	case OP_FCONST_2:
		i.opcode = BcOpcode::PushFloat;
		i.operands.push_back(BcFloatOperand{2.f});
		i.effect = {0, 1};
		break;
	case OP_DCONST_0:
		i.opcode = BcOpcode::PushDouble;
		i.operands.push_back(BcFloatOperand{0.0});
		i.effect = {0, 1};
		break;
	case OP_DCONST_1:
		i.opcode = BcOpcode::PushDouble;
		i.operands.push_back(BcFloatOperand{1.0});
		i.effect = {0, 1};
		break;
	case OP_BIPUSH:
		i.opcode = BcOpcode::PushInt;
		i.operands.push_back(BcIntOperand{readS1()});
		i.effect = {0, 1};
		break;
	case OP_SIPUSH:
		i.opcode = BcOpcode::PushInt;
		i.operands.push_back(BcIntOperand{readS2()});
		i.effect = {0, 1};
		break;
	case OP_LDC:
	case OP_LDC_W: {
		uint16_t idx = (op == OP_LDC) ? readU1() : readU2();
		if (opts_.resolveLdc && idx < pool_.size())
		{
			try
			{
				CpTag t = pool_.tag(idx);
				if (t == CpTag::String)
				{
					i.opcode = BcOpcode::PushString;
					i.operands.push_back(BcStringOperand{pool_.string(idx)});
					i.effect = {0, 1};
				}
				else if (t == CpTag::Integer)
				{
					i.opcode = BcOpcode::PushInt;
					i.operands.push_back(BcIntOperand{std::get<CpInt>(pool_.entry(idx)).value});
					i.effect = {0, 1};
				}
				else if (t == CpTag::Float)
				{
					i.opcode = BcOpcode::PushFloat;
					i.operands.push_back(BcFloatOperand{std::get<CpFloat>(pool_.entry(idx)).value});
					i.effect = {0, 1};
				}
				else if (t == CpTag::Class)
				{
					i.opcode = BcOpcode::LoadClass;
					i.operands.push_back(BcTypeOperand{bc_module::types::Class(pool_.className(idx))});
					i.effect = {0, 1};
				}
				else
				{
					i.opcode = BcOpcode::PushInt;
					i.operands.push_back(BcIntOperand{(int64_t)idx});
					i.effect = {0, 1};
				}
			}
			catch (const std::exception&)
			{
				i.opcode = BcOpcode::PushInt;
				i.operands.push_back(BcIntOperand{idx});
				i.effect = {0, 1};
			}
		}
		else
		{
			i.opcode = BcOpcode::PushInt;
			i.operands.push_back(BcIntOperand{idx});
			i.effect = {0, 1};
		}
		break;
	}
	case OP_LDC2_W: {
		uint16_t idx = readU2();
		try
		{
			CpTag t = pool_.tag(idx);
			if (t == CpTag::Long)
			{
				i.opcode = BcOpcode::PushLong;
				i.operands.push_back(BcIntOperand{std::get<CpLong>(pool_.entry(idx)).value});
			}
			else
			{
				i.opcode = BcOpcode::PushDouble;
				i.operands.push_back(BcFloatOperand{std::get<CpDouble>(pool_.entry(idx)).value});
			}
		}
		catch (const std::exception&)
		{
			i.opcode = BcOpcode::PushLong;
			i.operands.push_back(BcIntOperand{idx});
		}
		i.effect = {0, 1};
		break;
	}
	// Loads (iload-0 through aload-3 families)
	case OP_ILOAD_0:
	case 0x1B:
	case 0x1C:
	case 0x1D:
		i.opcode = BcOpcode::LoadLocal;
		i.operands.push_back(BcLocalOperand{static_cast<uint32_t>(op - OP_ILOAD_0)});
		i.effect = {0, 1};
		break;
	case 0x1E:
	case 0x1F:
	case 0x20:
	case 0x21: // lload-0..3
		i.opcode = BcOpcode::LoadLocal;
		i.operands.push_back(BcLocalOperand{static_cast<uint32_t>(op - 0x1E)});
		i.effect = {0, 1};
		break;
	case 0x22:
	case 0x23:
	case 0x24:
	case 0x25: // fload-0..3
		i.opcode = BcOpcode::LoadLocal;
		i.operands.push_back(BcLocalOperand{static_cast<uint32_t>(op - 0x22)});
		i.effect = {0, 1};
		break;
	case 0x26:
	case 0x27:
	case 0x28:
	case 0x29: // dload-0..3
		i.opcode = BcOpcode::LoadLocal;
		i.operands.push_back(BcLocalOperand{static_cast<uint32_t>(op - 0x26)});
		i.effect = {0, 1};
		break;
	case 0x2A:
	case 0x2B:
	case 0x2C:
	case OP_ALOAD_3: // aload-0..3
		i.opcode = BcOpcode::LoadLocal;
		i.operands.push_back(BcLocalOperand{static_cast<uint32_t>(op - 0x2A)});
		i.effect = {0, 1};
		break;
	case OP_ILOAD:
	case OP_LLOAD:
	case OP_FLOAD:
	case OP_DLOAD:
	case OP_ALOAD:
		i.opcode = BcOpcode::LoadLocal;
		i.operands.push_back(BcLocalOperand{readU1()});
		i.effect = {0, 1};
		break;
	// Array loads
	case OP_IALOAD:
	case OP_LALOAD:
	case OP_FALOAD:
	case OP_DALOAD:
	case OP_AALOAD:
	case OP_BALOAD:
	case OP_CALOAD:
	case OP_SALOAD:
		i.opcode = BcOpcode::ArrayLoad;
		i.effect = {2, 1};
		break;
	// Stores (istore-0 through astore-3)
	case OP_ISTORE_0:
	case 0x3C:
	case 0x3D:
	case 0x3E:
		i.opcode = BcOpcode::StoreLocal;
		i.operands.push_back(BcLocalOperand{static_cast<uint32_t>(op - OP_ISTORE_0)});
		i.effect = {1, 0};
		break;
	case 0x3F:
	case 0x40:
	case 0x41:
	case 0x42:
		i.opcode = BcOpcode::StoreLocal;
		i.operands.push_back(BcLocalOperand{static_cast<uint32_t>(op - 0x3F)});
		i.effect = {1, 0};
		break;
	case 0x43:
	case 0x44:
	case 0x45:
	case 0x46:
		i.opcode = BcOpcode::StoreLocal;
		i.operands.push_back(BcLocalOperand{static_cast<uint32_t>(op - 0x43)});
		i.effect = {1, 0};
		break;
	case 0x47:
	case 0x48:
	case 0x49:
	case 0x4A:
		i.opcode = BcOpcode::StoreLocal;
		i.operands.push_back(BcLocalOperand{static_cast<uint32_t>(op - 0x47)});
		i.effect = {1, 0};
		break;
	case 0x4B:
	case 0x4C:
	case 0x4D:
	case OP_ASTORE_3:
		i.opcode = BcOpcode::StoreLocal;
		i.operands.push_back(BcLocalOperand{static_cast<uint32_t>(op - 0x4B)});
		i.effect = {1, 0};
		break;
	case OP_ISTORE:
	case OP_LSTORE:
	case OP_FSTORE:
	case OP_DSTORE:
	case OP_ASTORE:
		i.opcode = BcOpcode::StoreLocal;
		i.operands.push_back(BcLocalOperand{readU1()});
		i.effect = {1, 0};
		break;
	// Array stores
	case OP_IASTORE:
	case OP_LASTORE:
	case OP_FASTORE:
	case OP_DASTORE:
	case OP_AASTORE:
	case OP_BASTORE:
	case OP_CASTORE:
	case OP_SASTORE:
		i.opcode = BcOpcode::ArrayStore;
		i.effect = {3, 0};
		break;
	// Stack
	case OP_POP:
		i.opcode = BcOpcode::Pop;
		i.effect = {1, 0};
		break;
	case OP_POP2:
		i.opcode = BcOpcode::Pop2;
		i.effect = {2, 0};
		break;
	case OP_DUP:
		i.opcode = BcOpcode::Dup;
		i.effect = {1, 2};
		break;
	case OP_DUP_X1:
		i.opcode = BcOpcode::DupX1;
		i.effect = {2, 3};
		break;
	case OP_DUP_X2:
		i.opcode = BcOpcode::DupX2;
		i.effect = {3, 4};
		break;
	case OP_DUP2:
		i.opcode = BcOpcode::Dup2;
		i.effect = {2, 4};
		break;
	case OP_DUP2_X1:
		i.opcode = BcOpcode::Dup2X1;
		i.effect = {3, 5};
		break;
	case OP_DUP2_X2:
		i.opcode = BcOpcode::Dup2X2;
		i.effect = {4, 6};
		break;
	case OP_SWAP:
		i.opcode = BcOpcode::Swap;
		i.effect = {2, 2};
		break;
	// Arithmetic
	case OP_IADD:
		i.opcode = BcOpcode::Add;
		i.effect = {2, 1};
		break;
	case OP_LADD:
		i.opcode = BcOpcode::Add;
		i.effect = {2, 1};
		break;
	case OP_FADD:
		i.opcode = BcOpcode::FAdd;
		i.effect = {2, 1};
		break;
	case OP_DADD:
		i.opcode = BcOpcode::FAdd;
		i.effect = {2, 1};
		break;
	case OP_ISUB:
		i.opcode = BcOpcode::Sub;
		i.effect = {2, 1};
		break;
	case OP_LSUB:
		i.opcode = BcOpcode::Sub;
		i.effect = {2, 1};
		break;
	case OP_FSUB:
		i.opcode = BcOpcode::FSub;
		i.effect = {2, 1};
		break;
	case OP_DSUB:
		i.opcode = BcOpcode::FSub;
		i.effect = {2, 1};
		break;
	case OP_IMUL:
		i.opcode = BcOpcode::Mul;
		i.effect = {2, 1};
		break;
	case OP_LMUL:
		i.opcode = BcOpcode::Mul;
		i.effect = {2, 1};
		break;
	case OP_FMUL:
		i.opcode = BcOpcode::FMul;
		i.effect = {2, 1};
		break;
	case OP_DMUL:
		i.opcode = BcOpcode::FMul;
		i.effect = {2, 1};
		break;
	case OP_IDIV:
		i.opcode = BcOpcode::Div;
		i.effect = {2, 1};
		break;
	case OP_LDIV:
		i.opcode = BcOpcode::Div;
		i.effect = {2, 1};
		break;
	case OP_FDIV:
		i.opcode = BcOpcode::FDiv;
		i.effect = {2, 1};
		break;
	case OP_DDIV:
		i.opcode = BcOpcode::FDiv;
		i.effect = {2, 1};
		break;
	case OP_IREM:
		i.opcode = BcOpcode::Rem;
		i.effect = {2, 1};
		break;
	case OP_LREM:
		i.opcode = BcOpcode::Rem;
		i.effect = {2, 1};
		break;
	case OP_FREM:
		i.opcode = BcOpcode::FRem;
		i.effect = {2, 1};
		break;
	case OP_DREM:
		i.opcode = BcOpcode::FRem;
		i.effect = {2, 1};
		break;
	case OP_INEG:
		i.opcode = BcOpcode::Neg;
		i.effect = {1, 1};
		break;
	case OP_LNEG:
		i.opcode = BcOpcode::Neg;
		i.effect = {1, 1};
		break;
	case OP_FNEG:
		i.opcode = BcOpcode::FNeg;
		i.effect = {1, 1};
		break;
	case OP_DNEG:
		i.opcode = BcOpcode::FNeg;
		i.effect = {1, 1};
		break;
	case OP_ISHL:
		i.opcode = BcOpcode::Shl;
		i.effect = {2, 1};
		break;
	case OP_LSHL:
		i.opcode = BcOpcode::Shl;
		i.effect = {2, 1};
		break;
	case OP_ISHR:
		i.opcode = BcOpcode::Shr;
		i.effect = {2, 1};
		break;
	case OP_LSHR:
		i.opcode = BcOpcode::Shr;
		i.effect = {2, 1};
		break;
	case OP_IUSHR:
		i.opcode = BcOpcode::UShr;
		i.effect = {2, 1};
		break;
	case OP_LUSHR:
		i.opcode = BcOpcode::UShr;
		i.effect = {2, 1};
		break;
	case OP_IAND:
		i.opcode = BcOpcode::And;
		i.effect = {2, 1};
		break;
	case OP_LAND:
		i.opcode = BcOpcode::And;
		i.effect = {2, 1};
		break;
	case OP_IOR:
		i.opcode = BcOpcode::Or;
		i.effect = {2, 1};
		break;
	case OP_LOR:
		i.opcode = BcOpcode::Or;
		i.effect = {2, 1};
		break;
	case OP_IXOR:
		i.opcode = BcOpcode::Xor;
		i.effect = {2, 1};
		break;
	case OP_LXOR:
		i.opcode = BcOpcode::Xor;
		i.effect = {2, 1};
		break;
	case OP_IINC: {
		readU1();
		readS1();
		i.opcode = BcOpcode::Add;
		i.effect = {0, 0};
		break;
	}
	// Conversions
	case OP_I2L:
		i.opcode = BcOpcode::I2L;
		i.effect = {1, 1};
		break;
	case OP_I2F:
		i.opcode = BcOpcode::I2F;
		i.effect = {1, 1};
		break;
	case OP_I2D:
		i.opcode = BcOpcode::I2D;
		i.effect = {1, 1};
		break;
	case OP_L2I:
		i.opcode = BcOpcode::L2I;
		i.effect = {1, 1};
		break;
	case OP_L2F:
		i.opcode = BcOpcode::L2F;
		i.effect = {1, 1};
		break;
	case OP_L2D:
		i.opcode = BcOpcode::L2D;
		i.effect = {1, 1};
		break;
	case OP_F2I:
		i.opcode = BcOpcode::F2I;
		i.effect = {1, 1};
		break;
	case OP_F2L:
		i.opcode = BcOpcode::F2L;
		i.effect = {1, 1};
		break;
	case OP_F2D:
		i.opcode = BcOpcode::F2D;
		i.effect = {1, 1};
		break;
	case OP_D2I:
		i.opcode = BcOpcode::D2I;
		i.effect = {1, 1};
		break;
	case OP_D2L:
		i.opcode = BcOpcode::D2L;
		i.effect = {1, 1};
		break;
	case OP_D2F:
		i.opcode = BcOpcode::D2F;
		i.effect = {1, 1};
		break;
	case OP_I2B:
		i.opcode = BcOpcode::I2B;
		i.effect = {1, 1};
		break;
	case OP_I2C:
		i.opcode = BcOpcode::I2C;
		i.effect = {1, 1};
		break;
	case OP_I2S:
		i.opcode = BcOpcode::I2S;
		i.effect = {1, 1};
		break;
	// Comparisons
	case OP_LCMP:
		i.opcode = BcOpcode::CmpEq;
		i.effect = {2, 1};
		break;
	case OP_FCMPL:
		i.opcode = BcOpcode::FCmpL;
		i.effect = {2, 1};
		break;
	case OP_FCMPG:
		i.opcode = BcOpcode::FCmpG;
		i.effect = {2, 1};
		break;
	case OP_DCMPL:
		i.opcode = BcOpcode::FCmpL;
		i.effect = {2, 1};
		break;
	case OP_DCMPG:
		i.opcode = BcOpcode::FCmpG;
		i.effect = {2, 1};
		break;
	// Branches
	case OP_IFEQ: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfEq;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {1, 0};
		break;
	}
	case OP_IFNE: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfNe;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {1, 0};
		break;
	}
	case OP_IFLT: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfLt;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {1, 0};
		break;
	}
	case OP_IFGE: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfGe;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {1, 0};
		break;
	}
	case OP_IFGT: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfGt;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {1, 0};
		break;
	}
	case OP_IFLE: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfLe;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {1, 0};
		break;
	}
	case OP_IF_ICMPEQ: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfEq;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {2, 0};
		break;
	}
	case OP_IF_ICMPNE: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfNe;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {2, 0};
		break;
	}
	case OP_IF_ICMPLT: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfLt;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {2, 0};
		break;
	}
	case OP_IF_ICMPGE: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfGe;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {2, 0};
		break;
	}
	case OP_IF_ICMPGT: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfGt;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {2, 0};
		break;
	}
	case OP_IF_ICMPLE: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfLe;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {2, 0};
		break;
	}
	case OP_IF_ACMPEQ: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::CmpEq;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {2, 0};
		break;
	}
	case OP_IF_ACMPNE: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::CmpNe;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {2, 0};
		break;
	}
	case OP_GOTO: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::Goto;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {0, 0};
		break;
	}
	case OP_GOTO_W: {
		uint32_t t = branchPc(readS4(), instrPc);
		i.opcode = BcOpcode::Goto;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {0, 0};
		break;
	}
	case OP_JSR:
	case OP_JSR_W: {
		int32_t off = (op == OP_JSR) ? (int32_t)readS2() : readS4();
		uint32_t t = branchPc(off, instrPc);
		i.opcode = BcOpcode::Goto;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {0, 0};
		break;
	}
	case OP_RET: {
		readU1();
		i.opcode = BcOpcode::Return;
		i.effect = {0, 0};
		break;
	}
	case OP_TABLESWITCH: {
		size_t pad = (4 - ((pc) & 3)) & 3;
		while (pad--)
			pc++;
		int32_t def = readS4();
		int32_t lo = readS4();
		int32_t hi = readS4();
		BcSwitchTable sw;
		sw.defaultBlock = branchPc(def, instrPc);
		// Same bound as in findLeaders: lo/hi are file data and can claim
		// billions of cases, but each case is a 4-byte offset, so whatever is
		// left of code[] is the real limit. Counting in int64_t also keeps
		// hi == INT32_MAX from overflowing the loop variable.
		int64_t declared = static_cast<int64_t>(hi) - static_cast<int64_t>(lo) + 1;
		int64_t n = std::min<int64_t>(std::max<int64_t>(declared, 0), static_cast<int64_t>(avail() / 4));
		for (int64_t e = 0; e < n; ++e)
		{
			int32_t off = readS4();
			sw.cases.push_back({static_cast<int64_t>(lo) + e, branchPc(off, instrPc)});
		}
		i.opcode = BcOpcode::TableSwitch;
		i.operands.push_back(std::move(sw));
		i.effect = {1, 0};
		break;
	}
	case OP_LOOKUPSWITCH: {
		size_t pad = (4 - ((pc) & 3)) & 3;
		while (pad--)
			pc++;
		int32_t def = readS4();
		int32_t npairs = readS4();
		BcSwitchTable sw;
		sw.defaultBlock = branchPc(def, instrPc);
		// npairs is file data just like hi/lo above; a (key, offset) pair costs
		// 8 bytes, so code[] itself bounds how many can exist.
		int64_t n = std::min<int64_t>(std::max<int64_t>(npairs, 0), static_cast<int64_t>(avail() / 8));
		for (int64_t k = 0; k < n; ++k)
		{
			int32_t key = readS4();
			int32_t off = readS4();
			sw.cases.push_back({key, branchPc(off, instrPc)});
		}
		i.opcode = BcOpcode::LookupSwitch;
		i.operands.push_back(std::move(sw));
		i.effect = {1, 0};
		break;
	}
	case OP_IRETURN:
	case OP_LRETURN:
	case OP_FRETURN:
	case OP_DRETURN:
	case OP_ARETURN:
		i.opcode = BcOpcode::ReturnValue;
		i.effect = {1, 0};
		break;
	case OP_RETURN:
		i.opcode = BcOpcode::Return;
		i.effect = {0, 0};
		break;
	// Field access
	case OP_GETSTATIC: {
		uint16_t idx = readU2();
		i.opcode = BcOpcode::GetStatic;
		i.operands.push_back(BcFieldRef{pool_.refClass(idx), pool_.refName(idx), bc_module::types::Int(), true});
		i.effect = {0, 1};
		break;
	}
	case OP_PUTSTATIC: {
		uint16_t idx = readU2();
		i.opcode = BcOpcode::PutStatic;
		i.operands.push_back(BcFieldRef{pool_.refClass(idx), pool_.refName(idx), bc_module::types::Int(), true});
		i.effect = {1, 0};
		break;
	}
	case OP_GETFIELD: {
		uint16_t idx = readU2();
		i.opcode = BcOpcode::GetField;
		i.operands.push_back(BcFieldRef{pool_.refClass(idx), pool_.refName(idx), bc_module::types::Int(), false});
		i.effect = {1, 1};
		break;
	}
	case OP_PUTFIELD: {
		uint16_t idx = readU2();
		i.opcode = BcOpcode::PutField;
		i.operands.push_back(BcFieldRef{pool_.refClass(idx), pool_.refName(idx), bc_module::types::Int(), false});
		i.effect = {2, 0};
		break;
	}
	// Invocations
	case OP_INVOKEVIRTUAL: {
		uint16_t idx = readU2();
		BcMethodRef mr;
		mr.owner = pool_.refClass(idx);
		mr.name = pool_.refName(idx);
		i.opcode = BcOpcode::InvokeVirtual;
		i.operands.push_back(std::move(mr));
		i.effect = {-1, -1};
		break;
	}
	case OP_INVOKESPECIAL: {
		uint16_t idx = readU2();
		BcMethodRef mr;
		mr.owner = pool_.refClass(idx);
		mr.name = pool_.refName(idx);
		i.opcode = BcOpcode::InvokeSpecial;
		i.operands.push_back(std::move(mr));
		i.effect = {-1, -1};
		break;
	}
	case OP_INVOKESTATIC: {
		uint16_t idx = readU2();
		BcMethodRef mr;
		mr.owner = pool_.refClass(idx);
		mr.name = pool_.refName(idx);
		i.opcode = BcOpcode::InvokeStatic;
		i.operands.push_back(std::move(mr));
		i.effect = {-1, -1};
		break;
	}
	case OP_INVOKEINTERFACE: {
		uint16_t idx = readU2();
		readU1();
		readU1();
		BcMethodRef mr;
		mr.owner = pool_.refClass(idx);
		mr.name = pool_.refName(idx);
		mr.isInterface = true;
		i.opcode = BcOpcode::InvokeInterface;
		i.operands.push_back(std::move(mr));
		i.effect = {-1, -1};
		break;
	}
	case OP_INVOKEDYNAMIC: {
		uint16_t idx = readU2();
		readU1();
		readU1();
		BcMethodRef mr;
		mr.name = "<lambda>";
		i.opcode = BcOpcode::InvokeDynamic;
		i.operands.push_back(std::move(mr));
		i.effect = {-1, -1};
		break;
	}
	// Object creation
	case OP_NEW: {
		uint16_t idx = readU2();
		i.opcode = BcOpcode::New;
		i.operands.push_back(BcTypeOperand{bc_module::types::Class(pool_.className(idx))});
		i.effect = {0, 1};
		break;
	}
	case OP_NEWARRAY: {
		readU1();
		i.opcode = BcOpcode::NewArray;
		i.operands.push_back(BcTypeOperand{bc_module::types::Int()});
		i.effect = {1, 1};
		break;
	}
	case OP_ANEWARRAY: {
		uint16_t idx = readU2();
		i.opcode = BcOpcode::NewArray;
		i.operands.push_back(BcTypeOperand{bc_module::types::Class(pool_.className(idx))});
		i.effect = {1, 1};
		break;
	}
	case OP_ARRAYLENGTH:
		i.opcode = BcOpcode::ArrayLength;
		i.effect = {1, 1};
		break;
	case OP_ATHROW:
		i.opcode = BcOpcode::Throw;
		i.effect = {1, 0};
		break;
	case OP_CHECKCAST: {
		uint16_t idx = readU2();
		i.opcode = BcOpcode::CheckCast;
		i.operands.push_back(BcTypeOperand{bc_module::types::Class(pool_.className(idx))});
		i.effect = {1, 1};
		break;
	}
	case OP_INSTANCEOF: {
		uint16_t idx = readU2();
		i.opcode = BcOpcode::Instanceof;
		i.operands.push_back(BcTypeOperand{bc_module::types::Class(pool_.className(idx))});
		i.effect = {1, 1};
		break;
	}
	case OP_MONITORENTER:
		i.opcode = BcOpcode::MonitorEnter;
		i.effect = {1, 0};
		break;
	case OP_MONITOREXIT:
		i.opcode = BcOpcode::MonitorExit;
		i.effect = {1, 0};
		break;
	case OP_WIDE: {
		uint8_t wop = readU1();
		if (wop == OP_IINC)
		{
			readU2();
			readS2();
			i.opcode = BcOpcode::Add;
			i.effect = {0, 0};
		}
		else if (wop == OP_ILOAD || wop == OP_LLOAD || wop == OP_FLOAD || wop == OP_DLOAD || wop == OP_ALOAD)
		{
			uint16_t idx = readU2();
			i.opcode = BcOpcode::LoadLocal;
			i.operands.push_back(BcLocalOperand{idx});
			i.effect = {0, 1};
		}
		else
		{
			uint16_t idx = readU2();
			i.opcode = BcOpcode::StoreLocal;
			i.operands.push_back(BcLocalOperand{idx});
			i.effect = {1, 0};
		}
		break;
	}
	case OP_MULTIANEWARRAY: {
		uint16_t idx = readU2();
		uint8_t dims = readU1();
		i.opcode = BcOpcode::MultiNewArray;
		i.operands.push_back(BcTypeOperand{bc_module::types::Class(pool_.className(idx))});
		i.operands.push_back(BcIntOperand{dims});
		i.effect = {-1, 1};
		break;
	}
	case OP_IFNULL: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfNull;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {1, 0};
		break;
	}
	case OP_IFNONNULL: {
		uint32_t t = branchPc(readS2(), instrPc);
		i.opcode = BcOpcode::IfNonNull;
		i.operands.push_back(BcBlockOperand{t});
		i.effect = {1, 0};
		break;
	}
	default:
		i.opcode = BcOpcode::Nop;
		i.effect = {0, 0};
		break;
	}
	return i;
}

// ─── Pass 4: Annotate stack types ────────────────────────────────────────────

void JvmLifter::annotateStack(BcCFG& cfg, const std::string& /*methodDesc*/)
{
	// Minimal forward propagation: propagate stack depth (not full types).
	// Full type propagation requires StackMapTable analysis.
	// For now, just clear and mark entry block.
	if (cfg.blockCount() > 0) cfg.block(0).entryStack.clear();
}

// ─── LiftResult ──────────────────────────────────────────────────────────────

LiftResult JvmLifter::lift(const CodeAttr& code, const std::string& methodDesc)
{
	LiftResult res;
	try
	{
		// Build line-number map.
		lineMap_.clear();
		for (const auto& ln: code.lineNumbers)
			lineMap_[ln.startPc] = ln.lineNumber;

		// Drop exception_table entries whose protected region is not a region
		// at all before either pass sees them: findLeaders() would otherwise
		// make a leader out of the handler pc of an entry that is never wired,
		// and wireExceptions() would copy the malformed extent into the CFG.
		const uint64_t codeSize = std::min<uint64_t>(code.bytecode.size(), kCodeSizeCap);
		std::vector<ExceptionEntry> wellFormedEh;
		wellFormedEh.reserve(code.exceptionTable.size());
		for (const auto& e: code.exceptionTable)
			if (protectedRegionFits(e, codeSize)) wellFormedEh.push_back(e);

		auto leaders = findLeaders(code.bytecode, wellFormedEh);
		if (leaders.empty()) leaders.push_back(0);

		buildBlocks(res.cfg, code.bytecode, leaders, pool_);
		wireExceptions(res.cfg, wellFormedEh, leaders, pool_);
		if (opts_.annotateStack) annotateStack(res.cfg, methodDesc);

		res.ok = true;
	}
	catch (const std::exception& e)
	{
		res.ok = false;
		res.error = e.what();
	}
	return res;
}

} // namespace jvm_parser
} // namespace retdec
