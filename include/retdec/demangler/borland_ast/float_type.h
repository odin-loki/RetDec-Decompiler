/**
* @file include/retdec/demangler/borland_ast/float_type.h
* @brief Representation of floating point number types in borland AST.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#ifndef RETDEC_FLOAT_TYPE_H
#define RETDEC_FLOAT_TYPE_H

#include <memory>
#include "retdec/demangler/borland_ast/built_in_type.h"

namespace retdec {
namespace demangler {
namespace borland {

/**
 * @brief Representaion of floating point number types.
 */
class FloatTypeNode : public BuiltInTypeNode
{
public:
	static std::shared_ptr<FloatTypeNode> create(
		Context &context,
		const std::string &typeName,
		const Qualifiers &quals);

private:
	FloatTypeNode(const std::string &typeName, const Qualifiers &quals);
};

}    // borland
}    // demangler
}    // retdec

#endif //RETDEC_FLOAT_TYPE_H
