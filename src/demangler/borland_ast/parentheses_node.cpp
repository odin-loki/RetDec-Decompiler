/**
* @file src/demangler/borland_ast/parentheses_node.cpp
* @brief Representation of parentheses.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <memory>
#include <sstream>

#include "retdec/demangler/borland_ast/parentheses_node.h"

namespace retdec {
namespace demangler {
namespace borland {

ParenthesesNode::ParenthesesNode(std::shared_ptr<Node> type) :
	Node(Kind::KParentheses), _type(std::move(type)) {}

void ParenthesesNode::printLeft(std::ostream &s) const
{
	s << "(";
	_type->print(s);
	s << ")";
}

std::shared_ptr<ParenthesesNode> ParenthesesNode::create(std::shared_ptr<Node> type)
{
	return std::shared_ptr<ParenthesesNode>(new ParenthesesNode(std::move(type)));
}

}
}
}
