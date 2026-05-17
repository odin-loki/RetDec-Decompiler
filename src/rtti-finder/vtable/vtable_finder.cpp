/**
 * @file src/rtti-finder/vtable/vtable_finder.cpp
 * @brief Find vtable structures in @c Image.
 * @copyright (c) 2017 Odin Loch Trading as Imortek
 */

#include <iostream>

#include "retdec/loader/loader/image.h"
#include "retdec/rtti-finder/rtti/rtti_gcc_parser.h"
#include "retdec/rtti-finder/rtti/rtti_msvc_parser.h"
#include "retdec/rtti-finder/vtable/vtable_finder.h"

#define LOG \
	if (!debug_enabled) {} \
	else std::cout << std::showbase
const bool debug_enabled = false;

using namespace retdec::common;
using namespace retdec::utils;
using namespace retdec::rtti_finder;

void findPossibleVtables(
		const retdec::loader::Image* img,
		std::set<retdec::common::Address>& possibleVtables,
		bool gcc)
{
	auto wordSz = img->getBytesPerWord();

	for (auto& seg : img->getSegments())
	{
		if (seg->getSecSeg() && !seg->getSecSeg()->isSomeData())
		{
			continue;
		}

		auto addr = seg->getAddress();
		auto end = seg->getEndAddress();
		while (addr + wordSz < end)
		{
			std::uint64_t val = 0;
			if (!img->getWord(addr, val))
			{
				addr += wordSz;
				continue;
			}

			if (gcc && val != 0)
			{
				addr += wordSz;
				continue;
			}

			Address item1 = addr + wordSz;
			Address item2 = item1 + wordSz;

			if (!img->isPointer(item1)
					|| !img->isPointer(item2))
			{
				addr += wordSz;
				continue;
			}

			possibleVtables.insert(item2);
			addr = item2;
		}
	}
}

/**
 * Stage 13: ABI-specific vtable validation.
 * Reject candidates that fail alignment or have invalid RTTI targets.
 */
static bool validateVtableAbi(
		const retdec::loader::Image* img,
		Address vtableAddr,
		std::uint64_t rttiPtrVal)
{
	auto bpw = img->getBytesPerWord();
	// Vtable must be aligned to word size (4 or 8 bytes)
	if (vtableAddr % bpw != 0)
		return false;
	// RTTI pointer must point to readable section (data or code for type_info)
	if (!img->hasDataOnAddress(rttiPtrVal) && !img->hasDataInitializedOnAddress(rttiPtrVal))
		return false;
	return true;
}

/**
 * @return @c True if vtable ok and can be used, @c false if it should
 * be thrown away.
 */
bool fillVtable(
		const retdec::loader::Image* img,
		std::set<retdec::common::Address>& processedAddresses,
		Address a,
		Vtable& vt)
{
	LOG << "\t\t" << "fillVtable() @ " << a << std::endl;
	std::set<retdec::common::Address> items;

	bool isThumb = false;
	auto bpw = img->getBytesPerWord();
	std::uint64_t ptr = 0;
	auto isPtr = img->isPointer(a, &ptr);
	while (true)
	{
		if (!isPtr)
		{
			LOG << "\t\t\t" << a << " @ !isPtr" << std::endl;
			break;
		}
		if (img->getFileFormat()->isArm() && ptr % 2)
		{
			--ptr;
			isThumb = true;
		}
		if (processedAddresses.find(a) != processedAddresses.end())
		{
			LOG << "\t\t\t" << a << " @ !processedAddresses" << std::endl;
			break;
		}
		auto* seg = img->getSegmentFromAddress(ptr);
		if (seg == nullptr
				|| seg->getSecSeg() == nullptr
				|| !seg->getSecSeg()->isSomeCode())
		{
			LOG << "\t\t\t" << a << " @ !isSomeCode" << std::endl;
			break;
		}

		// All items in vtable must be unique (really???).
		//
		if (items.find(ptr) != items.end())
		{
			LOG << "\t\t\t" << a << " @ !unique" << std::endl;
			return false;
		}

		LOG << "\t\t\t" << a << " @ OK" << std::endl;
		vt.items.emplace(VtableItem(a, ptr, isThumb));
		items.insert(ptr);
		processedAddresses.insert(a);

		a += bpw;
		isPtr = img->isPointer(a, &ptr);
	}

	if (vt.items.empty())
	{
		LOG << "\t\t\t" << "===> FAIL" << std::endl;
		return false;
	}

	LOG << "\t\t\t" << "===> OK" << std::endl;
	return true;
}

/**
 * @note This method is defined outside the namespace retdec::rtti_finder with
 *       explicit namespace declarations to help Doxygen and prevent it from
 *       generating "no matching file member found for" warnings.
 */
void retdec::rtti_finder::findGccVtables(
		const retdec::loader::Image* img,
		retdec::rtti_finder::VtablesGcc& vtables,
		retdec::rtti_finder::RttiGcc& rttis)
{
	std::set<retdec::common::Address> possibleVtables;
	findPossibleVtables(img, possibleVtables, true);

	std::set<retdec::common::Address> processedAddresses;
	for (auto addr : possibleVtables)
	{
		LOG << "\t" << "possible vtable @ " << addr << std::endl;
		retdec::rtti_finder::VtableGcc vt(addr);

		if (!fillVtable(img, processedAddresses, addr, vt))
		{
			LOG << "\t\t" << "fillVtable() failed" << std::endl;
			continue;
		}

		auto bpw = img->getBytesPerWord();
		auto rttiPtrAddr = addr - bpw;
		std::uint64_t rttiAddr = 0;
		if (img->getWord(rttiPtrAddr, rttiAddr))
		{
			// Stage 13: ABI validation — reject invalid vtable/RTTI
			if (!validateVtableAbi(img, addr, rttiAddr))
			{
				LOG << "\t\t" << "validateVtableAbi() failed" << std::endl;
				continue;
			}
			std::set<retdec::common::Address> visited;
			vt.rttiAddress = rttiAddr;
			vt.rtti = parseGccRtti(img, rttis, vt.rttiAddress, visited);
			if (vt.rtti == nullptr)
			{
				LOG << "\t\t" << "parseGccRtti() failed" << std::endl;
				continue;
			}

			// Read offset_to_top (signed pointer-width integer at addr - 2*bpw).
			// Per Itanium ABI §2.6.2, this is the adjustment added to a derived-
			// class pointer to get the pointer to the subobject for which this
			// vtable is used.  Zero for most-derived single-inheritance vtables.
			std::uint64_t topOffRaw = 0;
			if (img->getWord(addr - 2 * bpw, topOffRaw)) {
				vt.topOffset = (bpw == 4)
				    ? static_cast<int>(static_cast<int32_t>(topOffRaw))
				    : static_cast<int>(static_cast<int64_t>(topOffRaw));
			}

			// vcallOffsets and vbaseOffsets (Itanium ABI §2.6.1) appear above
			// the RTTI pointer only in secondary vtables for virtual bases.
			// Their count and layout depend on the full class hierarchy, which
			// requires multi-pass RTTI analysis to determine.  They are left
			// empty here for primary vtables; secondary vtable parsing (if
			// supported) should populate them during hierarchy reconstruction.
		}
		else
		{
			continue;
		}

		vtables.emplace(addr, vt);
	}

	LOG << "\t\t" << "vtable OK" << std::endl;
	finalizeGccRtti(rttis);
}

/**
 * @note This method is defined outside the namespace retdec::rtti_finder with
 *       explicit namespace declarations to help Doxygen and prevent it from
 *       generating "no matching file member found for" warnings.
 */
void retdec::rtti_finder::findMsvcVtables(
		const retdec::loader::Image* img,
		retdec::rtti_finder::VtablesMsvc& vtables,
		retdec::rtti_finder::RttiMsvc& rttis)
{
	std::set<retdec::common::Address> possibleVtables;
	findPossibleVtables(img, possibleVtables, false);

	std::set<retdec::common::Address> processedAddresses;
	for (auto addr : possibleVtables)
	{
		retdec::rtti_finder::VtableMsvc vt(addr);

		if (!fillVtable(img, processedAddresses, addr, vt))
		{
			continue;
		}

		auto rttiPtrAddr = addr - img->getBytesPerWord();
		std::uint64_t rttiAddr = 0;
		if (img->getWord(rttiPtrAddr, rttiAddr))
		{
			// Stage 13: ABI validation for MSVC vtables
			if (!validateVtableAbi(img, addr, rttiAddr))
			{
				continue;
			}
			vt.objLocatorAddress = rttiAddr;
			vt.rtti = parseMsvcRtti(img, rttis, vt.objLocatorAddress);
			if (vt.rtti == nullptr)
			{
				continue;
			}
		}
		else
		{
			continue;
		}

		vtables.emplace(addr, vt);
	}
}
