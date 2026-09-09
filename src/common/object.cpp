/**
 * @file src/common/object.cpp
 * @brief Common object representation.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cassert>

#include "retdec/common/object.h"
#include "retdec/common/storage.h"
#include "retdec/common/type.h"

namespace retdec {
namespace common {

//
//=============================================================================
// Object
//=============================================================================
//

Object::Object()
{

}

Object::Object(const std::string& name, const common::Storage& storage) :
		_name(name),
		_storage(storage)
{
	assert( !getName().empty() );
}

/**
 * Object are equal if their names are equal.
 * @param o Other object.
 */
bool Object::operator==(const Object& o) const
{
	return _name == o._name;
}

/**
 * Objects are compared by their uniqie IDs (i.e. names).
 */
bool Object::operator<(const Object& o) const
{
	return _name < o._name;
}

void Object::setName(const std::string& n)
{
	_name = n;
}

void Object::setRealName(const std::string& n)
{
	_realName = n;
}

void Object::setCryptoDescription(const std::string& d)
{
	_cryptoDescription = d;
}

void Object::setIsFromDebug(bool b)
{
	_fromDebug = b;
}

void Object::setStorage(const common::Storage& s)
{
	_storage = s;
}

bool Object::isFromDebug() const
{
	return _fromDebug;
}

/**
 * @return Object's ID is its name.
 */
const std::string& Object::getId() const
{
	return getName();
}

const std::string& Object::getName() const
{
	return _name;
}

/**
 * @return Real name of this object to appear in output C.
 * This may or may not differ from @c name.
 */
const std::string& Object::getRealName() const
{
	return _realName;
}

const std::string& Object::getCryptoDescription() const
{
	return _cryptoDescription;
}

const common::Storage& Object::getStorage() const
{
	return _storage;
}

//
//=============================================================================
// ObjectContainer
//=============================================================================
//

/**
 * @return Pointer to object or @c nullptr if not found.
 */
const Object* ObjectSequentialContainer::getObjectByName(
		const std::string& name) const
{
	for (auto& elem : *this)
	{
		if (elem.getName() == name)
			return &elem;
	}
	return nullptr;
}

//
//=============================================================================
// ObjectSetContainer
//=============================================================================
//

/**
 * @return Pointer to object or @c nullptr if not found.
 */
const Object* ObjectSetContainer::getObjectByName(
		const std::string& name) const
{
	auto it = find(name);
	return it != end() ? &(*it) : nullptr;
}

//
//=============================================================================
// GlobalVarContainer
//=============================================================================
//

GlobalVarContainer::GlobalVarContainer() :
		ObjectSetContainer()
{

}

GlobalVarContainer::GlobalVarContainer(const GlobalVarContainer& o) :
		ObjectSetContainer(o)
{
	*this = o;
}

/**
 * We need to make sure pointers in @c _addr2global are valid -- point
 * to the new @c _data container, not the old one.
 */
GlobalVarContainer& GlobalVarContainer::operator=(const GlobalVarContainer& o)
{
	if (this != &o)
	{
		ObjectSetContainer::operator=(o);
		_addr2global.clear();
		for (auto& p : *this)
		{
			_addr2global[p.getStorage().getAddress()] = &p;
		}
	}
	return *this;
}

/**
 * @return Pointer to global object or @c nullptr if not found.
 */
const Object* GlobalVarContainer::getObjectByAddress(
		const retdec::common::Address& address) const
{
	auto fIt = _addr2global.find(address);
	return fIt != _addr2global.end() ? fIt->second : nullptr;
}

/**
 * Besides calling the underlying container's insert which checks (and replaces)
 * for existing elements with the same unique ID (name), this method also check
 * for elements with the same global address. Such elements are also removed
 * before adding the new element, since container is not allowed to hold two
 * ebjects with the same global address. Moreover, @c _addr2global is also
 * updated.
 */
std::pair<GlobalVarContainer::iterator,bool> GlobalVarContainer::insert(
		const Object& e)
{
	// A refusal, not a precondition: a "globals" entry in a config file can
	// name a register, or omit its address entirely, and both arrive here. An
	// assert would make that a file-controlled abort.
	if (!e.getStorage().isMemory())
	{
		return {end(), false};
	}

	auto existing = getObjectByAddress(e.getStorage().getAddress());
	if (existing)
	{
		erase(*existing);
	}
	auto fit = find(e.getName());
	if (fit != end())
	{
		// _addr2global's entry for the node about to be dropped is keyed by
		// THAT node's address, which is not e's -- that is the whole reason
		// this branch exists. Dropping the node without erasing the entry
		// leaves the map holding a pointer into the freed set node, and
		// getObjectByAddress() hands it straight to its callers. Two "globals"
		// entries in a config file with the same name at different addresses
		// are enough.
		if (fit->getStorage().isMemory())
		{
			_addr2global.erase(fit->getStorage().getAddress());
		}
		ObjectSetContainer::erase(fit);
	}

	auto retPair = ObjectSetContainer::insert(e);

	const Object* obj = &(*retPair.first);
	const auto& addr = e.getStorage().getAddress();
	auto res = _addr2global.emplace(addr,obj);
	if (!res.second)
	{
		// There is already an object on this address, so overwrite it
		// to ensure that it is updated.
		res.first->second = obj;
	}

	return retPair;
}

std::pair<GlobalVarContainer::iterator,bool> GlobalVarContainer::insert(
		GlobalVarContainer::iterator,
		const Object& e)
{
	return insert(e);
}

/**
 * Clear both underlying container and @c addr2global map.
 */
void GlobalVarContainer::clear()
{
	ObjectSetContainer::clear();
	_addr2global.clear();
}

/**
 * Erase from both underlying container and @c addr2global map.
 */
size_t GlobalVarContainer::erase(const Object& val)
{
	// find() matches on the id (the name), so the stored object need not be
	// val: it can sit at a different address. Erasing val's address from
	// _addr2global rather than the stored object's left the stored object's
	// entry behind, pointing at a node this call is about to free.
	auto it = find(val.getId());
	if (it == end())
	{
		return 0;
	}
	if (it->getStorage().isMemory())
	{
		_addr2global.erase(it->getStorage().getAddress());
	}
	return ObjectSetContainer::erase(*it);
}

} // namespace common
} // namespace retdec
