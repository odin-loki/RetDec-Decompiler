/**
 * @file include/retdec/demangler/borland_demangler.h
 * @brief Borland demangler adapter.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_LLVM_BORLAND_DEMANGLER_H
#define RETDEC_LLVM_BORLAND_DEMANGLER_H

#include <memory>
#include "retdec/demangler/demangler_base.h"
#include "retdec/demangler/borland_ast_parser.h"

namespace retdec {
namespace demangler {

/**
 * @brief Borland demangler.
 */
class BorlandDemangler : public Demangler
{
public:
	BorlandDemangler();

	std::string demangleToString(const std::string &mangled) override;

	std::shared_ptr<ctypes::Function> demangleFunctionToCtypes(
		const std::string &mangled,
		std::unique_ptr<ctypes::Module> &module,
		const ctypesparser::CTypesParser::TypeWidths &typeWidths,
		const ctypesparser::CTypesParser::TypeSignedness &typeSignedness,
		unsigned defaultBitWidth) override;

private:
	borland::Context _demangleContext;
};

}
}

#endif //RETDEC_LLVM_BORLAND_DEMANGLER_H
