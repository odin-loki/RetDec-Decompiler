/**
 * @file src/debug_info/debug_info.cpp
 * @brief Implementation of DebugGroundTruth helpers and DebugLocEvaluator.
 */

#include "retdec/debug_info/debug_info.h"

#include "retdec/utils/leb128.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <sstream>

namespace retdec {
namespace debug_info {

// ─── DebugVar ─────────────────────────────────────────────────────────────────

StorageLoc DebugVar::locationAt(uint64_t pc) const noexcept
{
	for (const auto& lr: liveRanges)
	{
		if (pc >= lr.lo && pc < lr.hi) return lr.loc;
	}
	// If there is exactly one range with lo == hi == 0 it is a "static" entry
	// from a PDB symbol record with no explicit range.
	if (liveRanges.size() == 1 && liveRanges[0].lo == 0 && liveRanges[0].hi == 0)
	{
		return liveRanges[0].loc;
	}
	return StorageLoc{};
}

// ─── DebugGroundTruth ─────────────────────────────────────────────────────────

const DebugFunc* DebugGroundTruth::funcAt(uint64_t pc) const noexcept
{
	// Linear scan is acceptable; callers are not on hot paths.
	for (const auto& kv: functions)
	{
		const DebugFunc& f = kv.second;
		if (f.lowPc <= pc && pc < f.highPc) return &f;
	}
	return nullptr;
}

const DebugFunc* DebugGroundTruth::funcByName(const std::string& n) const noexcept
{
	for (const auto& kv: functions)
	{
		if (kv.second.name == n || kv.second.linkageName == n) return &kv.second;
	}
	return nullptr;
}

const DebugType* DebugGroundTruth::typeById(uint64_t id) const noexcept
{
	auto it = types.find(id);
	if (it == types.end()) return nullptr;
	return &it->second;
}

std::string DebugGroundTruth::typeName(uint64_t id) const
{
	const DebugType* t = typeById(id);
	if (!t) return "<unknown_type>";
	if (!t->name.empty()) return t->name;

	switch (t->kind)
	{
	case DebugTypeKind::Void: return "void";
	case DebugTypeKind::Pointer: {
		std::string inner = typeName(t->pointedToTypeId);
		return inner + "*";
	}
	case DebugTypeKind::Array: {
		std::string elem = typeName(t->elementTypeId);
		std::ostringstream oss;
		oss << elem << "[" << t->elementCount << "]";
		return oss.str();
	}
	case DebugTypeKind::Struct: return "struct <anon>";
	case DebugTypeKind::Union: return "union <anon>";
	case DebugTypeKind::Enum: return "enum <anon>";
	default: return "<type>";
	}
}

std::vector<const InlinedSite*> DebugGroundTruth::inlinedAt(uint64_t pc) const
{
	std::vector<const InlinedSite*> result;
	for (const auto& s: allInlined)
	{
		if (pc >= s.loAddr && pc < s.hiAddr) result.push_back(&s);
	}
	return result;
}

// ─── DebugLocEvaluator ────────────────────────────────────────────────────────

// Both readers used to be written out here with an unbounded shift: a DWARF
// expression made of bytes with the continuation bit set drives `shift` past
// the width of the accumulator, and `x << shift` with `shift >= width` is
// undefined. The signed one was undefined sooner still, because it accumulated
// into an int64_t, where the shift overflows the signed range long before the
// count becomes the problem. retdec/utils/leb128.h bounds both the shift and
// the cursor, accumulates unsigned and sign-extends at the end, and is proved
// over the whole domain by tests/verification/leb128_proof.cpp.
//
// The cursor contract is kept: p advances by the bytes consumed. A malformed
// encoding -- truncated, or longer than a 64-bit value can be -- consumes the
// rest of the expression, so a caller's loop still terminates and cannot go on
// reading from the middle of a run it did not understand.
uint64_t DebugLocEvaluator::readULEB128(const uint8_t*& p, const uint8_t* end) noexcept
{
	if (p == nullptr || end == nullptr || p >= end) return 0;
	const auto r = utils::leb128::decodeUnsigned(p, static_cast<std::size_t>(end - p), 0);
	if (!r.ok)
	{
		p = end;
		return 0;
	}
	p += r.bytesRead;
	return r.value;
}

int64_t DebugLocEvaluator::readSLEB128(const uint8_t*& p, const uint8_t* end) noexcept
{
	if (p == nullptr || end == nullptr || p >= end) return 0;
	const auto r = utils::leb128::decodeSigned(p, static_cast<std::size_t>(end - p), 0);
	if (!r.ok)
	{
		p = end;
		return 0;
	}
	p += r.bytesRead;
	return utils::leb128::toSigned(r.value);
}

// DWARF opcode constants (subset we evaluate)
static constexpr uint8_t DW_OP_addr = 0x03;
static constexpr uint8_t DW_OP_deref = 0x06;
static constexpr uint8_t DW_OP_lit0 = 0x30;
static constexpr uint8_t DW_OP_lit31 = 0x4f;
static constexpr uint8_t DW_OP_reg0 = 0x50;
static constexpr uint8_t DW_OP_reg31 = 0x6f;
static constexpr uint8_t DW_OP_breg0 = 0x77;
static constexpr uint8_t DW_OP_breg31 = 0x90;
static constexpr uint8_t DW_OP_regx = 0x90;
static constexpr uint8_t DW_OP_fbreg = 0x91;
static constexpr uint8_t DW_OP_bregx = 0x92;
static constexpr uint8_t DW_OP_plus_uconst = 0x23;
static constexpr uint8_t DW_OP_stack_value = 0x9f;
static constexpr uint8_t DW_OP_implicit_value = 0x9e;

StorageLoc DebugLocEvaluator::evaluate(const uint8_t* expr, std::size_t len, uint8_t addrSize)
{
	if (!expr || len == 0) return StorageLoc{};

	const uint8_t* p = expr;
	const uint8_t* end = expr + len;

	// We use a tiny stack for the expression result.
	// For our purposes (single-location expressions), depth never exceeds 2.
	int64_t stackTop = 0;
	bool hasValue = false;

	while (p < end)
	{
		uint8_t op = *p++;

		// ── Register opcodes ──────────────────────────────────────────────────
		if (op >= DW_OP_reg0 && op <= DW_OP_reg31)
		{
			return StorageLoc::reg(op - DW_OP_reg0);
		}
		if (op == DW_OP_regx)
		{
			uint64_t r = readULEB128(p, end);
			return StorageLoc::reg(static_cast<uint32_t>(r));
		}

		// ── Frame-base-relative ───────────────────────────────────────────────
		if (op == DW_OP_fbreg)
		{
			int64_t off = readSLEB128(p, end);
			return StorageLoc::stack(off);
		}

		// ── Register + signed offset ──────────────────────────────────────────
		if (op >= DW_OP_breg0 && op <= DW_OP_breg31)
		{
			int64_t off = readSLEB128(p, end);
			StorageLoc s;
			s.kind = StorageKind::Register;
			s.regNum = op - DW_OP_breg0;
			s.offset = off;
			return s;
		}
		if (op == DW_OP_bregx)
		{
			uint64_t r = readULEB128(p, end);
			int64_t off = readSLEB128(p, end);
			StorageLoc s;
			s.kind = StorageKind::Register;
			s.regNum = static_cast<uint32_t>(r);
			s.offset = off;
			return s;
		}

		// ── Static address ────────────────────────────────────────────────────
		if (op == DW_OP_addr)
		{
			uint64_t addr = 0;
			std::size_t avail = static_cast<std::size_t>(end - p);
			if (avail >= addrSize)
			{
				if (addrSize == 8)
				{
					std::memcpy(&addr, p, 8);
				}
				else if (addrSize == 4)
				{
					uint32_t a32 = 0;
					std::memcpy(&a32, p, 4);
					addr = a32;
				}
				p += addrSize;
			}
			stackTop = static_cast<int64_t>(addr);
			hasValue = true;
			continue;
		}

		// ── Literals ──────────────────────────────────────────────────────────
		if (op >= DW_OP_lit0 && op <= DW_OP_lit31)
		{
			stackTop = op - DW_OP_lit0;
			hasValue = true;
			continue;
		}

		// ── plus_uconst: stackTop += ULEB ────────────────────────────────────
		if (op == DW_OP_plus_uconst)
		{
			uint64_t c = readULEB128(p, end);
			stackTop += static_cast<int64_t>(c);
			continue;
		}

		// ── DW_OP_deref: treat result as static address ───────────────────────
		if (op == DW_OP_deref)
		{
			// We can't actually dereference at extraction time;
			// treat the current stack value as a static address.
			return StorageLoc::staticAddr(static_cast<uint64_t>(stackTop));
		}

		// ── DW_OP_stack_value / DW_OP_implicit_value ─────────────────────────
		if (op == DW_OP_stack_value || op == DW_OP_implicit_value)
		{
			return StorageLoc::optimized();
		}

		// Unhandled opcode — bail out.
		break;
	}

	// If we processed some address-like value, return it as a static location.
	if (hasValue) return StorageLoc::staticAddr(static_cast<uint64_t>(stackTop));

	return StorageLoc{};
}

} // namespace debug_info
} // namespace retdec
