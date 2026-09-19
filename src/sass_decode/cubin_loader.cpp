/**
 * @file src/sass_decode/cubin_loader.cpp
 * @brief cubin ELF (EM_CUDA) and fatbin container loader.
 */

#include "retdec/sass_decode/cubin_loader.h"

#include <cstring>
#include <string>

namespace retdec {
namespace sass_decode {
namespace {

constexpr size_t kMaxCubinBytes = 64u * 1024u * 1024u;
constexpr uint32_t kShtProgbits = 1;
constexpr uint32_t kShfExecinstr = 4;
constexpr uint16_t kEtRel = 1;
constexpr uint16_t kEtExec = 2;
constexpr uint16_t kEtDyn = 3;

uint16_t u16le(const uint8_t* p)
{
	return static_cast<uint16_t>(p[0] | (uint16_t(p[1]) << 8));
}
uint32_t u32le(const uint8_t* p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t u64le(const uint8_t* p)
{
	return uint64_t(u32le(p)) | (uint64_t(u32le(p + 4)) << 32);
}

bool inRange(size_t size, uint64_t off, uint64_t n)
{
	if (n > kMaxCubinBytes || off > kMaxCubinBytes)
	{
		return false;
	}
	if (off > size)
	{
		return false;
	}
	return n <= size - static_cast<size_t>(off);
}

std::string cstrAt(const uint8_t* base, size_t tableSize, size_t off)
{
	if (off >= tableSize)
	{
		return {};
	}
	size_t n = 0;
	while (off + n < tableSize && base[off + n] != 0)
	{
		++n;
		if (n > 4096)
		{
			break;
		}
	}
	return std::string(reinterpret_cast<const char*>(base + off), n);
}

bool isPtxText(const uint8_t* p, size_t n)
{
	if (n < 8)
	{
		return false;
	}
	std::string s(reinterpret_cast<const char*>(p), n > 256 ? 256 : n);
	return s.find(".version") != std::string::npos && s.find(".target") != std::string::npos;
}

std::string extractPtx(const uint8_t* data, size_t size)
{
	// PTX is ASCII. Look for a `.version` / `.target` pair and take through
	// the last non-NUL byte of that island. Fatbin often stores a NUL-terminated
	// PTX blob after the public header.
	for (size_t i = 0; i + 8 < size; ++i)
	{
		if (data[i] != '.')
		{
			continue;
		}
		if (i + 8 <= size && std::memcmp(data + i, ".version", 8) == 0)
		{
			size_t end = i;
			while (end < size && data[end] != 0)
			{
				++end;
			}
			if (end - i >= 16 && isPtxText(data + i, end - i))
			{
				return std::string(reinterpret_cast<const char*>(data + i), end - i);
			}
		}
	}
	return {};
}

struct ElfView
{
	const uint8_t* data = nullptr;
	size_t size = 0;
	bool elf64 = false;
	uint16_t machine = 0;
	uint32_t flags = 0;
	uint8_t abi = 0;
	uint64_t shoff = 0;
	uint16_t shentsize = 0;
	uint16_t shnum = 0;
	uint16_t shstrndx = 0;
};

bool parseElfHeader(const uint8_t* data, size_t size, ElfView& v, std::string& err)
{
	if (size < 52 || data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
	{
		err = "not ELF";
		return false;
	}
	if (data[5] != 1)
	{
		err = "cubin ELF is expected little-endian";
		return false;
	}
	v.data = data;
	v.size = size;
	v.elf64 = (data[4] == 2);
	v.abi = data[8];
	if (v.elf64)
	{
		if (size < 64)
		{
			err = "truncated ELF64 header";
			return false;
		}
		v.machine = u16le(data + 18);
		v.flags = u32le(data + 48);
		v.shoff = u64le(data + 40);
		v.shentsize = u16le(data + 58);
		v.shnum = u16le(data + 60);
		v.shstrndx = u16le(data + 62);
		if (v.shentsize != 64)
		{
			err = "unexpected ELF64 shentsize";
			return false;
		}
	}
	else if (data[4] == 1)
	{
		v.machine = u16le(data + 18);
		v.flags = u32le(data + 36);
		v.shoff = u32le(data + 32);
		v.shentsize = u16le(data + 46);
		v.shnum = u16le(data + 48);
		v.shstrndx = u16le(data + 50);
		if (v.shentsize != 40)
		{
			err = "unexpected ELF32 shentsize";
			return false;
		}
	}
	else
	{
		err = "unknown ELF class";
		return false;
	}
	if (v.shnum > 4096 || v.shstrndx >= v.shnum)
	{
		err = "implausible section table";
		return false;
	}
	const uint64_t shBytes = uint64_t(v.shnum) * v.shentsize;
	if (!inRange(size, v.shoff, shBytes))
	{
		err = "section table out of range";
		return false;
	}
	return true;
}

struct Shdr
{
	uint32_t nameOff = 0;
	uint32_t type = 0;
	uint64_t flags = 0;
	uint64_t offset = 0;
	uint64_t size = 0;
	uint32_t link = 0;
	uint32_t info = 0;
	uint64_t entsize = 0;
};

bool readShdr(const ElfView& v, uint16_t idx, Shdr& sh)
{
	const uint8_t* p = v.data + static_cast<size_t>(v.shoff) + size_t(idx) * v.shentsize;
	if (v.elf64)
	{
		sh.nameOff = u32le(p + 0);
		sh.type = u32le(p + 4);
		sh.flags = u64le(p + 8);
		sh.offset = u64le(p + 24);
		sh.size = u64le(p + 32);
		sh.link = u32le(p + 40);
		sh.info = u32le(p + 44);
		sh.entsize = u64le(p + 56);
	}
	else
	{
		sh.nameOff = u32le(p + 0);
		sh.type = u32le(p + 4);
		sh.flags = u32le(p + 8);
		sh.offset = u32le(p + 16);
		sh.size = u32le(p + 20);
		sh.link = u32le(p + 24);
		sh.info = u32le(p + 28);
		sh.entsize = u32le(p + 36);
	}
	return true;
}

bool sectionBytes(const ElfView& v, const Shdr& sh, const uint8_t*& out, size_t& n, std::string& err)
{
	if (sh.type == 8 /* SHT_NOBITS */)
	{
		out = nullptr;
		n = 0;
		return true;
	}
	if (!inRange(v.size, sh.offset, sh.size))
	{
		err = "section bytes out of range";
		return false;
	}
	out = v.data + static_cast<size_t>(sh.offset);
	n = static_cast<size_t>(sh.size);
	return true;
}

std::string kernelNameFromSection(const std::string& sec)
{
	static const char kPref[] = ".text.";
	if (sec.size() > sizeof(kPref) - 1 && sec.compare(0, sizeof(kPref) - 1, kPref) == 0)
	{
		return sec.substr(sizeof(kPref) - 1);
	}
	if (sec == ".text")
	{
		return "text";
	}
	return sec;
}

} // namespace

bool looksLikeCubin(const uint8_t* data, size_t size)
{
	if (!data || size < 20)
	{
		return false;
	}
	if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
	{
		return false;
	}
	return u16le(data + 18) == kEmCuda;
}

bool looksLikeFatbin(const uint8_t* data, size_t size)
{
	if (!data || size < 16)
	{
		return false;
	}
	const uint32_t mag = u32le(data);
	return mag == kFatbinMagic || mag == kOldFatbinMagic;
}

SassModule loadCubin(const uint8_t* data, size_t size, const SassConfig& cfg)
{
	SassModule mod;
	if (!data || size == 0)
	{
		mod.error = "empty cubin";
		return mod;
	}
	if (size > kMaxCubinBytes)
	{
		mod.error = "cubin too large";
		return mod;
	}

	ElfView v;
	if (!parseElfHeader(data, size, v, mod.error))
	{
		return mod;
	}
	mod.elf64 = v.elf64;
	mod.eMachine = v.machine;
	if (v.machine != kEmCuda)
	{
		mod.error = "ELF e_machine is not EM_CUDA (190)";
		return mod;
	}

	const uint16_t et = u16le(data + 16);
	if (et != kEtRel && et != kEtExec && et != kEtDyn)
	{
		mod.warnings.push_back("unusual ELF e_type for cubin");
	}

	mod.sm = cfg.smOverride ? cfg.smOverride : smFromEflags(v.flags, v.abi);
	if (cfg.pointerBits == 32 || cfg.pointerBits == 64)
	{
		mod.pointerBits = cfg.pointerBits;
	}
	else
	{
		mod.pointerBits = v.elf64 ? 64u : 32u;
	}
	if (!cfg.smOverride && !isKnownSm(mod.sm))
	{
		mod.warnings.push_back("unrecognised cubin e_flags SM; assuming Volta+ 16-byte words if ELF64");
		if (v.elf64 && mod.sm < 70)
		{
			mod.sm = 80;
		}
	}

	Shdr shstr;
	if (!readShdr(v, v.shstrndx, shstr))
	{
		mod.error = "cannot read shstrtab header";
		return mod;
	}
	const uint8_t* strBase = nullptr;
	size_t strN = 0;
	if (!sectionBytes(v, shstr, strBase, strN, mod.error))
	{
		return mod;
	}

	for (uint16_t i = 1; i < v.shnum; ++i)
	{
		Shdr sh;
		readShdr(v, i, sh);
		const std::string name = strBase ? cstrAt(strBase, strN, sh.nameOff) : std::string();
		const bool exec = (sh.flags & kShfExecinstr) != 0;
		const bool textName = name == ".text" || (name.size() >= 6 && name.compare(0, 6, ".text.") == 0)
			|| (name.size() >= 5 && name.compare(0, 5, ".text") == 0 && name != ".text.rel");
		if (sh.type != kShtProgbits || (!exec && !textName))
		{
			continue;
		}
		const uint8_t* bytes = nullptr;
		size_t n = 0;
		std::string secErr;
		if (!sectionBytes(v, sh, bytes, n, secErr))
		{
			mod.warnings.push_back(secErr + " (" + name + ")");
			continue;
		}
		SassKernel k;
		k.name = kernelNameFromSection(name);
		k.fileOffset = sh.offset;
		if (bytes && n)
		{
			k.bytes.assign(bytes, bytes + n);
		}
		mod.kernels.push_back(std::move(k));
	}

	if (mod.kernels.empty())
	{
		mod.warnings.push_back("no .text / SHF_EXECINSTR sections");
	}
	return mod;
}

SassModule loadFatbin(const uint8_t* data, size_t size, const SassConfig& cfg)
{
	SassModule mod;
	if (!looksLikeFatbin(data, size))
	{
		mod.error = "not a CUDA fatbin (magic 0xBA55ED50)";
		return mod;
	}

	// NVIDIA fatbinary.h: magic, version, headerSize, fatSize. Header size is
	// a multiple of 8; payload follows the header.
	const uint16_t version = u16le(data + 4);
	const uint16_t headerSize = u16le(data + 6);
	const uint64_t fatSize = u64le(data + 8);
	(void)version;
	if (headerSize < 16 || (headerSize & 7) != 0 || headerSize > size)
	{
		mod.error = "invalid fatbin headerSize";
		return mod;
	}
	if (fatSize > kMaxCubinBytes || headerSize + fatSize > size)
	{
		// Tolerate truncated payloads: parse what we have.
		mod.warnings.push_back("fatbin fatSize exceeds buffer; scanning remaining bytes");
	}
	const size_t payloadOff = headerSize;
	const size_t payloadN = size > payloadOff ? size - payloadOff : 0;
	const uint8_t* payload = data + payloadOff;

	mod.ptxText = extractPtx(payload, payloadN);

	// Scan 8-byte-aligned offsets for nested cubin ELF (EM_CUDA).
	SassModule cubin;
	bool found = false;
	const size_t scanN = payloadN > kMaxCubinBytes ? kMaxCubinBytes : payloadN;
	for (size_t off = 0; off + 20 < scanN; off += 8)
	{
		if (payload[off] != 0x7f || payload[off + 1] != 'E' || payload[off + 2] != 'L' || payload[off + 3] != 'F')
		{
			continue;
		}
		if (u16le(payload + off + 18) != kEmCuda)
		{
			continue;
		}
		cubin = loadCubin(payload + off, scanN - off, cfg);
		if (cubin.ok() && (!cubin.kernels.empty() || cubin.eMachine == kEmCuda))
		{
			found = true;
			break;
		}
	}

	if (found)
	{
		cubin.ptxText = std::move(mod.ptxText);
		if (cfg.pointerBits == 32 || cfg.pointerBits == 64)
		{
			cubin.pointerBits = cfg.pointerBits;
		}
		return cubin;
	}

	if (!mod.ptxText.empty())
	{
		mod.warnings.push_back("fatbin contains PTX but no uncompressed cubin ELF");
		mod.pointerBits = cfg.pointerBits == 32 ? 32u : 64u;
		return mod;
	}

	mod.error = "fatbin has no uncompressed cubin or PTX (compressed members are not inflated)";
	return mod;
}

SassModule loadCudaBinary(const uint8_t* data, size_t size, const SassConfig& cfg)
{
	if (looksLikeFatbin(data, size))
	{
		return loadFatbin(data, size, cfg);
	}
	if (looksLikeCubin(data, size))
	{
		return loadCubin(data, size, cfg);
	}
	SassModule mod;
	mod.error = "input is neither cubin (ELF EM_CUDA) nor fatbin";
	return mod;
}

SassModule loadCudaBinary(const std::vector<uint8_t>& bytes, const SassConfig& cfg)
{
	return loadCudaBinary(bytes.data(), bytes.size(), cfg);
}

} // namespace sass_decode
} // namespace retdec
