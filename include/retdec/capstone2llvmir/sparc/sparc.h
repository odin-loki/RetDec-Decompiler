/**
 * @file include/retdec/capstone2llvmir/sparc/sparc.h
 * @brief SPARC specialization of translator's abstract public interface.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_CAPSTONE2LLVMIR_SPARC_SPARC_H
#define RETDEC_CAPSTONE2LLVMIR_SPARC_SPARC_H

#include "retdec/capstone2llvmir/capstone2llvmir.h"

namespace retdec {
namespace capstone2llvmir {

/**
 * SPARC specialization of translator's abstract public interface.
 *
 * One implementation covers SPARC V8 (32-bit) and SPARC V9 / SPARC64.
 * V9 is selected by extra mode @c CS_MODE_V9 (see @c createSparc()).
 * Capstone 5.0.9 @c CS_ARCH_SPARC does not accept @c CS_MODE_32 / @c CS_MODE_64
 * as a basic mode; those values are mapped to extra @c CS_MODE_V9 in
 * @c createArch() wiring documented in @c docs/internal/wire-sparc.md.
 */
class Capstone2LlvmIrTranslatorSparc : virtual public Capstone2LlvmIrTranslator
{

};

} // namespace capstone2llvmir
} // namespace retdec

#endif
