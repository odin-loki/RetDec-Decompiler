/**
 * @file src/common/address.cpp
 * @brief Address, address pair and other derived class representation.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cassert>
#include <climits>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "retdec/common/address.h"
#include "retdec/utils/conversion.h"

namespace retdec {
namespace common {

//
//=============================================================================
//  Address
//=============================================================================
//

const uint64_t Address::Undefined = ULLONG_MAX;

Address::Address() :
		address(Address::Undefined)
{
}

Address::Address(uint64_t a) :
		address(a)
{
}

Address::Address(const std::string &a) :
		address(Address::Undefined)
{
	try
	{
		size_t idx = 0;
		unsigned long long ull = std::stoull(a, &idx, 0);
		if (idx == a.size()) // no leftovers
		{
			address = ull;
		}
	}
	catch (const std::invalid_argument&)
	{
		// nothing -> undefined value.
	}
	catch (const std::out_of_range&)
	{
		// std::stoull throws this for any numeral above ULLONG_MAX, and it was
		// not caught, so it escaped a constructor documented to yield an
		// undefined address for anything it cannot parse. Every address in a
		// config file arrives here (src/serdes/address.cpp), and no caller
		// catches it: `"entryPoint": "0x1FFFFFFFFFFFFFFFFF"` aborted the
		// process.
	}
}

Address::operator uint64_t() const
{
	return address;
}

Address::operator bool() const
{
	return isDefined() && address;
}

Address& Address::operator++()
{
	if (isDefined())
		address++;

	return *this;
}
Address Address::operator++(int)
{
	if (isDefined())
		address++;

	return *this;
}

Address& Address::operator--()
{
	if (isDefined())
		address--;

	return *this;
}
Address Address::operator--(int)
{
	if (isDefined())
		address--;

	return *this;
}

Address& Address::operator+=(const Address& rhs)
{
	address += rhs;
	return *this;
}
Address& Address::operator-=(const Address& rhs)
{
	address -= rhs;
	return *this;
}
Address& Address::operator|=(const Address& rhs)
{
	address |= rhs;
	return *this;
}

bool Address::isUndefined() const
{
	return address == Address::Undefined;
}

bool Address::isDefined() const
{
	return !isUndefined();
}

uint64_t Address::getValue() const
{
	assert( isDefined() );
	return address;
}

std::string Address::toHexString() const
{
	assert(isDefined());
	return utils::intToHexString(address);
}

std::string Address::toHexPrefixString() const
{
	assert(isDefined());

	return "0x" + toHexString();
}

std::ostream& operator<<(std::ostream &out, const Address &a)
{
	if (a.isDefined())
		return out << a.toHexPrefixString();
	else
		return out << "UNDEFINED";
}

//
//=============================================================================
//  AddressRange
//=============================================================================
//

AddressRange stringToAddrRange(const std::string &r)
{
	AddressRange ar;

	unsigned long long f = 0, s = 0;
	int ret = std::sscanf(r.c_str(), "0x%llx-0x%llx", &f, &s);
	if (ret == 2 && f <= s)
	{
		ar.setStartEnd(f, s);
	}

	return ar;
}

} // namespace common
} // namespace retdec
