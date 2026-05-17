/**
 * @file include/retdec/rtti-finder/vtable/vtable_gcc.h
 * @brief GCC C++ virtual table structures.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#ifndef RETDEC_RTTI_FINDER_VTABLE_VTABLE_GCC_H
#define RETDEC_RTTI_FINDER_VTABLE_VTABLE_GCC_H

#include <memory>
#include <cstdint>
#include <map>
#include <vector>

#include "retdec/rtti-finder/rtti/rtti_gcc.h"
#include "retdec/common/address.h"
#include "retdec/common/vtable.h"

namespace retdec {
namespace rtti_finder {

/**
 * gcc&clang virtual table sturcture ( [] means array of entries ):
 *
 *   [virtual call (vcall) offsets]
 *   [virtual base (vbase) offsets]
 *   offset to top
 *   typeinfo (RTTI) pointer
 *   [virtual function pointers] <- vtable address in instances points here
 *
 */
class VtableGcc : public retdec::common::Vtable
{
	public:
		VtableGcc(retdec::common::Address a) : Vtable(a) {}

	public:
		/// vcall offsets (Itanium ABI §2.6.1) — non-empty only for secondary
		/// vtables in virtual-inheritance hierarchies; set during hierarchy
		/// reconstruction after RTTI parsing completes.
		std::vector<int> vcallOffsets;

		/// vbase offsets (Itanium ABI §2.6.1) — non-empty only for secondary
		/// vtables; set during hierarchy reconstruction after RTTI parsing.
		std::vector<int> vbaseOffsets;

		/// offset_to_top (Itanium ABI §2.6.2): adjustment added to 'this' to
		/// recover the most-derived object pointer.  Zero for primary vtables.
		/// Populated by vtable_finder.cpp during vtable discovery.
		int topOffset = 0;
		retdec::common::Address rttiAddress;
		std::shared_ptr<ClassTypeInfo> rtti;
};

using VtablesGcc = std::map<retdec::common::Address, VtableGcc>;

} // namespace rtti_finder
} // namespace retdec

#endif
