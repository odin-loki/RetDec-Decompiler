/**
 * @file include/retdec/capstone2llvmir/powerpc/powerpc.h
 * @brief PowerPC specialization of translator's abstract public interface.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_CAPSTONE2LLVMIR_POWERPC_POWERPC_H
#define RETDEC_CAPSTONE2LLVMIR_POWERPC_POWERPC_H

#include "retdec/capstone2llvmir/capstone2llvmir.h"
#include "retdec/capstone2llvmir/powerpc/powerpc_defs.h"

namespace retdec {
namespace capstone2llvmir {

/**
 * PowerPC specialization of translator's abstract public interface.
 */
class Capstone2LlvmIrTranslatorPowerpc : virtual public Capstone2LlvmIrTranslator
{

};

} // namespace capstone2llvmir
} // namespace retdec

#endif
