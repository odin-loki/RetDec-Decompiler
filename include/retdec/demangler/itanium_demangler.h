/**
 * @file include/retdec/demangler/itanium_demangler.h
 * @brief Itanium demangler adapter.
 * @copyright (c) 2018 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_LLVM_ITANIUM_DEMANGLER_H
#define RETDEC_LLVM_ITANIUM_DEMANGLER_H

#include <memory>
#include "retdec/demangler/demangler_base.h"

namespace retdec {
namespace demangler {

/**
 * @brief Adapter for llvm itanium demangler.
 */
class ItaniumDemangler : public Demangler
{
public:
	ItaniumDemangler();

	std::string demangleToString(const std::string &mangled) override;

	std::shared_ptr<ctypes::Function> demangleFunctionToCtypes(
		const std::string &mangled,
		std::unique_ptr<ctypes::Module> &module,
		const ctypesparser::CTypesParser::TypeWidths &typeWidths,
		const ctypesparser::CTypesParser::TypeSignedness &typeSignedness,
		unsigned defaultBitWidth) override;
};

}
}

#endif //RETDEC_LLVM_ITANIUM_DEMANGLER_H
