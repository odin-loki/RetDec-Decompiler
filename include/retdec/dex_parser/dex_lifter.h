/**
 * @file include/retdec/dex_parser/dex_lifter.h
 * @brief Dalvik bytecode → BcCFG lifter.
 *
 * Unlike the JVM (stack-based), Dalvik is register-based. Each instruction
 * specifies explicit source and destination registers (vN). The lifter:
 *
 *   1. Scans all instructions in the code_item to find basic-block leaders
 *      (branch targets, try-block entries, exception handler entries, first insn).
 *   2. Splits into BcBasicBlocks.
 *   3. Wires exception handlers from the try/catch table.
 *   4. Maps each Dalvik register vN to a BcLocalVar (typed once a move or
 *      const is seen; otherwise typed as Object initially).
 *   5. Translates every Dalvik opcode to BcInstruction(s) using the shared
 *      BcOpcode set (DALVIK_ prefixed opcodes in BcOpcode).
 *
 * Register operands are encoded as BcOperand::Local with id = register number.
 * Wide register pairs (vN, vN+1) are encoded as the lower-numbered register.
 */

#ifndef RETDEC_DEX_PARSER_DEX_LIFTER_H
#define RETDEC_DEX_PARSER_DEX_LIFTER_H

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_instr.h"
#include "retdec/dex_parser/dex_header.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace retdec {
namespace dex_parser {

struct LiftOptions
{
	bool emitLineNumbers = true; ///< Populate BcInstruction::line when debug info present
	bool emitAnnotations = true; ///< Wire exception handlers from try/catch table
	bool resolveStrings = true;  ///< Resolve string-const indices to BcOperand::Str
};
static LiftOptions defaultLiftOptions() noexcept
{
	return {};
}

struct DexLiftResult
{
	enum Status
	{
		OK,
		Error
	};
	Status status = OK;
	std::string error;
	bc_module::BcCFG cfg;
};

/**
 * @brief Converts a single DEX code_item to a BcCFG.
 */
class DexLifter {
public:
	DexLifter(const DexFile& dexFile, LiftOptions opts = defaultLiftOptions());

	DexLiftResult lift(const CodeItem& code, uint32_t methodIdx);

private:
	const DexFile& dex_;
	LiftOptions opts_;

	/// Method-global instruction counter, reset by buildBlocks() for each
	/// method. See decodeInsn() for why it is not per block.
	uint32_t nextInstrId_ = 0;

	using BlockId = uint32_t;

	// Pass 1: find leader addresses (code unit offsets).
	std::vector<uint32_t> findLeaders(const CodeItem& code) const;

	// Pass 2: build BcBasicBlock skeletons.
	void buildBlocks(bc_module::BcCFG& cfg, const CodeItem& code, const std::vector<uint32_t>& leaders);

	// Pass 3: wire exception handlers.
	void wireExceptions(bc_module::BcCFG& cfg, const CodeItem& code, const std::vector<uint32_t>& leaders);

	// Decode one Dalvik instruction starting at codeUnit offset `off`.
	// Returns number of code units consumed.
	uint32_t
	decodeInsn(bc_module::BcBasicBlock& blk, const std::vector<uint16_t>& insns, uint32_t off, const DexFile& dex);

	/// The operand form of a method reference, by method index.
	///
	/// Building one means DexFile::methodProto() reading the method's type list
	/// out of the file and concatenating a descriptor from it, and then
	/// parseDexProto() allocating a BcType node per parameter -- and both ran
	/// again for every invoke instruction naming the method. Both the
	/// descriptor's length and the number of invokes are file-controlled, so
	/// the work was the product of two quantities an input chooses
	/// independently.
	///
	/// To be accurate about what this fixed: the out-of-memory the fuzzer found
	/// here is closed by the parameter bound in parseDexProto, not by this --
	/// removing the cache and keeping the bound, the fuzzer no longer reaches
	/// it. What the cache removes is the repeated work, and it makes the
	/// per-call-site cost a vector of pointers to shared nodes rather than a
	/// fresh parse and a fresh node per parameter. One entry per method index,
	/// so it is bounded by the file's own method_ids count.
	std::unordered_map<uint32_t, bc_module::BcOperand> methodRefCache_;
};

} // namespace dex_parser
} // namespace retdec

#endif // RETDEC_DEX_PARSER_DEX_LIFTER_H
