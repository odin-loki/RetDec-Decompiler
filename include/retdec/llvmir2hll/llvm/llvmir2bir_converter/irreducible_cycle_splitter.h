/**
* @file include/retdec/llvmir2hll/llvm/llvmir2bir_converter/irreducible_cycle_splitter.h
* @brief Makes small irreducible cycles reducible by duplicating entry blocks.
* @copyright (c) 2026 Avast Software, licensed under the MIT license
*/

#ifndef RETDEC_LLVMIR2HLL_LLVM_LLVMIR2BIR_CONVERTER_IRREDUCIBLE_CYCLE_SPLITTER_H
#define RETDEC_LLVMIR2HLL_LLVM_LLVMIR2BIR_CONVERTER_IRREDUCIBLE_CYCLE_SPLITTER_H

namespace llvm {

class Function;

} // namespace llvm

namespace retdec {
namespace llvmir2hll {

/**
* @brief Makes irreducible cycles (cycles with more than one entry block) of
*        @a func reducible by node splitting.
*
* For each such cycle one entry is kept as the loop header. Every other entry
* is reached from outside the cycle through a copy of the blocks between it
* and the header, so the header becomes the only entry and LoopInfo sees a
* natural loop. A cycle is split only when those blocks hold at most
* @a maxInstsPerCycle instructions and form no cycle of their own, and the
* whole function grows by at most @a maxInstsPerFunc instructions.
*
* @return The number of instructions added.
*/
unsigned splitIrreducibleCycles(llvm::Function &func,
	unsigned maxInstsPerCycle, unsigned maxInstsPerFunc);

} // namespace llvmir2hll
} // namespace retdec

#endif
