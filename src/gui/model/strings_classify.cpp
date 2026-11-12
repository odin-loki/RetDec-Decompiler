/**
 * @file src/gui/model/strings_classify.cpp
 * @brief String categories and constant labels.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/strings_classify.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <regex>
#include <unordered_map>

namespace retdec::gui::model {

namespace {

char lower(char c)
{
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool startsWithCi(std::string_view s, std::string_view p)
{
	if (s.size() < p.size()) return false;
	for (std::size_t i = 0; i < p.size(); ++i)
		if (lower(s[i]) != lower(p[i])) return false;
	return true;
}

bool endsWithCi(std::string_view s, std::string_view p)
{
	return s.size() >= p.size() && startsWithCi(s.substr(s.size() - p.size()), p);
}

bool containsCi(std::string_view s, std::string_view p)
{
	if (p.empty()) return true;
	if (s.size() < p.size()) return false;
	for (std::size_t i = 0; i + p.size() <= s.size(); ++i)
		if (startsWithCi(s.substr(i), p)) return true;
	return false;
}

} // namespace

bool CategoryDetector::isFormatString(std::string_view v)
{
	static const std::regex re(
		R"(%[-+0 #]?\d*(\.\d+)?[diouxXeEfgGaAcsSpn%])", std::regex::ECMAScript | std::regex::icase);
	return std::regex_search(v.begin(), v.end(), re);
}

bool CategoryDetector::isURL(std::string_view v)
{
	for (std::string_view scheme: {"http://", "https://", "ftp://", "ftps://", "file://", "ws://", "wss://"})
		if (startsWithCi(v, scheme)) return true;
	return false;
}

bool CategoryDetector::isFilePath(std::string_view v)
{
	if (v.empty()) return false;
	if (v.front() == '/') return true;
	if (v.size() >= 3 && v[1] == ':' && (v[2] == '\\' || v[2] == '/')) return true;
	if (v.starts_with("\\\\")) return true;
	for (std::string_view p:
		 {"/proc/", "/sys/", "/dev/", "/etc/", "/usr/", "/var/", "C:\\", "C:/", "%SystemRoot%", "%AppData%", "%TEMP%"})
		if (startsWithCi(v, p)) return true;
	if (v.find('/') != std::string_view::npos || v.find('\\') != std::string_view::npos)
	{
		for (std::string_view e:
			 {".exe",
			  ".dll",
			  ".so",
			  ".dylib",
			  ".sys",
			  ".conf",
			  ".cfg",
			  ".ini",
			  ".log",
			  ".txt",
			  ".xml",
			  ".json",
			  ".yaml"})
			if (endsWithCi(v, e)) return true;
	}
	return false;
}

bool CategoryDetector::isRegEx(std::string_view v)
{
	static constexpr std::string_view kMeta = "\\^$.|?*+(){}[]";
	int count = 0;
	for (char c: v)
		if (kMeta.find(c) != std::string_view::npos) ++count;
	return count >= 3;
}

bool CategoryDetector::isCryptoConst(std::string_view v)
{
	static constexpr std::string_view kKeywords[] = {
		"SHA",
		"MD5",
		"AES",
		"RSA",
		"HMAC",
		"PBKDF",
		"BLAKE",
		"ChaCha",
		"Poly1305",
		"ECDSA",
		"secp256k1",
		"Curve25519",
		"BEGIN CERTIFICATE",
		"BEGIN RSA",
		"BEGIN PRIVATE KEY",
		"BEGIN PUBLIC KEY",
		"BEGIN EC",
		"BEGIN OPENSSH",
		"iv=",
		"salt=",
		"key=",
	};
	for (std::string_view kw: kKeywords)
		if (containsCi(v, kw)) return true;
	return false;
}

StringCategory CategoryDetector::classify(std::string_view value)
{
	if (isCryptoConst(value)) return StringCategory::CryptoConst;
	if (isURL(value)) return StringCategory::URL;
	if (isFilePath(value)) return StringCategory::FilePath;
	if (isFormatString(value)) return StringCategory::FormatString;
	if (isRegEx(value)) return StringCategory::RegEx;
	return StringCategory::Plain;
}

// ─── ConstantLabeler ─────────────────────────────────────────────────────────

namespace {

const std::unordered_map<uint32_t, const char*>& magic32()
{
	static const std::unordered_map<uint32_t, const char*> m = {
		{0x5A4D, "MZ (PE)"},
		{0x4D5A, "ZM (BE PE)"},
		{0x7F454C46, "ELF magic"},
		{0xFEEDFACE, "Mach-O 32-bit"},
		{0xCEFAEDFE, "Mach-O 32-bit (LE)"},
		{0xFEEDFACF, "Mach-O 64-bit"},
		{0xCFFAEDFE, "Mach-O 64-bit (LE)"},
		{0x89504E47, "PNG magic"},
		{0xFFD8FF, "JPEG magic"},
		{0x47494638, "GIF magic"},
		{0x504B0304, "ZIP magic"},
		{0x1F8B08, "gzip magic"},
		{0xFD377A58, "xz magic"},
		{0x52494646, "RIFF (WAV/AVI)"},
		{0x424D, "BMP magic"},
		{0x25504446, "PDF magic"},
		{0xD0CF11E0, "MS Compound Doc"},
		{0x377ABCAF, "7-Zip magic"},
		{0xCAFEBABE, "Java class / Mach-O fat"},
		{0xBEBAFECA, "Mach-O fat (LE)"},
		{0xDEADBEEF, "DEADBEEF"},
		{0xDEADC0DE, "DEADCODE"},
		{0xBADDCAFE, "BADDCAFE"},
		{0x0BADF00D, "BADFOOD"},
		{0xCCCCCCCC, "MSVC uninit stack"},
		{0xCDCDCDCD, "MSVC uninit heap"},
		{0xFEEEFEEE, "MSVC free heap"},
		{0x8BADF00D, "iOS watchdog"},
		{0xDEADA55, "DEAD A55"},
	};
	return m;
}

const std::unordered_map<uint32_t, const char*>& crypto32()
{
	static const std::unordered_map<uint32_t, const char*> m = {
		{0x428a2f98, "SHA256_K[0]"},     {0x71374491, "SHA256_K[1]"}, {0xb5c0fbcf, "SHA256_K[2]"},
		{0xe9b5dba5, "SHA256_K[3]"},     {0x3956c25b, "SHA256_K[4]"}, {0x59f111f1, "SHA256_K[5]"},
		{0x923f82a4, "SHA256_K[6]"},     {0xab1c5ed5, "SHA256_K[7]"}, {0xd807aa98, "SHA256_K[8]"},
		{0x12835b01, "SHA256_K[9]"},     {0x01000000, "AES Rcon[1]"}, {0x02000000, "AES Rcon[2]"},
		{0x04000000, "AES Rcon[3]"},     {0x08000000, "AES Rcon[4]"}, {0x10000000, "AES Rcon[5]"},
		{0x1B000000, "AES Rcon[6]"},     {0x36000000, "AES Rcon[7]"}, {0xEDB88320, "CRC32 poly (LE)"},
		{0x04C11DB7, "CRC32 poly (BE)"}, {0xd76aa478, "MD5_T[1]"},    {0xe8c7b756, "MD5_T[2]"},
		{5381, "DJB2 hash seed"},
	};
	return m;
}

const std::unordered_map<uint32_t, const char*>& float32()
{
	static const std::unordered_map<uint32_t, const char*> m = {
		{0x3F800000, "1.0f"},
		{0x40000000, "2.0f"},
		{0x3F000000, "0.5f"},
		{0x40490FDB, "\xCF\x80 (3.14159\xE2\x80\xA6)"},
		{0x402DF854, "e (2.71828\xE2\x80\xA6)"},
		{0x3FB504F3,
		 "\xE2\x88\x9A"
		 "2/2"},
		{0x3F3504F3,
		 "\xE2\x88\x9A"
		 "2-1"},
		{0x7F800000, "+Inf"},
		{0xFF800000, "-Inf"},
		{0x7FC00000, "NaN"},
		{0x00000000, "0.0f"},
		{0x80000000, "-0.0f"},
	};
	return m;
}

} // namespace

std::string ConstantLabeler::lookupMagic(uint64_t val, ConstantSize sz)
{
	if (sz == ConstantSize::Dword || sz == ConstantSize::Word)
	{
		auto it = magic32().find(static_cast<uint32_t>(val));
		if (it != magic32().end()) return it->second;
	}
	return {};
}

std::string ConstantLabeler::lookupCrypto(uint64_t val)
{
	auto it = crypto32().find(static_cast<uint32_t>(val));
	if (it != crypto32().end()) return it->second;
	return {};
}

std::string ConstantLabeler::lookupFloat(uint64_t val, ConstantSize sz)
{
	if (sz == ConstantSize::Dword)
	{
		auto it = float32().find(static_cast<uint32_t>(val));
		if (it != float32().end()) return it->second;
	}
	return {};
}

std::string ConstantLabeler::lookupPort(uint64_t val)
{
	static const std::unordered_map<uint32_t, const char*> ports = {
		{21, "FTP"},        {22, "SSH"},         {23, "Telnet"},     {25, "SMTP"},         {53, "DNS"},
		{80, "HTTP"},       {110, "POP3"},       {143, "IMAP"},      {443, "HTTPS"},       {465, "SMTPS"},
		{993, "IMAPS"},     {995, "POP3S"},      {3306, "MySQL"},    {5432, "PostgreSQL"}, {6379, "Redis"},
		{8080, "HTTP-Alt"}, {8443, "HTTPS-Alt"}, {27017, "MongoDB"}, {1433, "MSSQL"},      {1521, "Oracle"},
	};
	auto it = ports.find(static_cast<uint32_t>(val));
	if (it != ports.end()) return it->second;
	return {};
}

void ConstantLabeler::label(ConstantEntry& e)
{
	e.hexStr = std::format("{:0{}X}", e.value, static_cast<int>(e.size) * 2);
	e.decStr = std::to_string(e.value);

	std::string lbl = lookupMagic(e.value, e.size);
	if (!lbl.empty())
	{
		e.label = lbl;
		e.labelKind = ConstantLabel::MagicNumber;
		return;
	}
	lbl = lookupCrypto(e.value);
	if (!lbl.empty())
	{
		e.label = lbl;
		e.labelKind = ConstantLabel::CryptoConstant;
		return;
	}
	lbl = lookupFloat(e.value, e.size);
	if (!lbl.empty())
	{
		e.label = lbl;
		e.labelKind = ConstantLabel::FloatSpecial;
		return;
	}
	if (e.value > 0 && e.value <= 65535)
	{
		lbl = lookupPort(e.value);
		if (!lbl.empty())
		{
			e.label = lbl;
			e.labelKind = ConstantLabel::PortNumber;
			return;
		}
	}
	e.label.clear();
	e.labelKind = ConstantLabel::None;
}

std::string_view stringTypeName(StringType t)
{
	switch (t)
	{
	case StringType::ASCII: return "ASCII";
	case StringType::UTF8: return "UTF-8";
	case StringType::UTF16LE: return "UTF-16 LE";
	case StringType::UTF16BE: return "UTF-16 BE";
	case StringType::Wide: return "Wide";
	case StringType::Pascal: return "Pascal";
	default: return "?";
	}
}

std::string_view stringCategoryName(StringCategory c)
{
	switch (c)
	{
	case StringCategory::Plain: return "Plain";
	case StringCategory::FormatString: return "FormatStr";
	case StringCategory::URL: return "URL";
	case StringCategory::FilePath: return "FilePath";
	case StringCategory::RegEx: return "RegEx";
	case StringCategory::CryptoConst: return "Crypto";
	default: return "?";
	}
}

std::string_view constantLabelKindName(ConstantLabel k)
{
	switch (k)
	{
	case ConstantLabel::None: return "All";
	case ConstantLabel::MagicNumber: return "Magic";
	case ConstantLabel::CryptoConstant: return "Crypto";
	case ConstantLabel::FloatSpecial: return "Float";
	case ConstantLabel::PortNumber: return "Port";
	case ConstantLabel::Errno: return "Errno";
	case ConstantLabel::WinError: return "WinError";
	case ConstantLabel::SyscallNr: return "Syscall";
	case ConstantLabel::OtherKnown: return "Other";
	}
	return "?";
}

} // namespace retdec::gui::model
