/**
 * @file include/retdec/capstone2llvmir/sysz/sysz.h
 * @brief SystemZ specialization of translator's abstract public interface.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_CAPSTONE2LLVMIR_SYSZ_SYSZ_H
#define RETDEC_CAPSTONE2LLVMIR_SYSZ_SYSZ_H

#include "retdec/capstone2llvmir/capstone2llvmir.h"

namespace retdec {
namespace capstone2llvmir {

/**
 * SystemZ (s390x) specialization of translator's abstract public interface.
 *
 * Capstone 5.0.9 has no 31-bit / ESA-390 mode for CS_ARCH_SYSZ. This
 * translator models 64-bit z/Architecture GPRs.
 */
class Capstone2LlvmIrTranslatorSysz : virtual public Capstone2LlvmIrTranslator
{

};

} // namespace capstone2llvmir
} // namespace retdec

#endif
