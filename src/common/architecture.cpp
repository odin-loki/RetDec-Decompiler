/**
 * @file src/common/architecture.cpp
 * @brief Common architecture representation.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <algorithm>

#include "retdec/common/architecture.h"
#include "retdec/utils/string.h"

namespace {

const std::string ARCH_UNKNOWN = "unknown";
const std::string ARCH_MIPS    = "mips";
const std::string ARCH_MIPS64  = "mips64";
const std::string ARCH_PIC32   = "pic32";
const std::string ARCH_ARM     = "arm";
const std::string ARCH_ARM64   = "aarch64";
const std::string ARCH_THUMB   = "thumb";
const std::string ARCH_x86     = "x86";
const std::string ARCH_PPC     = "powerpc";
const std::string ARCH_PPC64   = "powerpc64";
const std::string ARCH_RISCV   = "riscv";
const std::string ARCH_RISCV64 = "riscv64";
const std::string ARCH_SPARC   = "sparc";
const std::string ARCH_SPARC64 = "sparc64";
const std::string ARCH_SYSZ    = "sysz";
const std::string ARCH_S390X   = "s390x";
const std::string ARCH_SYSTEMZ = "systemz";
const std::string ARCH_XCORE   = "xcore";
const std::string ARCH_ARM64_CLI = "arm64";
const std::string ARCH_X86_64  = "x86-64";
const std::string ARCH_X86_64U = "x86_64";

} // anonymous namespace

namespace retdec {
namespace common {

bool Architecture::isArm32OrThumb() const { return isArm32() || isThumb(); }
bool Architecture::isPic32() const        { return isArch(eArch::PIC32); }
bool Architecture::isArm() const          { return isArch(eArch::ARM); }
bool Architecture::isArm32() const        { return isArm() && getBitSize() == 32 && !_thumbFlag; }
bool Architecture::isArm64() const        { return isArm() && getBitSize() == 64; }
bool Architecture::isThumb() const        { return isArm() && _thumbFlag; }
bool Architecture::isX86() const          { return isArch(eArch::X86); }
bool Architecture::isX86_16() const       { return isX86() && getBitSize() == 16; }
bool Architecture::isX86_32() const       { return isX86() && getBitSize() == 32; }
bool Architecture::isX86_64() const       { return isX86() && getBitSize() == 64; }
bool Architecture::isPpc() const          { return isArch(eArch::PPC); }
bool Architecture::isPpc64() const        { return isPpc() && getBitSize() == 64; }
bool Architecture::isRiscv() const        { return isArch(eArch::RISCV); }
bool Architecture::isRiscv64() const      { return isRiscv() && getBitSize() == 64; }
bool Architecture::isSparc() const        { return isArch(eArch::SPARC); }
bool Architecture::isSparc64() const      { return isSparc() && getBitSize() == 64; }
bool Architecture::isSysz() const         { return isArch(eArch::SYSZ); }
bool Architecture::isXcore() const        { return isArch(eArch::XCORE); }
bool Architecture::isKnown() const        { return !isUnknown(); }
bool Architecture::isUnknown() const      { return isArch(eArch::UNKNOWN); }
bool Architecture::isMips() const         { return isArch(eArch::MIPS); }
bool Architecture::isMips64() const       { return isMips() && getBitSize() == 64; }
bool Architecture::isMipsOrPic32() const  { return isMips() || isPic32(); }

/**
 * Checks if this architecture instance matches with the provided architecture name.
 * Matching is successful if instance's name contains (case insensitive) the provided name.
 * @param a Name to match with.
 * @return @c True if matching successful, @c false otherwise.
 */
bool Architecture::isArch(const std::string& a) const
{
	return retdec::utils::containsCaseInsensitive(_name, a);
}

bool Architecture::isArch(eArch a) const
{
	return _arch == a;
}

bool Architecture::isEndianLittle() const  { return _endian == E_LITTLE; }
bool Architecture::isEndianBig() const     { return _endian == E_BIG; }
bool Architecture::isEndianUnknown() const { return _endian == E_UNKNOWN; }
bool Architecture::isEndianKnown() const   { return !isEndianUnknown(); }

void Architecture::setIsUnknown()        { setName(ARCH_UNKNOWN); }
void Architecture::setIsMips()           { setName(ARCH_MIPS); }
void Architecture::setIsPic32()          { setName(ARCH_PIC32); }
void Architecture::setIsArm()            { setName(ARCH_ARM); }
void Architecture::setIsThumb()          { setName(ARCH_THUMB); _thumbFlag = true; }
void Architecture::setIsArm32()          { setName(ARCH_ARM); setBitSize(32); }
void Architecture::setIsArm64()          { setName(ARCH_ARM64); setBitSize(64); }
void Architecture::setIsX86()            { setName(ARCH_x86); }
void Architecture::setIsPpc()            { setName(ARCH_PPC); }
void Architecture::setIsRiscv()          { setName(ARCH_RISCV); }
void Architecture::setIsRiscv64()        { setName(ARCH_RISCV64); setBitSize(64); }
void Architecture::setIsSparc()          { setName(ARCH_SPARC); }
void Architecture::setIsSparc64()        { setName(ARCH_SPARC64); setBitSize(64); }
void Architecture::setIsSysz()           { setName(ARCH_SYSZ); setBitSize(64); }
void Architecture::setIsXcore()          { setName(ARCH_XCORE); setBitSize(32); }

void Architecture::setIsEndianLittle()           { _endian = E_LITTLE; }
void Architecture::setIsEndianBig()              { _endian = E_BIG; }
void Architecture::setIsEndianUnknown()          { _endian = E_UNKNOWN; }
void Architecture::setBitSize(unsigned bs)       { _bitSize = bs; }

unsigned Architecture::getBitSize() const  { return _bitSize; }

/**
 * @return Byte size computed from bit size by dividing it by 8.
 */
unsigned Architecture::getByteSize() const { return _bitSize/8; }

/**
 * @return Architecture name. For unknown architecture returns "unknown" string.
 */
std::string Architecture::getName() const
{
	return _name.empty() ? ARCH_UNKNOWN : _name;
}

void Architecture::setName(const std::string& n)
{
	_name = n;
	setArch();
}

void Architecture::setArch()
{
	if (retdec::utils::containsCaseInsensitive(_name, ARCH_UNKNOWN))
	{
		_arch = eArch::UNKNOWN;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_MIPS))
	{
		_arch = eArch::MIPS;
		if (retdec::utils::containsCaseInsensitive(_name, ARCH_MIPS64))
		{
			_bitSize = 64;
		}
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_PIC32))
	{
		_arch = eArch::PIC32;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_THUMB))
	{
		_arch = eArch::ARM;
		_thumbFlag = true;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_ARM64)
			|| retdec::utils::containsCaseInsensitive(_name, ARCH_ARM64_CLI))
	{
		_arch = eArch::ARM;
		_thumbFlag = false;
		_bitSize = 64;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_ARM))
	{
		_arch = eArch::ARM;
		_thumbFlag = false;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_x86))
	{
		_arch = eArch::X86;
		if (retdec::utils::containsCaseInsensitive(_name, ARCH_X86_64)
				|| retdec::utils::containsCaseInsensitive(_name, ARCH_X86_64U))
		{
			_bitSize = 64;
		}
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_PPC64))
	{
		_arch = eArch::PPC;
		_bitSize = 64;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_PPC))
	{
		_arch = eArch::PPC;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_RISCV64))
	{
		_arch = eArch::RISCV;
		_bitSize = 64;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_RISCV))
	{
		_arch = eArch::RISCV;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_SPARC64))
	{
		_arch = eArch::SPARC;
		_bitSize = 64;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_SPARC))
	{
		_arch = eArch::SPARC;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_S390X)
			|| retdec::utils::containsCaseInsensitive(_name, ARCH_SYSTEMZ)
			|| retdec::utils::containsCaseInsensitive(_name, ARCH_SYSZ))
	{
		_arch = eArch::SYSZ;
		_bitSize = 64;
	}
	else if (retdec::utils::containsCaseInsensitive(_name, ARCH_XCORE))
	{
		_arch = eArch::XCORE;
		_bitSize = 32;
	}
}

} // namespace common
} // namespace retdec
