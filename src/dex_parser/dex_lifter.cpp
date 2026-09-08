/**
 * @file src/dex_parser/dex_lifter.cpp
 * @brief Dalvik bytecode → BcCFG lifter (all 256 Dalvik opcodes).
 *
 * Reference: https://source.android.com/docs/core/runtime/dalvik-bytecode
 *
 * Instruction format encoding (Dalvik):
 *   Each instruction is 1..5 code units (u16 words).
 *   The low byte of word[0] is the opcode.
 *   The high byte and subsequent words carry register/operand data.
 *
 * Register naming: vN (local reg), vA, vB, vC etc. in format tables.
 * We encode register refs as BcOperand::Local{id=regN}.
 */

#include <memory>
#include "retdec/dex_parser/dex_lifter.h"
#include "retdec/bc_module/bc_instr.h"
#include "retdec/utils/bounds.h"
#include "retdec/utils/branch_target.h"
#include "retdec/utils/byte_order.h"
#include "retdec/utils/scan_cursor.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <set>
#include <unordered_map>

namespace retdec {
namespace dex_parser {

using namespace bc_module;

/// Two's-complement reading of the low @p bits bits of a Dalvik operand.
///
/// Dalvik carries its signed operands -- branch offsets, /lit8 and /lit16
/// literals, the const forms -- inside unsigned code units, so recovering one
/// means reinterpreting a fixed-width bit pattern as signed.
/// `static_cast<int16_t>(w)` does that only by implementation-defined
/// behaviour: converting an unsigned value above INT16_MAX to a signed type is
/// implementation-defined in C++17, and where a shift followed the conversion
/// -- as it did in the const/4 arm, `static_cast<int8_t>(vB << 4) >> 4` -- the
/// result was also a right shift of a negative value, implementation-defined in
/// every standard. This is the same defect dex_class_parser.cpp's
/// signExtendEncoded had, and it takes the same cure: byteorder::signExtendFrom
/// fills in unsigned arithmetic and converts once, at the end, through
/// leb128::toSigned, which is total.
///
/// The result is int64_t because every Dalvik signed operand fits in one and
/// the widening is where the value is used anyway; a caller storing a narrower
/// operand converts back explicitly, which is in range by construction.
static int64_t sext(uint64_t v, unsigned bits)
{
	return utils::byteorder::signExtendFrom(v, bits);
}

// ─── Dalvik opcode constants ─────────────────────────────────────────────────

// Opcode byte values (low byte of first code unit)
enum DalvikOp : uint8_t
{
	OP_NOP = 0x00,
	OP_MOVE = 0x01,
	OP_MOVE_FROM16 = 0x02,
	OP_MOVE_16 = 0x03,
	OP_MOVE_WIDE = 0x04,
	OP_MOVE_WIDE16 = 0x05,
	OP_MOVE_WIDE_16 = 0x06,
	OP_MOVE_OBJ = 0x07,
	OP_MOVE_OBJ16 = 0x08,
	OP_MOVE_OBJ_16 = 0x09,
	OP_MOVE_RESULT = 0x0a,
	OP_MOVE_RES_WIDE = 0x0b,
	OP_MOVE_RES_OBJ = 0x0c,
	OP_MOVE_EXCEPTION = 0x0d,
	OP_RETURN_VOID = 0x0e,
	OP_RETURN = 0x0f,
	OP_RETURN_WIDE = 0x10,
	OP_RETURN_OBJ = 0x11,
	OP_CONST_4 = 0x12,
	OP_CONST_16 = 0x13,
	OP_CONST = 0x14,
	OP_CONST_HIGH16 = 0x15,
	OP_CONST_WIDE_16 = 0x16,
	OP_CONST_WIDE_32 = 0x17,
	OP_CONST_WIDE = 0x18,
	OP_CONST_WIDE_H16 = 0x19,
	OP_CONST_STR = 0x1a,
	OP_CONST_STR_J = 0x1b,
	OP_CONST_CLASS = 0x1c,
	OP_MONITOR_ENTER = 0x1d,
	OP_MONITOR_EXIT = 0x1e,
	OP_CHECK_CAST = 0x1f,
	OP_INSTANCE_OF = 0x20,
	OP_ARRAY_LEN = 0x21,
	OP_NEW_INSTANCE = 0x22,
	OP_NEW_ARRAY = 0x23,
	OP_FILLED_NEW_ARR = 0x24,
	OP_FILLED_NEW_RANGE = 0x25,
	OP_FILL_ARRAY_DATA = 0x26,
	OP_THROW = 0x27,
	OP_GOTO = 0x28,
	OP_GOTO_16 = 0x29,
	OP_GOTO_32 = 0x2a,
	OP_PACKED_SWITCH = 0x2b,
	OP_SPARSE_SWITCH = 0x2c,
	OP_CMPL_FLOAT = 0x2d,
	OP_CMPG_FLOAT = 0x2e,
	OP_CMPL_DOUBLE = 0x2f,
	OP_CMPG_DOUBLE = 0x30,
	OP_CMP_LONG = 0x31,
	OP_IF_EQ = 0x32,
	OP_IF_NE = 0x33,
	OP_IF_LT = 0x34,
	OP_IF_GE = 0x35,
	OP_IF_GT = 0x36,
	OP_IF_LE = 0x37,
	OP_IF_EQZ = 0x38,
	OP_IF_NEZ = 0x39,
	OP_IF_LTZ = 0x3a,
	OP_IF_GEZ = 0x3b,
	OP_IF_GTZ = 0x3c,
	OP_IF_LEZ = 0x3d,
	// 0x3e-0x43 unused
	OP_AGET = 0x44,
	OP_AGET_WIDE = 0x45,
	OP_AGET_OBJ = 0x46,
	OP_AGET_BOOL = 0x47,
	OP_AGET_BYTE = 0x48,
	OP_AGET_CHAR = 0x49,
	OP_AGET_SHORT = 0x4a,
	OP_APUT = 0x4b,
	OP_APUT_WIDE = 0x4c,
	OP_APUT_OBJ = 0x4d,
	OP_APUT_BOOL = 0x4e,
	OP_APUT_BYTE = 0x4f,
	OP_APUT_CHAR = 0x50,
	OP_APUT_SHORT = 0x51,
	OP_IGET = 0x52,
	OP_IGET_WIDE = 0x53,
	OP_IGET_OBJ = 0x54,
	OP_IGET_BOOL = 0x55,
	OP_IGET_BYTE = 0x56,
	OP_IGET_CHAR = 0x57,
	OP_IGET_SHORT = 0x58,
	OP_IPUT = 0x59,
	OP_IPUT_WIDE = 0x5a,
	OP_IPUT_OBJ = 0x5b,
	OP_IPUT_BOOL = 0x5c,
	OP_IPUT_BYTE = 0x5d,
	OP_IPUT_CHAR = 0x5e,
	OP_IPUT_SHORT = 0x5f,
	OP_SGET = 0x60,
	OP_SGET_WIDE = 0x61,
	OP_SGET_OBJ = 0x62,
	OP_SGET_BOOL = 0x63,
	OP_SGET_BYTE = 0x64,
	OP_SGET_CHAR = 0x65,
	OP_SGET_SHORT = 0x66,
	OP_SPUT = 0x67,
	OP_SPUT_WIDE = 0x68,
	OP_SPUT_OBJ = 0x69,
	OP_SPUT_BOOL = 0x6a,
	OP_SPUT_BYTE = 0x6b,
	OP_SPUT_CHAR = 0x6c,
	OP_SPUT_SHORT = 0x6d,
	OP_INVOKE_VIRTUAL = 0x6e,
	OP_INVOKE_SUPER = 0x6f,
	OP_INVOKE_DIRECT = 0x70,
	OP_INVOKE_STATIC = 0x71,
	OP_INVOKE_INTERFACE = 0x72,
	// 0x73 unused
	OP_INVOKE_VIRT_RANGE = 0x74,
	OP_INVOKE_SUPER_RANGE = 0x75,
	OP_INVOKE_DIRECT_RANGE = 0x76,
	OP_INVOKE_STATIC_RANGE = 0x77,
	OP_INVOKE_IFACE_RANGE = 0x78,
	// 0x79-0x7a unused
	OP_NEG_INT = 0x7b,
	OP_NOT_INT = 0x7c,
	OP_NEG_LONG = 0x7d,
	OP_NOT_LONG = 0x7e,
	OP_NEG_FLOAT = 0x7f,
	OP_NEG_DOUBLE = 0x80,
	OP_INT_TO_LONG = 0x81,
	OP_INT_TO_FLOAT = 0x82,
	OP_INT_TO_DOUBLE = 0x83,
	OP_LONG_TO_INT = 0x84,
	OP_LONG_TO_FLOAT = 0x85,
	OP_LONG_TO_DOUBLE = 0x86,
	OP_FLOAT_TO_INT = 0x87,
	OP_FLOAT_TO_LONG = 0x88,
	OP_FLOAT_TO_DOUBLE = 0x89,
	OP_DOUBLE_TO_INT = 0x8a,
	OP_DOUBLE_TO_LONG = 0x8b,
	OP_DOUBLE_TO_FLOAT = 0x8c,
	OP_INT_TO_BYTE = 0x8d,
	OP_INT_TO_CHAR = 0x8e,
	OP_INT_TO_SHORT = 0x8f,
	OP_ADD_INT = 0x90,
	OP_SUB_INT = 0x91,
	OP_MUL_INT = 0x92,
	OP_DIV_INT = 0x93,
	OP_REM_INT = 0x94,
	OP_AND_INT = 0x95,
	OP_OR_INT = 0x96,
	OP_XOR_INT = 0x97,
	OP_SHL_INT = 0x98,
	OP_SHR_INT = 0x99,
	OP_USHR_INT = 0x9a,
	OP_ADD_LONG = 0x9b,
	OP_SUB_LONG = 0x9c,
	OP_MUL_LONG = 0x9d,
	OP_DIV_LONG = 0x9e,
	OP_REM_LONG = 0x9f,
	OP_AND_LONG = 0xa0,
	OP_OR_LONG = 0xa1,
	OP_XOR_LONG = 0xa2,
	OP_SHL_LONG = 0xa3,
	OP_SHR_LONG = 0xa4,
	OP_USHR_LONG = 0xa5,
	OP_ADD_FLOAT = 0xa6,
	OP_SUB_FLOAT = 0xa7,
	OP_MUL_FLOAT = 0xa8,
	OP_DIV_FLOAT = 0xa9,
	OP_REM_FLOAT = 0xaa,
	OP_ADD_DOUBLE = 0xab,
	OP_SUB_DOUBLE = 0xac,
	OP_MUL_DOUBLE = 0xad,
	OP_DIV_DOUBLE = 0xae,
	OP_REM_DOUBLE = 0xaf,
	OP_ADD_INT_2ADDR = 0xb0,
	OP_SUB_INT_2ADDR = 0xb1,
	OP_MUL_INT_2ADDR = 0xb2,
	OP_DIV_INT_2ADDR = 0xb3,
	OP_REM_INT_2ADDR = 0xb4,
	OP_AND_INT_2ADDR = 0xb5,
	OP_OR_INT_2ADDR = 0xb6,
	OP_XOR_INT_2ADDR = 0xb7,
	OP_SHL_INT_2ADDR = 0xb8,
	OP_SHR_INT_2ADDR = 0xb9,
	OP_USHR_INT_2ADDR = 0xba,
	OP_ADD_LONG_2ADDR = 0xbb,
	OP_SUB_LONG_2ADDR = 0xbc,
	OP_MUL_LONG_2ADDR = 0xbd,
	OP_DIV_LONG_2ADDR = 0xbe,
	OP_REM_LONG_2ADDR = 0xbf,
	OP_AND_LONG_2ADDR = 0xc0,
	OP_OR_LONG_2ADDR = 0xc1,
	OP_XOR_LONG_2ADDR = 0xc2,
	OP_SHL_LONG_2ADDR = 0xc3,
	OP_SHR_LONG_2ADDR = 0xc4,
	OP_USHR_LONG_2ADDR = 0xc5,
	OP_ADD_FLOAT_2ADDR = 0xc6,
	OP_SUB_FLOAT_2ADDR = 0xc7,
	OP_MUL_FLOAT_2ADDR = 0xc8,
	OP_DIV_FLOAT_2ADDR = 0xc9,
	OP_REM_FLOAT_2ADDR = 0xca,
	OP_ADD_DBL_2ADDR = 0xcb,
	OP_SUB_DBL_2ADDR = 0xcc,
	OP_MUL_DBL_2ADDR = 0xcd,
	OP_DIV_DBL_2ADDR = 0xce,
	OP_REM_DBL_2ADDR = 0xcf,
	OP_ADD_INT_LIT16 = 0xd0,
	OP_RSUB_INT = 0xd1,
	OP_MUL_INT_LIT16 = 0xd2,
	OP_DIV_INT_LIT16 = 0xd3,
	OP_REM_INT_LIT16 = 0xd4,
	OP_AND_INT_LIT16 = 0xd5,
	OP_OR_INT_LIT16 = 0xd6,
	OP_XOR_INT_LIT16 = 0xd7,
	OP_ADD_INT_LIT8 = 0xd8,
	OP_RSUB_INT_LIT8 = 0xd9,
	OP_MUL_INT_LIT8 = 0xda,
	OP_DIV_INT_LIT8 = 0xdb,
	OP_REM_INT_LIT8 = 0xdc,
	OP_AND_INT_LIT8 = 0xdd,
	OP_OR_INT_LIT8 = 0xde,
	OP_XOR_INT_LIT8 = 0xdf,
	OP_SHL_INT_LIT8 = 0xe0,
	OP_SHR_INT_LIT8 = 0xe1,
	OP_USHR_INT_LIT8 = 0xe2,
	// 0xe3-0xf9 unused/internal ART opcodes
	OP_INVOKE_POLYMORPHIC = 0xfa,
	OP_INVOKE_POLYMORPHIC_RANGE = 0xfb,
	OP_INVOKE_CUSTOM = 0xfc,
	OP_INVOKE_CUSTOM_RANGE = 0xfd,
	OP_CONST_METHOD_HANDLE = 0xfe,
	OP_CONST_METHOD_TYPE = 0xff,
};

// ─── Code unit size lookup ────────────────────────────────────────────────────

// Payload pseudo-instructions (packed-switch, sparse-switch, fill-array-data)
// sit in the instruction stream and are named by their whole first code unit.
// Their low byte -- the part that would be the opcode -- is 0x00, which is
// `nop`, so a payload cannot be recognised from an opcode table at all.
static constexpr uint16_t kPackedSwitchPayload = 0x0100;
static constexpr uint16_t kSparseSwitchPayload = 0x0200;
static constexpr uint16_t kFillArrayDataPayload = 0x0300;

// Instruction size in code units per opcode, from the format each opcode is
// declared with in the Dalvik bytecode reference: 10x/12x/11n/11x/10t are one
// unit, the 2* formats two, the 3* formats three, 45cc/4rcc four, 51l five.
// 0 marks an opcode the format does not define; the walkers step one unit for
// those rather than trusting a size nothing specifies.
//
// This table is load-bearing for correctness, not just for speed: every walk
// over a code_item steps by it, so a wrong entry does not fail loudly -- the
// walk lands mid-instruction and decodes operand words as opcodes, and valid
// Java comes out as a plausible but wrong CFG.  Sixteen entries were wrong,
// including invoke-virtual and invoke-super (the two most common instructions
// in compiled Java, sized 2 instead of 3) and goto (sized 2 instead of 1).
// packed-switch and sparse-switch were given a size of 0 to route them into
// the payload arm of the walker, which is what a payload's *identifier* is
// for; that made both switch instructions undecodable and left the real
// payloads to be decoded as instructions.  See DexInsnSize.* in
// tests/dex_parser/dex_parser_test.cpp, which pins each of these.
static const uint8_t kInsnSize[256] = {
	//  0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f
	1, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 1, 1, 1, 1, 1, // 00-0f
	1, 1, 1, 2, 3, 2, 2, 3, 5, 2, 2, 3, 2, 1, 1, 2, // 10-1f
	2, 1, 2, 2, 3, 3, 3, 1, 1, 2, 3, 3, 3, 2, 2, 2, // 20-2f
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 0, 0, // 30-3f (3e,3f unused)
	0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 40-4f (40-43 unused)
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 50-5f
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, // 60-6f (6e,6f = invoke)
	3, 3, 3, 0, 3, 3, 3, 3, 3, 0, 0, 1, 1, 1, 1, 1, // 70-7f (73,79,7a unused)
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // 80-8f
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // 90-9f
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // a0-af
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // b0-bf
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, // c0-cf
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, // d0-df
	2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // e0-ef (e3-ef unused)
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 3, 3, 2, 2, // f0-ff (f0-f9 unused)
};

// True when this code unit begins a payload rather than an instruction.
static inline bool isPayloadIdent(uint16_t ident)
{
	return ident == kPackedSwitchPayload || ident == kSparseSwitchPayload || ident == kFillArrayDataPayload;
}

/// Resolve the case targets of a packed-switch or sparse-switch at @p off.
///
/// The instruction carries a signed 32-bit branch offset in word[1..2], taken
/// relative to the switch instruction itself, naming a payload elsewhere in
/// the same instruction array:
///
///   packed-switch-payload   ident, size, first_key(2 units), targets[size](2)
///   sparse-switch-payload   ident, size, keys[size](2), targets[size](2)
///
/// and each target is itself a signed offset relative to the switch. Every
/// quantity here comes out of the file -- the branch offset, the identifier at
/// the far end of it, the entry count, each target -- so each is checked
/// against what the array can supply before it is used, in size_t, through the
/// verified kernel. An unresolvable switch yields no targets rather than a
/// guess; a switch whose payload is truncated yields the targets that are
/// actually there.
/// The block operand for a branch whose target is not inside this method.
///
/// The same sentinel jvm_lifter.cpp:198 uses, for the same reason: a target
/// that does not exist has to be distinguishable from offset 0, and every
/// consumer that resolves a block operand already has to cope with a label it
/// cannot find.
static constexpr uint32_t kNoBranchTarget = UINT32_MAX;

/// The absolute code-unit offset a relative Dalvik branch names, or nothing.
///
/// switchTargets below already forms its sum in int64 and tests it against the
/// code-unit count before use; the goto and if-test arms did neither. A
/// negative displacement wrapped to roughly four billion, buildBlocks created a
/// block for it, and the wrapped value became a real CFG edge -- `goto -8` at
/// offset 0 in a two-code-unit method produced four blocks, one of them
/// labelled L4294967288, at an offset no instruction falls into.
/// include/retdec/utils/branch_target.h states the rule and is proved over the
/// whole 64-bit domain; jvm_lifter refuses the equivalent JVM input through it.
static std::optional<uint32_t> branchTarget(uint32_t off, std::int64_t delta, std::size_t total)
{
	std::uint64_t target = 0;
	if (!utils::btgt::relative(off, delta, total, target)) return std::nullopt;
	return static_cast<uint32_t>(target);
}

static std::vector<uint32_t> switchTargets(const std::vector<uint16_t>& insns, uint32_t off)
{
	std::vector<uint32_t> targets;
	const size_t total = insns.size();
	if (!utils::bounds::rangeFits(off, total, 3)) return targets;

	const int64_t rel = sext(static_cast<uint32_t>(insns[off + 1]) | (static_cast<uint32_t>(insns[off + 2]) << 16), 32);
	const int64_t payloadOff = static_cast<int64_t>(off) + rel;
	if (payloadOff < 0 || static_cast<uint64_t>(payloadOff) >= total) return targets;
	const size_t p = static_cast<size_t>(payloadOff);

	if (!utils::bounds::rangeFits(p, total, 2)) return targets;
	const uint16_t ident = insns[p];
	const size_t count = insns[p + 1];

	size_t firstTarget, need;
	if (ident == kPackedSwitchPayload)
	{
		firstTarget = p + 4;
		need = 4 + count * 2;
	}
	else if (ident == kSparseSwitchPayload)
	{
		firstTarget = p + 2 + count * 2;
		need = 2 + count * 4;
	}
	else
	{
		return targets; // the branch offset does not name a switch payload
	}
	if (!utils::bounds::rangeFits(p, total, need)) return targets;

	targets.reserve(count);
	for (size_t i = 0; i < count; ++i)
	{
		const size_t t = firstTarget + i * 2;
		const int64_t delta = sext(static_cast<uint32_t>(insns[t]) | (static_cast<uint32_t>(insns[t + 1]) << 16), 32);
		const int64_t target = static_cast<int64_t>(off) + delta;
		if (target >= 0 && static_cast<uint64_t>(target) < total) targets.push_back(static_cast<uint32_t>(target));
	}
	return targets;
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Convert a DEX type descriptor string to BcType (minimal version for lifter).
static BcType dexDescToType(const std::string& desc)
{
	if (desc.empty()) return types::Void();
	switch (desc[0])
	{
	case 'V': return types::Void();
	case 'Z': return types::Bool();
	case 'B': return types::Byte();
	case 'S': return types::Short();
	case 'C': return types::Char();
	case 'I': return types::Int();
	case 'J': return types::Long();
	case 'F': return types::Float();
	case 'D': return types::Double();
	default: {
		BcRefType ref;
		ref.kind = BcRefKind::Class;
		ref.className = desc;
		return BcType{ref};
	}
	}
}

// Parse a DEX method descriptor "(params)ret" → BcFuncType (simplified).
static BcFuncType parseDexProto(const std::string& proto)
{
	BcFuncType ft;
	if (proto.empty() || proto[0] != '(')
	{
		ft.returnType = nullptr; // void
		return ft;
	}
	size_t closeP = proto.find(')');
	if (closeP == std::string::npos)
	{
		ft.returnType = nullptr;
		return ft;
	}
	// A method's arguments have to fit in a caller's code_item.outs_size, which
	// is a u2, so no callable method has more than 65535 of them. Without that
	// bound the descriptor's length is the only limit, and it is built from the
	// file's own type list -- N parameters each naming an L-byte type give an
	// N*L descriptor and one BcType node per parameter. It is the format's
	// number, not a cap chosen here.
	static constexpr size_t kMaxParams = 65535;

	// Parse params.
	//
	// The step is taken by utils::scan::advance rather than by assigning to a
	// bare index, because the array arm below had no `end == std::string::npos`
	// guard where its sibling 'L' arm at the top of the loop does. On the
	// descriptor "([L)V" -- a '[' whose element type names no class, which the
	// string table can hand us -- closeP is 3, the '[' arm runs at i = 1 with
	// j = 2, proto.find(';', 2) is npos = SIZE_MAX, and `i = end + 1` wraps to
	// 0. The cursor then cycles 0 -> 1 -> 0 -> 1, pushing two BcType nodes per
	// cycle, and the only thing that stops it is the kMaxParams cap -- which is
	// why the artifact was 65535 bogus parameters rather than a hang. The npos
	// guard is restored below; advance() is what makes the absence of another
	// one a refusal rather than a wrap, since it rejects a step of zero and a
	// step that leaves the descriptor.
	utils::scan::Cursor cur = utils::scan::cursorOver(proto.size());
	utils::scan::seek(cur, 1); // past the '(' proto[0] was checked to be

	while (cur.pos < closeP && ft.params.size() < kMaxParams)
	{
		const size_t i = cur.pos;
		char c = proto[i];
		size_t next = i + 1;
		if (c == 'L')
		{
			size_t end = proto.find(';', i);
			if (end == std::string::npos) break;
			ft.params.push_back(std::make_shared<BcType>(dexDescToType(proto.substr(i, end - i + 1))));
			next = end + 1;
		}
		else if (c == '[')
		{
			// Array type — find element
			size_t j = i + 1;
			while (j < closeP && proto[j] == '[')
				++j;
			if (j < closeP && proto[j] == 'L')
			{
				size_t end = proto.find(';', j);
				if (end == std::string::npos) break;
				ft.params.push_back(std::make_shared<BcType>(dexDescToType(proto.substr(i, end - i + 1))));
				next = end + 1;
			}
			else
			{
				ft.params.push_back(std::make_shared<BcType>(dexDescToType(proto.substr(i, j - i + 1))));
				next = j + 1;
			}
		}
		else
		{
			ft.params.push_back(std::make_shared<BcType>(dexDescToType(std::string(1, c))));
			next = i + 1;
		}
		// next > i on every path above, and next <= proto.size(), so this
		// refuses only when the arms themselves have gone wrong.
		if (!utils::scan::advance(cur, next - i)) break;
	}
	ft.returnType = std::make_shared<BcType>(dexDescToType(proto.substr(closeP + 1)));
	return ft;
}

static BcOperand makeReg(uint32_t regNum)
{
	return BcLocalOperand{regNum};
}
static BcOperand makeInt(int64_t v)
{
	return BcIntOperand{v};
}
static BcOperand makeStr(const std::string& s)
{
	return BcStringOperand{s};
}
static BcOperand makeTypeRef(const std::string& desc)
{
	return BcTypeOperand{dexDescToType(desc)};
}
static BcOperand makeMethodRef(const std::string& owner, const std::string& name, const std::string& proto)
{
	BcMethodRef ref;
	ref.owner = owner;
	ref.name = name;
	ref.descriptor = parseDexProto(proto);
	return ref;
}
static BcOperand makeFieldRef(const std::string& owner, const std::string& name, const std::string& typeDesc)
{
	BcFieldRef ref;
	ref.owner = owner;
	ref.name = name;
	ref.type = dexDescToType(typeDesc);
	return ref;
}
static BcOperand makeBlock(uint32_t id)
{
	return BcBlockOperand{id};
}

// Extract nibbles from the high byte of word[0]
static uint8_t highA(uint16_t w0)
{
	return (w0 >> 8) & 0xF;
}
static uint8_t highB(uint16_t w0)
{
	return (w0 >> 12) & 0xF;
}

// ─── DexLifter ───────────────────────────────────────────────────────────────

DexLifter::DexLifter(const DexFile& dexFile, LiftOptions opts): dex_(dexFile), opts_(opts) {}

DexLiftResult DexLifter::lift(const CodeItem& code, uint32_t methodIdx)
{
	DexLiftResult result;
	try
	{
		auto leaders = findLeaders(code);
		buildBlocks(result.cfg, code, leaders);
		if (opts_.emitAnnotations) wireExceptions(result.cfg, code, leaders);
	}
	catch (const std::exception& e)
	{
		result.status = DexLiftResult::Error;
		result.error = e.what();
	}
	(void)methodIdx;
	return result;
}

std::vector<uint32_t> DexLifter::findLeaders(const CodeItem& code) const
{
	std::set<uint32_t> leaders;
	leaders.insert(0);

	const auto& insns = code.insns;
	const uint32_t total = static_cast<uint32_t>(insns.size());

	uint32_t off = 0;
	// insns_size is a file-declared count, so a code_item may legitimately stop
	// in the middle of an instruction: the opcode in the last code unit can
	// declare two or three units and the array supplies only one.  The switch
	// below used to index word[1] and word[2] straight off `insns` on the
	// strength of the opcode alone, which reads past the end of the vector for
	// a truncated goto/16, goto/32 or if-*.  Every unit after the opcode goes
	// through this accessor instead; it asks whether the array can actually
	// supply the unit before touching it, and reads zero when it cannot.  The
	// question is asked in size_t via the verified kernel because `off + i`
	// is 32-bit here and would wrap for an offset near the end of the range.
	// decodeInsn()'s `w` accessor guards its reads through the same helper;
	// this is the copy of that lambda findLeaders was missing.
	auto unit = [&](uint32_t i) -> uint16_t {
		return utils::bounds::rangeFits(off, total, static_cast<size_t>(i) + 1) ? insns[off + i] : 0;
	};

	while (off < total)
	{
		const uint16_t ident = insns[off];

		// A payload is named by its whole first code unit, not by the low byte
		// of it.  This arm used to be reached through `kInsnSize[op] == 0` --
		// with packed-switch and sparse-switch given a size of 0 so that they
		// would reach it -- and then asked whether `ident & 0xFF` was 1, 2 or
		// 3.  The low byte of a payload identifier is 0x00, so that test could
		// never hold; the arm only ever ran for the two switch *instructions*,
		// where it fell through to `advance = 1` and walked into their
		// operands.  Real payloads, meanwhile, look like `nop` and were
		// decoded as instructions.  Ask the question the format answers.
		if (isPayloadIdent(ident))
		{
			// packed-switch:    word[0]=0x0100, word[1]=size, then targets
			// sparse-switch:    word[0]=0x0200, word[1]=size, keys+targets
			// fill-array-data:  word[0]=0x0300, word[1]=elem_width,
			//                   word[2..3]=num_elems, then the data
			size_t advance;
			if (ident == kPackedSwitchPayload)
			{
				advance = 4 + static_cast<size_t>(unit(1)) * 2;
			}
			else if (ident == kSparseSwitchPayload)
			{
				advance = 2 + static_cast<size_t>(unit(1)) * 4;
			}
			else
			{
				const size_t elemWidth = unit(1);
				const size_t numElems = static_cast<size_t>(unit(2)) | (static_cast<size_t>(unit(3)) << 16);
				// num_elements * element_width is a 48-bit product; forming
				// it in uint32_t (as this did) wraps, and the short advance
				// that comes back drops the walk into the middle of the
				// payload, where it decodes array data as opcodes.
				advance = 4 + (numElems * elemWidth + 1) / 2;
			}
			// A payload declares its own length, so cap the step at one unit
			// past the end of the array: an oversized length then leaves the
			// loop instead of overflowing the 32-bit `off` back to the start.
			// For a well-formed payload the whole of it is inside the array,
			// so the clamp never binds.
			off += static_cast<uint32_t>(utils::bounds::clamp(advance, utils::bounds::remaining(off, total) + 1));
			continue;
		}

		const uint8_t op = ident & 0xFF;
		// An opcode the format does not define carries no operands to skip.
		const uint32_t sz = kInsnSize[op] ? kInsnSize[op] : 1u;

		switch (op)
		{
		case OP_GOTO: {
			const int64_t offset = sext((insns[off] >> 8) & 0xFF, 8);
			if (auto t = branchTarget(off, offset, insns.size())) leaders.insert(*t);
			leaders.insert(off + sz);
			break;
		}
		case OP_GOTO_16: {
			const int64_t offset = sext(unit(1), 16);
			if (auto t = branchTarget(off, offset, insns.size())) leaders.insert(*t);
			leaders.insert(off + sz);
			break;
		}
		case OP_GOTO_32: {
			const int64_t offset = sext(static_cast<uint32_t>(unit(1)) | (static_cast<uint32_t>(unit(2)) << 16), 32);
			if (auto t = branchTarget(off, offset, insns.size())) leaders.insert(*t);
			leaders.insert(off + sz);
			break;
		}
		case OP_IF_EQ:
		case OP_IF_NE:
		case OP_IF_LT:
		case OP_IF_GE:
		case OP_IF_GT:
		case OP_IF_LE:
		case OP_IF_EQZ:
		case OP_IF_NEZ:
		case OP_IF_LTZ:
		case OP_IF_GEZ:
		case OP_IF_GTZ:
		case OP_IF_LEZ: {
			const int64_t offset = sext(unit(1), 16);
			if (auto t = branchTarget(off, offset, insns.size())) leaders.insert(*t);
			leaders.insert(off + sz);
			break;
		}
		case OP_RETURN_VOID:
		case OP_RETURN:
		case OP_RETURN_WIDE:
		case OP_RETURN_OBJ:
		case OP_THROW: leaders.insert(off + sz); break;
		case OP_PACKED_SWITCH:
		case OP_SPARSE_SWITCH:
			// Each case target begins a block, and so does the unit after
			// the switch -- a Dalvik switch falls through when no case
			// matches. Only the fall-through was recorded before, so a
			// switch reached its cases through no edge at all and every
			// case body looked unreachable.
			for (uint32_t target: switchTargets(insns, off))
				leaders.insert(target);
			leaders.insert(off + sz);
			break;
		default: break;
		}

		off += sz;
	}

	// Exception handler entries are also leaders
	for (const auto& t: code.tries)
	{
		// Same bound as the branch arms. startAddr is a u4 and insnCount a u2,
		// both copied verbatim out of the code_item, and their sum was formed
		// in uint32: startAddr = 0xFFFFFFFF with insnCount = 2 gives an end of
		// 1, a region ending four gigabytes before it begins. jvm_lifter
		// applies protectedRegionFits to the equivalent JVM entry; this had no
		// equivalent.
		std::uint64_t end = 0;
		if (!utils::btgt::region(t.startAddr, t.insnCount, insns.size(), end)) continue;
		leaders.insert(t.startAddr);
		leaders.insert(static_cast<uint32_t>(end));
	}
	for (size_t i = 0; i < code.handlers.handlers.size(); ++i)
	{
		for (const auto& h: code.handlers.handlers[i])
		{
			if (h.addr < insns.size()) leaders.insert(h.addr);
		}
		// catchAllAddrs is a SEPARATE vector from handlers, and nothing in the
		// type keeps them the same length -- the parser resizes both, but
		// DexLifter::lift takes a CodeItem from any caller.
		if (i < code.handlers.catchAllAddrs.size() && code.handlers.catchAllAddrs[i] != ~0u
				&& code.handlers.catchAllAddrs[i] < insns.size())
		{
			leaders.insert(code.handlers.catchAllAddrs[i]);
		}
	}

	return std::vector<uint32_t>(leaders.begin(), leaders.end());
}

void DexLifter::buildBlocks(BcCFG& cfg, const CodeItem& code, const std::vector<uint32_t>& leaders)
{
	// Map from code-unit offset → block id
	std::unordered_map<uint32_t, BlockId> offsetToBlock;
	for (size_t i = 0; i < leaders.size(); ++i)
	{
		auto& blk = cfg.addBlock();
		blk.label = "L" + std::to_string(leaders[i]);
		offsetToBlock[leaders[i]] = blk.id;
	}

	const auto& insns = code.insns;
	const uint32_t total = static_cast<uint32_t>(insns.size());

	nextInstrId_ = 0;
	for (size_t li = 0; li < leaders.size(); ++li)
	{
		uint32_t start = leaders[li];
		uint32_t end = (li + 1 < leaders.size()) ? leaders[li + 1] : total;

		if (start >= total) continue;

		BcBasicBlock& blkRef = cfg.block(offsetToBlock.at(start));
		BcBasicBlock* blk = &blkRef;

		uint32_t off = start;
		while (off < end && off < total)
		{
			// Payload data, not instructions.  Recognised by the whole code
			// unit; the low byte of a payload identifier is `nop`, so keying
			// this off the opcode (as `kInsnSize[op] == 0` did) both missed
			// every real payload and stopped the block at the two switch
			// instructions, which is why no switch was ever decoded.
			if (isPayloadIdent(insns[off])) break;

			const uint8_t op = insns[off] & 0xFF;
			const uint32_t sz = kInsnSize[op] ? kInsnSize[op] : 1u;
			uint32_t consumed = decodeInsn(*blk, insns, off, dex_);
			if (consumed == 0) consumed = sz;
			off += consumed;
		}

		// Add fall-through edge if block doesn't end with terminator
		if (li + 1 < leaders.size())
		{
			uint32_t nextLeader = leaders[li + 1];
			if (offsetToBlock.count(nextLeader) && !blk->instrs.empty())
			{
				auto& lastInsn = blk->instrs.back();
				bool isTerminator =
					lastInsn.opcode == BcOpcode::DALVIK_GOTO || lastInsn.opcode == BcOpcode::DALVIK_RETURN_VOID
					|| lastInsn.opcode == BcOpcode::DALVIK_RETURN || lastInsn.opcode == BcOpcode::DALVIK_THROW;
				if (!isTerminator) cfg.addEdge(blk->id, offsetToBlock.at(nextLeader));
			}
		}

		// Wire branch targets
		if (!blk->instrs.empty())
		{
			auto& last = blk->instrs.back();
			for (auto& op: last.operands)
			{
				if (auto* bop = std::get_if<BcBlockOperand>(&op))
				{
					// Target offset was encoded as blockId during decodeInsn.
					uint32_t targetOff = bop->blockId;
					if (offsetToBlock.count(targetOff)) cfg.addEdge(blk->id, offsetToBlock.at(targetOff));
				}
			}
		}
	}
}

void DexLifter::wireExceptions(BcCFG& cfg, const CodeItem& code, const std::vector<uint32_t>& leaders)
{
	(void)leaders; // Used via offsetToBlock lookup inside buildBlocks
	// For each try entry, locate the handler blocks and register them.
	for (size_t ti = 0; ti < code.tries.size(); ++ti)
	{
		const TryItem& t = code.tries[ti];
		// Find handler list index via handlerOff (byte offset into handler list)
		// We map by index since we parsed them sequentially.
		if (ti >= code.handlers.handlers.size()) break;

		// findLeaders() refuses a try whose region does not fit; this is where
		// the same numbers become BcExceptionHandler::startOffset/endOffset and
		// reach every consumer that asks how long the region is, so it has to
		// refuse the same ones. Without it, startAddr = 0xFFFFFFFF with
		// insnCount = 2 yields start=4294967295 end=1, and end - start read back
		// as a uint32 is a plausible-looking 2.
		std::uint64_t tryEnd = 0;
		if (!utils::btgt::region(t.startAddr, t.insnCount, code.insns.size(), tryEnd)) continue;

		BcExceptionHandler eh;
		eh.startOffset = t.startAddr;
		eh.endOffset = static_cast<uint32_t>(tryEnd);

		// Find the handler block id. We look up by the handler addr.
		// Blocks were labeled "L<offset>".
		for (const auto& handler: code.handlers.handlers[ti])
		{
			uint32_t handlerBlock = 0;
			// Search for block by label
			for (const auto& blk: cfg.blocks())
			{
				if (blk.label == "L" + std::to_string(handler.addr))
				{
					handlerBlock = blk.id;
					break;
				}
			}
			if (handler.typeIdx >= 0 && static_cast<uint32_t>(handler.typeIdx) < dex_.typeCount())
			{
				BcRefType ref;
				ref.kind = BcRefKind::Class;
				ref.className = dex_.typeName(static_cast<uint32_t>(handler.typeIdx));
				eh.catchType = BcType{ref};
			}
			eh.handlerBlock = handlerBlock;
			cfg.addExceptionHandler(eh);
		}

		if (ti < code.handlers.catchAllAddrs.size()
				&& code.handlers.catchAllAddrs[ti] != ~0u)
		{
			uint32_t catchAllBlock = 0;
			uint32_t addr = code.handlers.catchAllAddrs[ti];
			for (const auto& blk: cfg.blocks())
			{
				if (blk.label == "L" + std::to_string(addr))
				{
					catchAllBlock = blk.id;
					break;
				}
			}
			BcExceptionHandler catchAll;
			catchAll.startOffset = t.startAddr;
			catchAll.endOffset = static_cast<uint32_t>(tryEnd);
			catchAll.catchType = std::nullopt; // empty = catch-all
			catchAll.handlerBlock = catchAllBlock;
			cfg.addExceptionHandler(catchAll);
		}
	}
}

// ─── Instruction decode ───────────────────────────────────────────────────────

uint32_t DexLifter::decodeInsn(BcBasicBlock& blk, const std::vector<uint16_t>& insns, uint32_t off, const DexFile& dex)
{
	uint16_t w0 = insns[off];
	uint8_t op = w0 & 0xFF;
	uint32_t sz = kInsnSize[op];
	if (sz == 0) sz = 1;

	BcInstruction insn;
	// Method-global, not block-local. BcInstruction::id is the key every
	// method-wide per-instruction map uses, so blk.instrs.size() gave each
	// block's first instruction the same id 0 and collapsed those maps to one
	// entry per block ordinal. jvm_lifter's buildBlocks() counts the same way.
	insn.id = nextInstrId_++;
	insn.offset = off * 2u; // byte offset

	auto w = [&](uint32_t i) -> uint16_t {
		// Same guard as findLeaders' `unit`, through the same verified helper:
		// `off + i` is 32-bit here and would wrap for an offset near the end of
		// the range, so the question is asked in size_t instead.
		return utils::bounds::rangeFits(off, insns.size(), static_cast<size_t>(i) + 1) ? insns[off + i] : 0;
	};

	// Lambda to build invocation operands (method ref + arg registers)
	auto makeInvoke = [&](BcOpcode opc, uint16_t methIdx, const std::vector<uint32_t>& args) {
		insn.opcode = opc;
		// Built once per method, not once per call site -- see methodRefCache_.
		auto it = methodRefCache_.find(methIdx);
		if (it == methodRefCache_.end())
		{
			it = methodRefCache_
					 .emplace(
						 methIdx,
						 makeMethodRef(dex.methodClass(methIdx), dex.methodName(methIdx), dex.methodProto(methIdx)))
					 .first;
		}
		insn.operands.push_back(it->second);
		for (uint32_t r: args)
			insn.operands.push_back(makeReg(r));
	};

	// 35c format: `A|G|op BBBB F|E|D|C`, argument list {vC,vD,vE,vF,vG}.
	//
	// C, D, E and F are the four nibbles of the THIRD code unit, low to high;
	// G is the high nibble of the FIRST. G is the FIFTH argument, not the
	// first. This read G as vC and then shifted every real register one slot
	// later, so the list handed to makeInvoke was [G, C, D, E, F] truncated to
	// `count`: the receiver was wrong and the last argument was dropped.
	// Measured, before:
	//
	//   invoke-virtual {v1, v2}   ->  v0 v1
	//   invoke-direct  {v0, v1}   ->  v0 v0    (the new-instance/<init> idiom)
	//   invoke-virtual {v3}       ->  v0
	//   invoke-virtual {v1,..,v5} ->  v5 v1 v2 v3 v4
	//
	// Only a zero-argument call and one whose registers are all zero came out
	// right, which is what the two existing invoke tests happened to use. This
	// is invoke-virtual/super/direct/static/interface, invoke-polymorphic,
	// invoke-custom and filled-new-array -- essentially every call in compiled
	// Android code.
	auto args35c = [&]() -> std::vector<uint32_t> {
		const uint8_t count = (w(0) >> 12) & 0xF;
		const uint8_t vC = (w(2) >> 0) & 0xF;
		const uint8_t vD = (w(2) >> 4) & 0xF;
		const uint8_t vE = (w(2) >> 8) & 0xF;
		const uint8_t vF = (w(2) >> 12) & 0xF;
		const uint8_t vG = (w(0) >> 8) & 0xF;

		std::vector<uint32_t> args;
		if (count >= 1) args.push_back(vC);
		if (count >= 2) args.push_back(vD);
		if (count >= 3) args.push_back(vE);
		if (count >= 4) args.push_back(vF);
		if (count >= 5) args.push_back(vG);
		return args;
	};

	// 3rc format: {vCCCC .. vNNNN}, method@BBBB
	auto args3rc = [&]() -> std::vector<uint32_t> {
		uint8_t count = (w(0) >> 8) & 0xFF;
		uint16_t first = w(2);
		std::vector<uint32_t> args;
		for (uint32_t i = 0; i < count; ++i)
			args.push_back(first + i);
		return args;
	};

	uint16_t vA, vB, vC;
	int32_t litC;

	switch (op)
	{
	// ── NOP / payload ────────────────────────────────────────────────────
	case OP_NOP: insn.opcode = BcOpcode::DALVIK_NOP; break;

	// ── MOVE ─────────────────────────────────────────────────────────────
	case OP_MOVE:
	case OP_MOVE_OBJ:
		insn.opcode = BcOpcode::DALVIK_MOVE;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_MOVE_FROM16:
	case OP_MOVE_OBJ16:
		insn.opcode = BcOpcode::DALVIK_MOVE;
		insn.operands = {makeReg((w0 >> 8) & 0xFF), makeReg(w(1))};
		break;
	case OP_MOVE_16:
	case OP_MOVE_OBJ_16:
		insn.opcode = BcOpcode::DALVIK_MOVE;
		insn.operands = {makeReg(w(1)), makeReg(w(2))};
		sz = 3;
		break;
	case OP_MOVE_WIDE:
		insn.opcode = BcOpcode::DALVIK_MOVE_WIDE;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_MOVE_WIDE16:
		insn.opcode = BcOpcode::DALVIK_MOVE_WIDE;
		insn.operands = {makeReg((w0 >> 8) & 0xFF), makeReg(w(1))};
		break;
	case OP_MOVE_WIDE_16:
		insn.opcode = BcOpcode::DALVIK_MOVE_WIDE;
		insn.operands = {makeReg(w(1)), makeReg(w(2))};
		sz = 3;
		break;

	// ── MOVE_RESULT ───────────────────────────────────────────────────────
	case OP_MOVE_RESULT:
	case OP_MOVE_RES_WIDE:
	case OP_MOVE_RES_OBJ:
		insn.opcode = BcOpcode::DALVIK_MOVE_RESULT;
		insn.operands = {makeReg((w0 >> 8) & 0xFF)};
		break;
	case OP_MOVE_EXCEPTION:
		insn.opcode = BcOpcode::DALVIK_MOVE_EXCEPTION;
		insn.operands = {makeReg((w0 >> 8) & 0xFF)};
		break;

	// ── RETURN ────────────────────────────────────────────────────────────
	case OP_RETURN_VOID: insn.opcode = BcOpcode::DALVIK_RETURN_VOID; break;
	case OP_RETURN:
	case OP_RETURN_OBJ:
		insn.opcode = BcOpcode::DALVIK_RETURN;
		insn.operands = {makeReg((w0 >> 8) & 0xFF)};
		break;
	case OP_RETURN_WIDE:
		insn.opcode = BcOpcode::DALVIK_RETURN_WIDE;
		insn.operands = {makeReg((w0 >> 8) & 0xFF)};
		break;

	// ── CONST ─────────────────────────────────────────────────────────────
	case OP_CONST_4:
		insn.opcode = BcOpcode::DALVIK_CONST;
		vA = highA(w0);
		vB = highB(w0);
		// const/4 carries a 4-bit signed literal in the B nibble, so vB is
		// 0..15 and the value it denotes is -8..7.
		insn.operands = {makeReg(vA), makeInt(sext(vB, 4))};
		break;
	case OP_CONST_16:
		insn.opcode = BcOpcode::DALVIK_CONST;
		insn.operands = {makeReg((w0 >> 8) & 0xFF), makeInt(sext(w(1), 16))};
		break;
	case OP_CONST:
		insn.opcode = BcOpcode::DALVIK_CONST;
		insn.operands = {
			makeReg((w0 >> 8) & 0xFF),
			makeInt(sext(static_cast<uint32_t>(w(1)) | (static_cast<uint32_t>(w(2)) << 16), 32))};
		sz = 3;
		break;
	case OP_CONST_HIGH16:
		insn.opcode = BcOpcode::DALVIK_CONST;
		insn.operands = {makeReg((w0 >> 8) & 0xFF), makeInt(sext(static_cast<uint32_t>(w(1)) << 16, 32))};
		break;
	case OP_CONST_WIDE_16:
		insn.opcode = BcOpcode::DALVIK_CONST_WIDE;
		insn.operands = {makeReg((w0 >> 8) & 0xFF), makeInt(sext(w(1), 16))};
		break;
	case OP_CONST_WIDE_32:
		insn.opcode = BcOpcode::DALVIK_CONST_WIDE;
		insn.operands = {
			makeReg((w0 >> 8) & 0xFF),
			makeInt(sext(static_cast<uint32_t>(w(1)) | (static_cast<uint32_t>(w(2)) << 16), 32))};
		sz = 3;
		break;
	case OP_CONST_WIDE:
		insn.opcode = BcOpcode::DALVIK_CONST_WIDE;
		insn.operands = {
			makeReg((w0 >> 8) & 0xFF),
			makeInt(sext(
				static_cast<uint64_t>(w(1)) | (static_cast<uint64_t>(w(2)) << 16) | (static_cast<uint64_t>(w(3)) << 32)
					| (static_cast<uint64_t>(w(4)) << 48),
				64))};
		sz = 5;
		break;
	case OP_CONST_WIDE_H16:
		insn.opcode = BcOpcode::DALVIK_CONST_WIDE;
		insn.operands = {makeReg((w0 >> 8) & 0xFF), makeInt(sext(static_cast<uint64_t>(w(1)) << 48, 64))};
		break;

	// ── CONST_STRING ──────────────────────────────────────────────────────
	case OP_CONST_STR:
		insn.opcode = BcOpcode::DALVIK_CONST_STRING;
		if (opts_.resolveStrings && w(1) < dex.stringCount())
			insn.operands = {makeReg((w0 >> 8) & 0xFF), makeStr(dex.string(w(1)))};
		else
			insn.operands = {makeReg((w0 >> 8) & 0xFF), makeInt(w(1))};
		break;
	case OP_CONST_STR_J:
		insn.opcode = BcOpcode::DALVIK_CONST_STRING;
		{
			uint32_t idx = static_cast<uint32_t>(w(1)) | (static_cast<uint32_t>(w(2)) << 16);
			if (opts_.resolveStrings && idx < dex.stringCount())
				insn.operands = {makeReg((w0 >> 8) & 0xFF), makeStr(dex.string(idx))};
			else
				insn.operands = {makeReg((w0 >> 8) & 0xFF), makeInt(idx)};
			sz = 3;
		}
		break;
	case OP_CONST_CLASS:
		insn.opcode = BcOpcode::DALVIK_CONST_CLASS;
		if (w(1) < dex.typeCount())
			insn.operands = {makeReg((w0 >> 8) & 0xFF), makeTypeRef(dex.typeName(w(1)))};
		else
			insn.operands = {makeReg((w0 >> 8) & 0xFF), makeInt(w(1))};
		break;

	// ── MONITOR / CAST / CHECK ────────────────────────────────────────────
	case OP_MONITOR_ENTER:
		insn.opcode = BcOpcode::DALVIK_MONITOR_ENTER;
		insn.operands = {makeReg((w0 >> 8) & 0xFF)};
		break;
	case OP_MONITOR_EXIT:
		insn.opcode = BcOpcode::DALVIK_MONITOR_EXIT;
		insn.operands = {makeReg((w0 >> 8) & 0xFF)};
		break;
	case OP_CHECK_CAST:
		insn.opcode = BcOpcode::DALVIK_CHECK_CAST;
		insn.operands = {makeReg((w0 >> 8) & 0xFF), makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "")};
		break;
	case OP_INSTANCE_OF:
		insn.opcode = BcOpcode::DALVIK_INSTANCE_OF;
		insn.operands = {
			makeReg(highA(w0)), makeReg(highB(w0)), makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "")};
		break;
	case OP_ARRAY_LEN:
		insn.opcode = BcOpcode::DALVIK_ARRAY_LENGTH;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;

	// ── NEW ───────────────────────────────────────────────────────────────
	case OP_NEW_INSTANCE:
		insn.opcode = BcOpcode::DALVIK_NEW_INSTANCE;
		insn.operands = {makeReg((w0 >> 8) & 0xFF), makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "")};
		break;
	case OP_NEW_ARRAY:
		insn.opcode = BcOpcode::DALVIK_NEW_ARRAY;
		insn.operands = {
			makeReg(highA(w0)), makeReg(highB(w0)), makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "")};
		break;
	case OP_FILLED_NEW_ARR:
		insn.opcode = BcOpcode::DALVIK_FILLED_NEW_ARRAY;
		insn.operands = {makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "")};
		for (uint32_t r: args35c())
			insn.operands.push_back(makeReg(r));
		break;
	case OP_FILLED_NEW_RANGE:
		insn.opcode = BcOpcode::DALVIK_FILLED_NEW_ARRAY;
		insn.operands = {makeTypeRef(w(1) < dex.typeCount() ? dex.typeName(w(1)) : "")};
		for (uint32_t r: args3rc())
			insn.operands.push_back(makeReg(r));
		break;
	case OP_FILL_ARRAY_DATA:
		insn.opcode = BcOpcode::DALVIK_FILL_ARRAY_DATA;
		insn.operands = {
			makeReg((w0 >> 8) & 0xFF),
			makeInt(sext(static_cast<uint32_t>(w(1)) | (static_cast<uint32_t>(w(2)) << 16), 32))};
		sz = 3;
		break;

	// ── THROW / GOTO ──────────────────────────────────────────────────────
	case OP_THROW:
		insn.opcode = BcOpcode::DALVIK_THROW;
		insn.operands = {makeReg((w0 >> 8) & 0xFF)};
		break;
	case OP_GOTO:
		insn.opcode = BcOpcode::DALVIK_GOTO;
		{
			const int64_t offset = sext((w0 >> 8) & 0xFF, 8);
			// kNoBranchTarget when the displacement leaves the method; see
			// branchTarget(). The sum used to be formed in uint32 and handed
			// straight to makeBlock, so a negative displacement wrapped to
			// roughly four billion and became a real CFG edge.
			const auto target = branchTarget(off, offset, insns.size());
			insn.operands = {makeBlock(target ? *target : kNoBranchTarget)};
		}
		break;
	case OP_GOTO_16:
		insn.opcode = BcOpcode::DALVIK_GOTO;
		{
			const int64_t offset = sext(w(1), 16);
			// kNoBranchTarget when the displacement leaves the method; see
			// branchTarget(). The sum used to be formed in uint32 and handed
			// straight to makeBlock, so a negative displacement wrapped to
			// roughly four billion and became a real CFG edge.
			const auto target = branchTarget(off, offset, insns.size());
			insn.operands = {makeBlock(target ? *target : kNoBranchTarget)};
		}
		break;
	case OP_GOTO_32:
		insn.opcode = BcOpcode::DALVIK_GOTO;
		{
			const int64_t offset = sext(static_cast<uint32_t>(w(1)) | (static_cast<uint32_t>(w(2)) << 16), 32);
			// kNoBranchTarget when the displacement leaves the method; see
			// branchTarget(). The sum used to be formed in uint32 and handed
			// straight to makeBlock, so a negative displacement wrapped to
			// roughly four billion and became a real CFG edge.
			const auto target = branchTarget(off, offset, insns.size());
			insn.operands = {makeBlock(target ? *target : kNoBranchTarget)};
			sz = 3;
		}
		break;

	// ── SWITCH ────────────────────────────────────────────────────────────
	case OP_PACKED_SWITCH:
	case OP_SPARSE_SWITCH:
		insn.opcode = BcOpcode::DALVIK_SWITCH;
		insn.operands = {
			makeReg((w0 >> 8) & 0xFF),
			makeInt(sext(static_cast<uint32_t>(w(1)) | (static_cast<uint32_t>(w(2)) << 16), 32))};
		// The case targets follow the payload offset as block operands, so
		// that buildBlocks wires an edge to each of them the same way it
		// does for a goto or an if.
		for (uint32_t target: switchTargets(insns, off))
			insn.operands.push_back(makeBlock(target));
		sz = 3;
		break;

	// ── CMP ───────────────────────────────────────────────────────────────
	case OP_CMPL_FLOAT:
	case OP_CMPG_FLOAT:
	case OP_CMPL_DOUBLE:
	case OP_CMPG_DOUBLE:
	case OP_CMP_LONG:
		insn.opcode = BcOpcode::DALVIK_CMP;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC), makeInt(op)};
		break;

	// ── IF ────────────────────────────────────────────────────────────────
	case OP_IF_EQ:
	case OP_IF_NE:
	case OP_IF_LT:
	case OP_IF_GE:
	case OP_IF_GT:
	case OP_IF_LE: {
		insn.opcode = BcOpcode::DALVIK_IF;
		vA = highA(w0);
		vB = highB(w0);
		const int64_t offset = sext(w(1), 16);
		// kNoBranchTarget when the displacement leaves the method; see
		// branchTarget(). The sum used to be formed in uint32 and handed
		// straight to makeBlock, so a negative displacement wrapped to
		// roughly four billion and became a real CFG edge.
		const auto target = branchTarget(off, offset, insns.size());
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(op), makeBlock(target ? *target : kNoBranchTarget)};
		break;
	}
	case OP_IF_EQZ:
	case OP_IF_NEZ:
	case OP_IF_LTZ:
	case OP_IF_GEZ:
	case OP_IF_GTZ:
	case OP_IF_LEZ: {
		insn.opcode = BcOpcode::DALVIK_IF_Z;
		vA = (w0 >> 8) & 0xFF;
		const int64_t offset = sext(w(1), 16);
		// kNoBranchTarget when the displacement leaves the method; see
		// branchTarget(). The sum used to be formed in uint32 and handed
		// straight to makeBlock, so a negative displacement wrapped to
		// roughly four billion and became a real CFG edge.
		const auto target = branchTarget(off, offset, insns.size());
		insn.operands = {makeReg(vA), makeInt(op), makeBlock(target ? *target : kNoBranchTarget)};
		break;
	}

	// ── ARRAY OPS ─────────────────────────────────────────────────────────
	case OP_AGET:
	case OP_AGET_WIDE:
	case OP_AGET_OBJ:
	case OP_AGET_BOOL:
	case OP_AGET_BYTE:
	case OP_AGET_CHAR:
	case OP_AGET_SHORT:
		insn.opcode = BcOpcode::DALVIK_AGET;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_APUT:
	case OP_APUT_WIDE:
	case OP_APUT_OBJ:
	case OP_APUT_BOOL:
	case OP_APUT_BYTE:
	case OP_APUT_CHAR:
	case OP_APUT_SHORT:
		insn.opcode = BcOpcode::DALVIK_APUT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;

	// ── IGET / IPUT ───────────────────────────────────────────────────────
	case OP_IGET:
	case OP_IGET_WIDE:
	case OP_IGET_OBJ:
	case OP_IGET_BOOL:
	case OP_IGET_BYTE:
	case OP_IGET_CHAR:
	case OP_IGET_SHORT:
		insn.opcode = BcOpcode::DALVIK_IGET;
		vA = highA(w0);
		vB = highB(w0);
		if (w(1) < dex.fieldCount())
			insn.operands = {
				makeReg(vA), makeReg(vB), makeFieldRef(dex.fieldClass(w(1)), dex.fieldName(w(1)), dex.fieldType(w(1)))};
		else
			insn.operands = {makeReg(vA), makeReg(vB), makeInt(w(1))};
		break;
	case OP_IPUT:
	case OP_IPUT_WIDE:
	case OP_IPUT_OBJ:
	case OP_IPUT_BOOL:
	case OP_IPUT_BYTE:
	case OP_IPUT_CHAR:
	case OP_IPUT_SHORT:
		insn.opcode = BcOpcode::DALVIK_IPUT;
		vA = highA(w0);
		vB = highB(w0);
		if (w(1) < dex.fieldCount())
			insn.operands = {
				makeReg(vA), makeReg(vB), makeFieldRef(dex.fieldClass(w(1)), dex.fieldName(w(1)), dex.fieldType(w(1)))};
		else
			insn.operands = {makeReg(vA), makeReg(vB), makeInt(w(1))};
		break;

	// ── SGET / SPUT ───────────────────────────────────────────────────────
	case OP_SGET:
	case OP_SGET_WIDE:
	case OP_SGET_OBJ:
	case OP_SGET_BOOL:
	case OP_SGET_BYTE:
	case OP_SGET_CHAR:
	case OP_SGET_SHORT:
		insn.opcode = BcOpcode::DALVIK_SGET;
		if (w(1) < dex.fieldCount())
			insn.operands = {
				makeReg((w0 >> 8) & 0xFF),
				makeFieldRef(dex.fieldClass(w(1)), dex.fieldName(w(1)), dex.fieldType(w(1)))};
		else
			insn.operands = {makeReg((w0 >> 8) & 0xFF), makeInt(w(1))};
		break;
	case OP_SPUT:
	case OP_SPUT_WIDE:
	case OP_SPUT_OBJ:
	case OP_SPUT_BOOL:
	case OP_SPUT_BYTE:
	case OP_SPUT_CHAR:
	case OP_SPUT_SHORT:
		insn.opcode = BcOpcode::DALVIK_SPUT;
		if (w(1) < dex.fieldCount())
			insn.operands = {
				makeReg((w0 >> 8) & 0xFF),
				makeFieldRef(dex.fieldClass(w(1)), dex.fieldName(w(1)), dex.fieldType(w(1)))};
		else
			insn.operands = {makeReg((w0 >> 8) & 0xFF), makeInt(w(1))};
		break;

	// ── INVOKE ────────────────────────────────────────────────────────────
	case OP_INVOKE_VIRTUAL: makeInvoke(BcOpcode::DALVIK_INVOKE_VIRTUAL, w(1), args35c()); break;
	case OP_INVOKE_SUPER: makeInvoke(BcOpcode::DALVIK_INVOKE_SUPER, w(1), args35c()); break;
	case OP_INVOKE_DIRECT: makeInvoke(BcOpcode::DALVIK_INVOKE_DIRECT, w(1), args35c()); break;
	case OP_INVOKE_STATIC: makeInvoke(BcOpcode::DALVIK_INVOKE_STATIC, w(1), args35c()); break;
	case OP_INVOKE_INTERFACE: makeInvoke(BcOpcode::DALVIK_INVOKE_INTERFACE, w(1), args35c()); break;
	case OP_INVOKE_VIRT_RANGE: makeInvoke(BcOpcode::DALVIK_INVOKE_VIRTUAL, w(1), args3rc()); break;
	case OP_INVOKE_SUPER_RANGE: makeInvoke(BcOpcode::DALVIK_INVOKE_SUPER, w(1), args3rc()); break;
	case OP_INVOKE_DIRECT_RANGE: makeInvoke(BcOpcode::DALVIK_INVOKE_DIRECT, w(1), args3rc()); break;
	case OP_INVOKE_STATIC_RANGE: makeInvoke(BcOpcode::DALVIK_INVOKE_STATIC, w(1), args3rc()); break;
	case OP_INVOKE_IFACE_RANGE: makeInvoke(BcOpcode::DALVIK_INVOKE_INTERFACE, w(1), args3rc()); break;
	case OP_INVOKE_POLYMORPHIC: makeInvoke(BcOpcode::DALVIK_INVOKE_VIRTUAL, w(1), args35c()); break;
	case OP_INVOKE_POLYMORPHIC_RANGE: makeInvoke(BcOpcode::DALVIK_INVOKE_VIRTUAL, w(1), args3rc()); break;
	case OP_INVOKE_CUSTOM:
		insn.opcode = BcOpcode::DALVIK_INVOKE_CUSTOM;
		insn.operands = {makeInt(w(1))};
		for (uint32_t r: args35c())
			insn.operands.push_back(makeReg(r));
		break;
	case OP_INVOKE_CUSTOM_RANGE:
		insn.opcode = BcOpcode::DALVIK_INVOKE_CUSTOM;
		insn.operands = {makeInt(w(1))};
		for (uint32_t r: args3rc())
			insn.operands.push_back(makeReg(r));
		break;

	// ── UNARY OPS ─────────────────────────────────────────────────────────
	case OP_NEG_INT:
		insn.opcode = BcOpcode::DALVIK_NEG_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_NOT_INT:
		insn.opcode = BcOpcode::DALVIK_NOT_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_NEG_LONG:
		insn.opcode = BcOpcode::DALVIK_NEG_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_NOT_LONG:
		insn.opcode = BcOpcode::DALVIK_NOT_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_NEG_FLOAT:
		insn.opcode = BcOpcode::DALVIK_NEG_FLOAT;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_NEG_DOUBLE:
		insn.opcode = BcOpcode::DALVIK_NEG_DOUBLE;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_INT_TO_LONG:
		insn.opcode = BcOpcode::DALVIK_INT_TO_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_INT_TO_FLOAT:
		insn.opcode = BcOpcode::DALVIK_INT_TO_FLOAT;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_INT_TO_DOUBLE:
		insn.opcode = BcOpcode::DALVIK_INT_TO_DOUBLE;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_LONG_TO_INT:
		insn.opcode = BcOpcode::DALVIK_LONG_TO_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_LONG_TO_FLOAT:
		insn.opcode = BcOpcode::DALVIK_LONG_TO_FLOAT;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_LONG_TO_DOUBLE:
		insn.opcode = BcOpcode::DALVIK_LONG_TO_DOUBLE;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_FLOAT_TO_INT:
		insn.opcode = BcOpcode::DALVIK_FLOAT_TO_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_FLOAT_TO_LONG:
		insn.opcode = BcOpcode::DALVIK_FLOAT_TO_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_FLOAT_TO_DOUBLE:
		insn.opcode = BcOpcode::DALVIK_FLOAT_TO_DOUBLE;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_DOUBLE_TO_INT:
		insn.opcode = BcOpcode::DALVIK_DOUBLE_TO_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_DOUBLE_TO_LONG:
		insn.opcode = BcOpcode::DALVIK_DOUBLE_TO_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_DOUBLE_TO_FLOAT:
		insn.opcode = BcOpcode::DALVIK_DOUBLE_TO_FLOAT;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_INT_TO_BYTE:
		insn.opcode = BcOpcode::DALVIK_INT_TO_BYTE;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_INT_TO_CHAR:
		insn.opcode = BcOpcode::DALVIK_INT_TO_CHAR;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_INT_TO_SHORT:
		insn.opcode = BcOpcode::DALVIK_INT_TO_SHORT;
		insn.operands = {makeReg(highA(w0)), makeReg(highB(w0))};
		break;

	// ── BINARY OPS (reg/reg form 23x) ─────────────────────────────────────
	case OP_ADD_INT:
		insn.opcode = BcOpcode::DALVIK_ADD_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_SUB_INT:
		insn.opcode = BcOpcode::DALVIK_SUB_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_MUL_INT:
		insn.opcode = BcOpcode::DALVIK_MUL_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_DIV_INT:
		insn.opcode = BcOpcode::DALVIK_DIV_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_REM_INT:
		insn.opcode = BcOpcode::DALVIK_REM_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_AND_INT:
		insn.opcode = BcOpcode::DALVIK_AND_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_OR_INT:
		insn.opcode = BcOpcode::DALVIK_OR_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_XOR_INT:
		insn.opcode = BcOpcode::DALVIK_XOR_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_SHL_INT:
		insn.opcode = BcOpcode::DALVIK_SHL_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_SHR_INT:
		insn.opcode = BcOpcode::DALVIK_SHR_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_USHR_INT:
		insn.opcode = BcOpcode::DALVIK_USHR_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;

	case OP_ADD_LONG:
		insn.opcode = BcOpcode::DALVIK_ADD_LONG;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_SUB_LONG:
		insn.opcode = BcOpcode::DALVIK_SUB_LONG;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_MUL_LONG:
		insn.opcode = BcOpcode::DALVIK_MUL_LONG;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_DIV_LONG:
		insn.opcode = BcOpcode::DALVIK_DIV_LONG;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_REM_LONG:
		insn.opcode = BcOpcode::DALVIK_REM_LONG;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_AND_LONG:
		insn.opcode = BcOpcode::DALVIK_AND_LONG;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_OR_LONG:
		insn.opcode = BcOpcode::DALVIK_OR_LONG;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_XOR_LONG:
		insn.opcode = BcOpcode::DALVIK_XOR_LONG;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_SHL_LONG:
		insn.opcode = BcOpcode::DALVIK_SHL_LONG;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_SHR_LONG:
		insn.opcode = BcOpcode::DALVIK_SHR_LONG;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_USHR_LONG:
		insn.opcode = BcOpcode::DALVIK_USHR_LONG;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;

	case OP_ADD_FLOAT:
		insn.opcode = BcOpcode::DALVIK_ADD_FLOAT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_SUB_FLOAT:
		insn.opcode = BcOpcode::DALVIK_SUB_FLOAT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_MUL_FLOAT:
		insn.opcode = BcOpcode::DALVIK_MUL_FLOAT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_DIV_FLOAT:
		insn.opcode = BcOpcode::DALVIK_DIV_FLOAT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_REM_FLOAT:
		insn.opcode = BcOpcode::DALVIK_REM_FLOAT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_ADD_DOUBLE:
		insn.opcode = BcOpcode::DALVIK_ADD_DOUBLE;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_SUB_DOUBLE:
		insn.opcode = BcOpcode::DALVIK_SUB_DOUBLE;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_MUL_DOUBLE:
		insn.opcode = BcOpcode::DALVIK_MUL_DOUBLE;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_DIV_DOUBLE:
		insn.opcode = BcOpcode::DALVIK_DIV_DOUBLE;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;
	case OP_REM_DOUBLE:
		insn.opcode = BcOpcode::DALVIK_REM_DOUBLE;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		vC = (w(1) >> 8) & 0xFF;
		insn.operands = {makeReg(vA), makeReg(vB), makeReg(vC)};
		break;

	// ── 2ADDR forms (12x): vA op= vB ──────────────────────────────────────
	case OP_ADD_INT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_ADD_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_SUB_INT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_SUB_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_MUL_INT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_MUL_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_DIV_INT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_DIV_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_REM_INT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_REM_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_AND_INT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_AND_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_OR_INT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_OR_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_XOR_INT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_XOR_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_SHL_INT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_SHL_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_SHR_INT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_SHR_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_USHR_INT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_USHR_INT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_ADD_LONG_2ADDR:
		insn.opcode = BcOpcode::DALVIK_ADD_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_SUB_LONG_2ADDR:
		insn.opcode = BcOpcode::DALVIK_SUB_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_MUL_LONG_2ADDR:
		insn.opcode = BcOpcode::DALVIK_MUL_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_DIV_LONG_2ADDR:
		insn.opcode = BcOpcode::DALVIK_DIV_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_REM_LONG_2ADDR:
		insn.opcode = BcOpcode::DALVIK_REM_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_AND_LONG_2ADDR:
		insn.opcode = BcOpcode::DALVIK_AND_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_OR_LONG_2ADDR:
		insn.opcode = BcOpcode::DALVIK_OR_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_XOR_LONG_2ADDR:
		insn.opcode = BcOpcode::DALVIK_XOR_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_SHL_LONG_2ADDR:
		insn.opcode = BcOpcode::DALVIK_SHL_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_SHR_LONG_2ADDR:
		insn.opcode = BcOpcode::DALVIK_SHR_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_USHR_LONG_2ADDR:
		insn.opcode = BcOpcode::DALVIK_USHR_LONG;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_ADD_FLOAT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_ADD_FLOAT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_SUB_FLOAT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_SUB_FLOAT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_MUL_FLOAT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_MUL_FLOAT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_DIV_FLOAT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_DIV_FLOAT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_REM_FLOAT_2ADDR:
		insn.opcode = BcOpcode::DALVIK_REM_FLOAT;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_ADD_DBL_2ADDR:
		insn.opcode = BcOpcode::DALVIK_ADD_DOUBLE;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_SUB_DBL_2ADDR:
		insn.opcode = BcOpcode::DALVIK_SUB_DOUBLE;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_MUL_DBL_2ADDR:
		insn.opcode = BcOpcode::DALVIK_MUL_DOUBLE;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_DIV_DBL_2ADDR:
		insn.opcode = BcOpcode::DALVIK_DIV_DOUBLE;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;
	case OP_REM_DBL_2ADDR:
		insn.opcode = BcOpcode::DALVIK_REM_DOUBLE;
		insn.operands = {makeReg(highA(w0)), makeReg(highA(w0)), makeReg(highB(w0))};
		break;

	// ── LIT16 forms (22s): vA = vB op lit ─────────────────────────────────
	case OP_ADD_INT_LIT16:
		insn.opcode = BcOpcode::DALVIK_ADD_INT;
		vA = highA(w0);
		vB = highB(w0);
		litC = static_cast<int32_t>(sext(w(1), 16));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_RSUB_INT:
		insn.opcode = BcOpcode::DALVIK_RSUB_INT;
		vA = highA(w0);
		vB = highB(w0);
		litC = static_cast<int32_t>(sext(w(1), 16));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_MUL_INT_LIT16:
		insn.opcode = BcOpcode::DALVIK_MUL_INT;
		vA = highA(w0);
		vB = highB(w0);
		litC = static_cast<int32_t>(sext(w(1), 16));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_DIV_INT_LIT16:
		insn.opcode = BcOpcode::DALVIK_DIV_INT;
		vA = highA(w0);
		vB = highB(w0);
		litC = static_cast<int32_t>(sext(w(1), 16));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_REM_INT_LIT16:
		insn.opcode = BcOpcode::DALVIK_REM_INT;
		vA = highA(w0);
		vB = highB(w0);
		litC = static_cast<int32_t>(sext(w(1), 16));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_AND_INT_LIT16:
		insn.opcode = BcOpcode::DALVIK_AND_INT;
		vA = highA(w0);
		vB = highB(w0);
		litC = static_cast<int32_t>(sext(w(1), 16));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_OR_INT_LIT16:
		insn.opcode = BcOpcode::DALVIK_OR_INT;
		vA = highA(w0);
		vB = highB(w0);
		litC = static_cast<int32_t>(sext(w(1), 16));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_XOR_INT_LIT16:
		insn.opcode = BcOpcode::DALVIK_XOR_INT;
		vA = highA(w0);
		vB = highB(w0);
		litC = static_cast<int32_t>(sext(w(1), 16));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;

	// ── LIT8 forms (22b): vAA = vBB op lit ────────────────────────────────
	case OP_ADD_INT_LIT8:
		insn.opcode = BcOpcode::DALVIK_ADD_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		litC = static_cast<int32_t>(sext((w(1) >> 8) & 0xFF, 8));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_RSUB_INT_LIT8:
		insn.opcode = BcOpcode::DALVIK_RSUB_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		litC = static_cast<int32_t>(sext((w(1) >> 8) & 0xFF, 8));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_MUL_INT_LIT8:
		insn.opcode = BcOpcode::DALVIK_MUL_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		litC = static_cast<int32_t>(sext((w(1) >> 8) & 0xFF, 8));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_DIV_INT_LIT8:
		insn.opcode = BcOpcode::DALVIK_DIV_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		litC = static_cast<int32_t>(sext((w(1) >> 8) & 0xFF, 8));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_REM_INT_LIT8:
		insn.opcode = BcOpcode::DALVIK_REM_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		litC = static_cast<int32_t>(sext((w(1) >> 8) & 0xFF, 8));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_AND_INT_LIT8:
		insn.opcode = BcOpcode::DALVIK_AND_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		litC = static_cast<int32_t>(sext((w(1) >> 8) & 0xFF, 8));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_OR_INT_LIT8:
		insn.opcode = BcOpcode::DALVIK_OR_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		litC = static_cast<int32_t>(sext((w(1) >> 8) & 0xFF, 8));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_XOR_INT_LIT8:
		insn.opcode = BcOpcode::DALVIK_XOR_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		litC = static_cast<int32_t>(sext((w(1) >> 8) & 0xFF, 8));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_SHL_INT_LIT8:
		insn.opcode = BcOpcode::DALVIK_SHL_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		litC = static_cast<int32_t>(sext((w(1) >> 8) & 0xFF, 8));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_SHR_INT_LIT8:
		insn.opcode = BcOpcode::DALVIK_SHR_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		litC = static_cast<int32_t>(sext((w(1) >> 8) & 0xFF, 8));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;
	case OP_USHR_INT_LIT8:
		insn.opcode = BcOpcode::DALVIK_USHR_INT;
		vA = (w0 >> 8) & 0xFF;
		vB = w(1) & 0xFF;
		litC = static_cast<int32_t>(sext((w(1) >> 8) & 0xFF, 8));
		insn.operands = {makeReg(vA), makeReg(vB), makeInt(litC)};
		break;

	// ── CONST_METHOD_HANDLE / CONST_METHOD_TYPE (DEX 038+) ───────────────
	case OP_CONST_METHOD_HANDLE:
		insn.opcode = BcOpcode::DALVIK_CONST;
		insn.operands = {makeReg((w0 >> 8) & 0xFF), makeInt(w(1))};
		break;
	case OP_CONST_METHOD_TYPE:
		insn.opcode = BcOpcode::DALVIK_CONST;
		insn.operands = {makeReg((w0 >> 8) & 0xFF), makeInt(w(1))};
		break;

	default: insn.opcode = BcOpcode::DALVIK_NOP; break;
	}

	blk.instrs.push_back(std::move(insn));
	return sz;
}

} // namespace dex_parser
} // namespace retdec
