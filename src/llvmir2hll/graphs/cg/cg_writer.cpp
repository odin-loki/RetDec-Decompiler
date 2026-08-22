/**
* @file src/llvmir2hll/graphs/cg/cg_writer.cpp
* @brief Implementation of CGWriter.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include "retdec/llvmir2hll/graphs/cg/cg_writer.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

/**
* @brief Constructs a new writer.
*
* @param[in] cg CG to be emitted.
* @param[in] out Output stream where the CG is emitted.
*/
CGWriter::CGWriter(ShPtr<CG> cg, std::ostream &out):
	cg(cg), out(out) {}

} // namespace llvmir2hll
} // namespace retdec
