/**
* @file src/llvmir2hll/hll/compound_op_managers/no_compound_op_manager.cpp
* @brief Implementation of NoCompoundOpManager.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include "retdec/llvmir2hll/hll/compound_op_managers/no_compound_op_manager.h"

namespace retdec {
namespace llvmir2hll {

/**
* @brief Constructs a new compound operator manager that turns off all compound
*        optimizations.
*/
NoCompoundOpManager::NoCompoundOpManager(): CompoundOpManager() {}

std::string NoCompoundOpManager::getId() const {
	return "NoCompoundOpManager";
}

} // namespace llvmir2hll
} // namespace retdec
