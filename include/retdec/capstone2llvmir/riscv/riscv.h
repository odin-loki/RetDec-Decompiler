/**
 * @file include/retdec/capstone2llvmir/riscv/riscv.h
 * @brief RISC-V specialization of translator's abstract public interface.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_CAPSTONE2LLVMIR_RISCV_RISCV_H
#define RETDEC_CAPSTONE2LLVMIR_RISCV_RISCV_H

#include "retdec/capstone2llvmir/riscv/riscv_defs.h"
#include "retdec/capstone2llvmir/capstone2llvmir.h"

namespace retdec {
namespace capstone2llvmir {

/**
 * RISC-V specialization of translator's abstract public interface.
 */
class Capstone2LlvmIrTranslatorRiscv : virtual public Capstone2LlvmIrTranslator
{

};

} // namespace capstone2llvmir
} // namespace retdec

#endif
