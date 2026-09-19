/**
 * @file include/retdec/capstone2llvmir/xcore/xcore.h
 * @brief XCore specialization of translator's abstract public interface.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_CAPSTONE2LLVMIR_XCORE_XCORE_H
#define RETDEC_CAPSTONE2LLVMIR_XCORE_XCORE_H

#include "retdec/capstone2llvmir/capstone2llvmir.h"

namespace retdec {
namespace capstone2llvmir {

/**
 * XCore specialization of translator's abstract public interface.
 *
 * The XS1/XS2 ISA and Capstone 5.0.9 CS_ARCH_XCORE are 32-bit only.
 * There is no 64-bit XCore mode.
 */
class Capstone2LlvmIrTranslatorXcore : virtual public Capstone2LlvmIrTranslator
{

};

} // namespace capstone2llvmir
} // namespace retdec

#endif
