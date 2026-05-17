/**
 * @file include/retdec/bin2llvmir/providers/calling_convention/mips/mips_psp.h
 * @brief Calling convention of MIPS architecture.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_BIN2LLVMIR_PROVIDERS_CALL_CONV_MIPS_MIPS_PSP_CONV_H
#define RETDEC_BIN2LLVMIR_PROVIDERS_CALL_CONV_MIPS_MIPS_PSP_CONV_H

#include "retdec/bin2llvmir/providers/calling_convention/calling_convention.h"

namespace retdec {
namespace bin2llvmir {

class MipsPSPCallingConvention: public CallingConvention
{
	// Ctors, dtors.
	//
	public:
		MipsPSPCallingConvention(const Abi* a);
};

}
}

#endif
