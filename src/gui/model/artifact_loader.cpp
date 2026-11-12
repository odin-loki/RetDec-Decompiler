/**
 * @file src/gui/model/artifact_loader.cpp
 * @brief Decompiler sidecar loading (config.json, .dsm, .ll, .c).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/artifact_loader.h"

#include "retdec/gui/model/address.h"

#include <forge/os/os.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <format>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace retdec::gui::model {

namespace json = forge::json;
namespace fs = std::filesystem;

namespace {

// ─── Text helpers ────────────────────────────────────────────────────────────

bool isSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool isWordChar(char c)
{
	const auto u = static_cast<unsigned char>(c);
	return std::isalnum(u) || c == '_';
}

bool isHexDigit(char c)
{
	return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

std::string_view trimView(std::string_view s)
{
	while (!s.empty() && isSpace(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && isSpace(s.back()))
		s.remove_suffix(1);
	return s;
}

std::string lower(std::string_view s)
{
	std::string out(s);
	for (char& c: out)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

bool contains(std::string_view hay, std::string_view needle)
{
	return hay.find(needle) != std::string_view::npos;
}

/// Split at '\n' (like QString::split('\n')): n newlines -> n+1 pieces.
std::vector<std::string_view> splitLines(std::string_view text)
{
	std::vector<std::string_view> out;
	std::size_t start = 0;
	while (true)
	{
		const std::size_t nl = text.find('\n', start);
		if (nl == std::string_view::npos)
		{
			out.push_back(text.substr(start));
			break;
		}
		out.push_back(text.substr(start, nl - start));
		start = nl + 1;
	}
	return out;
}

std::string join(const std::vector<std::string_view>& lines)
{
	std::string out;
	for (std::size_t i = 0; i < lines.size(); ++i)
	{
		if (i) out += '\n';
		out += lines[i];
	}
	return out;
}

std::size_t countOverlapping(std::string_view hay, std::string_view needle)
{
	if (needle.empty()) return 0;
	std::size_t n = 0;
	for (std::size_t p = hay.find(needle); p != std::string_view::npos; p = hay.find(needle, p + 1))
		++n;
	return n;
}

std::size_t utf8Length(std::string_view s)
{
	std::size_t n = 0;
	for (unsigned char c: s)
		if ((c & 0xC0) != 0x80) ++n;
	return n;
}

std::string readTextFileIfExists(const fs::path& path)
{
	if (path.empty()) return {};
	auto r = forge::os::read_file(path.string());
	return r ? std::move(r.value) : std::string{};
}

/// "0xHEX:" at the start of @p line (already trimmed as the caller wants):
/// the address and the position after the colon.
std::optional<std::pair<uint64_t, std::size_t>> leadingInstrAddress(std::string_view line)
{
	if (line.size() < 4 || line[0] != '0' || line[1] != 'x') return std::nullopt;
	std::size_t j = 2;
	while (j < line.size() && isHexDigit(line[j]))
		++j;
	if (j == 2 || j >= line.size() || line[j] != ':') return std::nullopt;
	const auto a = parseAddress(line.substr(0, j));
	if (!a) return std::nullopt;
	return std::make_pair(*a, j + 1);
}

// ─── JSON helpers (missing / wrong type -> empty, like QJsonValue) ──────────

const json::value& emptyValue()
{
	static const json::value v;
	return v;
}

const json::array& emptyArray()
{
	static const json::array a;
	return a;
}

const json::value& member(const json::value& o, std::string_view key)
{
	const json::value* v = o.find(key);
	return v ? *v : emptyValue();
}

std::string str(const json::value& o, std::string_view key)
{
	const json::value& v = member(o, key);
	return v.is_str() ? v.str() : std::string{};
}

const json::array& arr(const json::value& o, std::string_view key)
{
	const json::value& v = member(o, key);
	return v.is_arr() ? v.arr() : emptyArray();
}

double dbl(const json::value& o, std::string_view key, double dflt)
{
	const json::value& v = member(o, key);
	return v.is_num() ? v.num() : dflt;
}

bool boolean(const json::value& o, std::string_view key, bool dflt)
{
	const json::value& v = member(o, key);
	return v.is_bool() ? v.b() : dflt;
}

std::optional<uint64_t> addr(const json::value& o, std::string_view key)
{
	const json::value& v = member(o, key);
	if (!v.is_str()) return std::nullopt;
	return parseAddress(v.str());
}

/// Line numbers are strings in the config ("12"); numbers are accepted too.
std::optional<int> lineNumber(const json::value& o, std::string_view key)
{
	const json::value& v = member(o, key);
	if (v.is_num()) return static_cast<int>(v.num());
	if (!v.is_str()) return std::nullopt;
	const auto a = parseAddress(v.str());
	if (!a || *a > 0x7fffffffu) return std::nullopt;
	return static_cast<int>(*a);
}

std::optional<uint64_t> jsonInt(const json::value& v)
{
	if (v.is_str()) return parseAddress(v.str());
	if (v.is_num())
	{
		const double d = v.num();
		if (!std::isfinite(d) || d < 0) return std::nullopt;
		return static_cast<uint64_t>(std::llround(d));
	}
	return std::nullopt;
}

// ─── Constant labelling (the Qt ConstantLabeler) ─────────────────────────────

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
		{0x40490FDB, "π (3.14159…)"},
		{0x402DF854, "e (2.71828…)"},
		{0x3FB504F3, "√2/2"},
		{0x3F3504F3, "√2-1"},
		{0x7F800000, "+Inf"},
		{0xFF800000, "-Inf"},
		{0x7FC00000, "NaN"},
		{0x00000000, "0.0f"},
		{0x80000000, "-0.0f"},
	};
	return m;
}

const std::unordered_map<uint32_t, const char*>& ports()
{
	static const std::unordered_map<uint32_t, const char*> m = {
		{21, "FTP"},        {22, "SSH"},         {23, "Telnet"},     {25, "SMTP"},         {53, "DNS"},
		{80, "HTTP"},       {110, "POP3"},       {143, "IMAP"},      {443, "HTTPS"},       {465, "SMTPS"},
		{993, "IMAPS"},     {995, "POP3S"},      {3306, "MySQL"},    {5432, "PostgreSQL"}, {6379, "Redis"},
		{8080, "HTTP-Alt"}, {8443, "HTTPS-Alt"}, {27017, "MongoDB"}, {1433, "MSSQL"},      {1521, "Oracle"},
	};
	return m;
}

void labelConstant(ConstantEntry& e)
{
	e.hexStr = std::format("{:0{}X}", e.value, static_cast<int>(e.size) * 2);
	e.decStr = std::to_string(e.value);

	const auto v32 = static_cast<uint32_t>(e.value);
	auto set = [&](const char* lbl, ConstantLabel kind) {
		e.label = lbl;
		e.labelKind = kind;
	};
	if (e.size == ConstantSize::Dword || e.size == ConstantSize::Word)
		if (auto it = magic32().find(v32); it != magic32().end()) return set(it->second, ConstantLabel::MagicNumber);
	if (auto it = crypto32().find(v32); it != crypto32().end()) return set(it->second, ConstantLabel::CryptoConstant);
	if (e.size == ConstantSize::Dword)
		if (auto it = float32().find(v32); it != float32().end()) return set(it->second, ConstantLabel::FloatSpecial);
	if (e.value > 0 && e.value <= 65535)
		if (auto it = ports().find(v32); it != ports().end()) return set(it->second, ConstantLabel::PortNumber);
	e.label.clear();
	e.labelKind = ConstantLabel::None;
}

// ─── config.json parsers ─────────────────────────────────────────────────────

std::vector<SemanticDetection> parseSemanticDetections(const json::array& fnArr)
{
	std::vector<SemanticDetection> out;
	for (const json::value& fo: fnArr)
	{
		if (!fo.is_obj()) continue;
		const std::string fnName = str(fo, "name");
		if (fnName.empty()) continue;
		for (const json::value& d: arr(fo, "semanticDetections"))
		{
			if (!d.is_obj()) continue;
			SemanticDetection e;
			e.function = fnName;
			e.kind = str(d, "kind");
			e.label = str(d, "label");
			e.confidence = dbl(d, "confidence", 0.0);
			e.detail = str(d, "detail");
			if (e.label.empty()) continue;
			out.push_back(std::move(e));
		}
	}
	return out;
}

bool isLinkedLibrary(const std::string& fncType)
{
	return fncType == "staticallyLinked" || fncType == "dynamicallyLinked";
}

std::vector<FunctionEntry> parseFunctions(const json::array& fnArr, const RecoverySettings& rec)
{
	std::vector<FunctionEntry> entries;
	entries.reserve(fnArr.size());
	for (const json::value& o: fnArr)
	{
		if (!o.is_obj()) continue;
		FunctionEntry e;
		e.name = str(o, "name");
		e.rawName = str(o, "demangledName");
		e.address = addr(o, "startAddr").value_or(0);
		if (const auto end = addr(o, "endAddr"); end && *end > e.address)
			e.sizeBytes = static_cast<int>(*end - e.address);
		e.cc = str(o, "callingConvention");
		e.isLibrary = isLinkedLibrary(str(o, "fncType"));
		e.patterns.library = e.isLibrary;

		for (const json::value& d: arr(o, "semanticDetections"))
		{
			if (!d.is_obj()) continue;
			std::string k = lower(str(d, "kind"));
			if (k.empty()) k = lower(str(d, "label"));
			const double conf = dbl(d, "confidence", 0.0);
			const auto belowFloor = [conf](double floor) { return conf > 0.0 && conf < floor; };

			if (contains(k, "stl") || contains(k, "container"))
				if (!belowFloor(rec.stlConfidence)) e.patterns.stl = true;
			if (contains(k, "crypto") || contains(k, "cipher") || contains(k, "hash"))
				if (!belowFloor(rec.cryptoConfidence)) e.patterns.crypto = true;
			if (contains(k, "pattern") || contains(k, "idiom") || contains(k, "algo"))
				if (!belowFloor(rec.patternConfidence)) e.patterns.algo = true;
			if (contains(k, "virtual") || contains(k, "vtable") || contains(k, "singleton") || contains(k, "factory")
				|| contains(k, "rtti"))
				e.patterns.design = true;
			if (contains(k, "library")) e.patterns.library = true;
		}

		if (const auto sl = lineNumber(o, "startLine"); sl && *sl > 0) e.startLine = *sl;
		if (const auto el = lineNumber(o, "endLine"); el && *el > 0) e.endLine = *el;
		entries.push_back(std::move(e));
	}
	return entries;
}

struct GlobalType
{
	std::string llvmIr;
	bool wide = false;
};

GlobalType globalType(const json::value& o)
{
	const json::value& t = member(o, "type");
	return GlobalType{str(t, "llvmIr"), boolean(t, "isWideString", false)};
}

std::vector<StringEntry> parseStringGlobals(const json::array& globals)
{
	std::vector<StringEntry> out;
	for (const json::value& o: globals)
	{
		if (!o.is_obj()) continue;
		const GlobalType t = globalType(o);
		if (!contains(t.llvmIr, "i8") && !t.wide) continue;

		StringEntry e;
		e.address = addr(member(o, "storage"), "value").value_or(0);
		e.value = str(o, "realName");
		if (e.value.empty()) e.value = str(o, "name");
		e.type = t.wide ? StringType::Wide : StringType::ASCII;
		e.length = static_cast<int>(utf8Length(e.value));
		out.push_back(std::move(e));
	}
	return out;
}

std::vector<ConstantEntry> parseConstantGlobals(const json::array& globals)
{
	std::vector<ConstantEntry> out;
	for (const json::value& o: globals)
	{
		if (!o.is_obj()) continue;
		const GlobalType t = globalType(o);
		if (contains(t.llvmIr, "i8") || t.wide) continue;

		ConstantEntry e;
		const json::value& storage = member(o, "storage");
		e.address = addr(storage, "value").value_or(0);

		if (contains(t.llvmIr, "i64"))
			e.size = ConstantSize::Qword;
		else if (contains(t.llvmIr, "i32"))
			e.size = ConstantSize::Dword;
		else if (contains(t.llvmIr, "i16"))
			e.size = ConstantSize::Word;
		else
			e.size = ConstantSize::Dword;

		std::optional<uint64_t> v = jsonInt(member(o, "value"));
		if (!v) v = jsonInt(member(o, "init"));
		if (!v) v = jsonInt(member(o, "integer"));
		if (!v) v = jsonInt(member(storage, "init"));
		if (!v) v = jsonInt(member(storage, "integer"));
		e.value = v.value_or(0);

		labelConstant(e);
		if (e.address != 0 || e.value != 0) out.push_back(std::move(e));
	}
	return out;
}

void fillBlockInstructionsFromDsm(std::vector<BasicBlockData>& blocks, std::string_view fullDsm)
{
	if (fullDsm.empty()) return;
	for (auto& bb: blocks)
	{
		if (bb.endAddress <= bb.address) continue;
		bb.instrs = extractDsmInstructionsForRange(fullDsm, bb.address, bb.endAddress);
	}
}

struct VtableParseData
{
	uint64_t address = 0;
	std::vector<VtableSlot> slots;
};

std::unordered_map<std::string, VtableParseData> parseVtables(const json::array& vtArr)
{
	std::unordered_map<std::string, VtableParseData> byName;
	for (const json::value& vo: vtArr)
	{
		if (!vo.is_obj()) continue;
		const std::string name = str(vo, "name");
		if (name.empty()) continue;

		VtableParseData data;
		data.address = addr(vo, "address").value_or(0);
		int idx = 0;
		for (const json::value& io: arr(vo, "items"))
		{
			if (!io.is_obj()) continue;
			VtableSlot slot;
			slot.index = idx++;
			slot.funcName = str(io, "targetName");
			slot.funcAddress = addr(io, "targetAddress").value_or(0);
			slot.isPure = (slot.funcAddress == 0);
			data.slots.push_back(std::move(slot));
		}
		byName.insert_or_assign(name, std::move(data));
	}
	return byName;
}

std::vector<ClassInfo> parseTypeHierarchyClasses(const json::value& config)
{
	const auto vtables = parseVtables(arr(config, "vtables"));

	std::vector<ClassInfo> classes;
	const json::array& classArr = arr(config, "classes");
	classes.reserve(classArr.size());
	for (const json::value& co: classArr)
	{
		if (!co.is_obj()) continue;
		ClassInfo info;
		const std::string demangled = str(co, "demangledName");
		info.name = !demangled.empty() ? demangled : str(co, "name");
		if (info.name.empty()) continue;

		for (const json::value& sv: arr(co, "superClasses"))
		{
			if (!sv.is_str() || sv.str().empty()) continue;
			InheritanceLink link;
			link.base = sv.str();
			link.kind = InheritanceLink::Kind::Public;
			info.bases.push_back(std::move(link));
		}

		int methodCount = 0;
		for (const char* key: {"constructors", "destructors", "methods", "virtualMethods"})
			methodCount += static_cast<int>(arr(co, key).size());
		info.methodCount = methodCount;

		for (const json::value& vtNameVal: arr(co, "virtualTables"))
		{
			if (!vtNameVal.is_str()) continue;
			const auto vtIt = vtables.find(vtNameVal.str());
			if (vtIt == vtables.end()) continue;
			if (info.vtableAddress == 0) info.vtableAddress = vtIt->second.address;
			if (info.vtable.empty()) info.vtable = vtIt->second.slots;
			for (const auto& slot: vtIt->second.slots)
			{
				if (slot.isPure)
				{
					info.isAbstract = true;
					break;
				}
			}
		}
		classes.push_back(std::move(info));
	}
	return classes;
}

void buildCallGraph(const json::array& fnArr, std::vector<CallGraphNode>& nodes, std::vector<CallEdge>& edges)
{
	nodes.clear();
	edges.clear();
	std::unordered_map<uint64_t, std::size_t> nodeIndex;

	auto ensureNode = [&](uint64_t a, const std::string& name, bool isLib, int instrCount) {
		if (nodeIndex.contains(a)) return;
		CallGraphNode n;
		n.address = a;
		n.name = name.empty() ? std::format("sub_{:x}", a) : name;
		n.isLibrary = isLib;
		n.instrCount = instrCount;
		nodeIndex.emplace(a, nodes.size());
		nodes.push_back(std::move(n));
	};

	for (const json::value& fo: fnArr)
	{
		if (!fo.is_obj()) continue;
		const std::string fname = str(fo, "name");
		const uint64_t start = addr(fo, "startAddr").value_or(0);
		const auto end = addr(fo, "endAddr");
		const int instrEst = (end && *end > start) ? static_cast<int>((*end - start) / 4) : 1;
		ensureNode(start, fname, isLinkedLibrary(str(fo, "fncType")), instrEst);

		for (const json::value& bv: arr(fo, "basicBlocks"))
		{
			if (!bv.is_obj()) continue;
			for (const json::value& cv: arr(bv, "calls"))
			{
				if (!cv.is_obj()) continue;
				const uint64_t tgt = addr(cv, "targetAddr").value_or(0);
				if (tgt == 0) continue;
				ensureNode(tgt, {}, false, 1);
				CallEdge e;
				e.callerAddress = start;
				e.calleeAddress = tgt;
				e.isDirect = true;
				e.callCount = 1;
				edges.push_back(e);
			}
		}
	}
}

void buildCfgMaps(
	const json::array& fnArr,
	std::unordered_map<uint64_t, std::vector<BasicBlockData>>& blocks,
	std::unordered_map<uint64_t, std::vector<CFGEdgeData>>& edges)
{
	blocks.clear();
	edges.clear();
	for (const json::value& fo: fnArr)
	{
		if (!fo.is_obj()) continue;
		const auto fnStart = addr(fo, "startAddr");
		if (!fnStart || *fnStart == 0) continue;

		std::vector<BasicBlockData> fnBlocks;
		std::vector<CFGEdgeData> fnEdges;
		for (const json::value& bo: arr(fo, "basicBlocks"))
		{
			if (!bo.is_obj()) continue;
			const auto start = addr(bo, "startAddr");
			if (!start) continue;
			BasicBlockData bb;
			bb.address = *start;
			bb.id = bb.address;
			if (const auto bbEnd = addr(bo, "endAddr"); bbEnd && *bbEnd > bb.address) bb.endAddress = *bbEnd;

			for (const json::value& sv: arr(bo, "succs"))
			{
				const uint64_t to = sv.is_str() ? parseAddress(sv.str()).value_or(0) : 0;
				if (to == 0) continue;
				fnEdges.push_back(CFGEdgeData{bb.address, to, EdgeKind::FallThrough});
			}
			fnBlocks.push_back(std::move(bb));
		}
		if (!fnBlocks.empty())
		{
			blocks[*fnStart] = std::move(fnBlocks);
			edges[*fnStart] = std::move(fnEdges);
		}
	}
}

constexpr std::size_t kMaxDsmScanChars = 2 * 1024 * 1024;

void fillInstrCountsFromDsm(std::vector<FunctionEntry>& functions, std::string_view fullDsm)
{
	if (fullDsm.empty() || fullDsm.size() > kMaxDsmScanChars) return;

	// Instruction lines only ("0xADDR: ..."). The Qt version parsed the whole
	// rest of the line as a number, which never succeeded on a real .dsm line
	// and left every count at zero.
	std::vector<uint64_t> lineAddrs;
	for (std::string_view line: splitLines(fullDsm))
		if (const auto a = leadingInstrAddress(trimView(line))) lineAddrs.push_back(a->first);

	for (auto& fn: functions)
	{
		if (fn.instrCount != 0 || fn.address == 0 || fn.sizeBytes <= 0) continue;
		const uint64_t lo = fn.address;
		const uint64_t hi = fn.address + static_cast<uint64_t>(fn.sizeBytes);
		fn.instrCount = static_cast<int>(
			std::count_if(lineAddrs.begin(), lineAddrs.end(), [&](uint64_t a) { return a >= lo && a < hi; }));
	}
}

int countDsmAddrRefs(std::string_view fullDsm, uint64_t a)
{
	const std::string hex = std::format("{:x}", a);
	return static_cast<int>(countOverlapping(fullDsm, "0x" + hex) + countOverlapping(fullDsm, hex));
}

void fillRefCountsFromDsm(
	std::vector<StringEntry>& strings, std::vector<ConstantEntry>& constants, std::string_view fullDsm)
{
	if (fullDsm.empty() || fullDsm.size() > kMaxDsmScanChars) return;
	for (auto& e: strings)
		if (e.address != 0) e.refCount = countDsmAddrRefs(fullDsm, e.address);
	for (auto& e: constants)
		if (e.address != 0) e.refCount = countDsmAddrRefs(fullDsm, e.address);
}

std::string readLineRangeFromFile(const fs::path& path, int startLine, int endLine)
{
	if (startLine < 1 || path.empty()) return {};
	auto r = forge::os::read_file(path.string());
	if (!r) return {};
	const std::string& text = r.value;

	std::vector<std::string_view> out;
	int line = 0;
	std::size_t pos = 0;
	while (pos < text.size())
	{
		std::size_t nl = text.find('\n', pos);
		const std::size_t end = nl == std::string::npos ? text.size() : nl;
		std::string_view l(text.data() + pos, end - pos);
		if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
		++line;
		pos = nl == std::string::npos ? text.size() : nl + 1;
		if (line < startLine) continue;
		if (endLine > 0 && line > endLine) break;
		out.push_back(l);
	}
	return join(out);
}

/// Position just past the ')' matching the '(' at @p open, or npos.
std::size_t matchParen(std::string_view s, std::size_t open)
{
	int depth = 0;
	for (std::size_t i = open; i < s.size(); ++i)
	{
		if (s[i] == '(')
			++depth;
		else if (s[i] == ')' && --depth == 0)
			return i + 1;
	}
	return std::string_view::npos;
}

} // namespace

// ─── Public API ──────────────────────────────────────────────────────────────

DecompileArtifactPaths pathsFromOutputC(const fs::path& cPath)
{
	DecompileArtifactPaths p;
	p.cPath = cPath;
	std::string stem = cPath.string();
	if (stem.size() >= 2 && stem[stem.size() - 2] == '.' && (stem.back() == 'c' || stem.back() == 'C'))
		stem.resize(stem.size() - 2);
	p.configPath = fs::path(stem + ".config.json");
	p.dsmPath = fs::path(stem + ".dsm");
	p.llPath = fs::path(stem + ".ll");
	return p;
}

std::expected<DecompileArtifacts, std::string> loadDecompileArtifactsFromConfigJson(
	std::string_view configJson, const DecompileArtifactPaths& paths, const RecoverySettings& recovery)
{
	DecompileArtifacts out;
	out.cPath = paths.cPath;
	try
	{
		out.config = json::parse(configJson);
	}
	catch (const std::exception&)
	{
		return std::unexpected(std::format("Invalid config JSON: {}", paths.configPath.string()));
	}
	if (!out.config.is_obj()) return std::unexpected(std::format("Invalid config JSON: {}", paths.configPath.string()));

	const json::array& fnArr = arr(out.config, "functions");
	const json::array& globals = arr(out.config, "globals");
	out.functions = parseFunctions(fnArr, recovery);
	out.strings = parseStringGlobals(globals);
	out.constants = parseConstantGlobals(globals);
	out.semanticDetections = parseSemanticDetections(fnArr);
	buildCallGraph(fnArr, out.callGraphNodes, out.callGraphEdges);
	buildCfgMaps(fnArr, out.cfgBlocks, out.cfgEdges);
	out.typeHierarchyClasses = parseTypeHierarchyClasses(out.config);

	out.fullDsm = readTextFileIfExists(paths.dsmPath);
	out.fullLl = readTextFileIfExists(paths.llPath);

	fillInstrCountsFromDsm(out.functions, out.fullDsm);
	fillRefCountsFromDsm(out.strings, out.constants, out.fullDsm);
	for (auto& entry: out.cfgBlocks)
		fillBlockInstructionsFromDsm(entry.second, out.fullDsm);
	return out;
}

std::expected<DecompileArtifacts, std::string>
loadDecompileArtifactsFromPaths(const DecompileArtifactPaths& paths, const RecoverySettings& recovery)
{
	auto cfg = forge::os::read_file(paths.configPath.string());
	if (!cfg) return std::unexpected(std::format("Missing config: {}", paths.configPath.string()));
	return loadDecompileArtifactsFromConfigJson(cfg.value, paths, recovery);
}

std::vector<BlockInstr> extractDsmInstructionsForRange(std::string_view fullDsm, uint64_t startAddr, uint64_t endAddr)
{
	std::vector<BlockInstr> instrs;
	if (fullDsm.empty() || endAddr <= startAddr) return instrs;
	for (std::string_view raw: splitLines(fullDsm))
	{
		const std::string_view line = trimView(raw);
		const auto m = leadingInstrAddress(line);
		if (!m) continue;
		if (m->first < startAddr || m->first >= endAddr) continue;
		instrs.push_back(BlockInstr{m->first, std::string(trimView(line.substr(m->second)))});
	}
	return instrs;
}

std::string
extractDsmForFunction(std::string_view fullDsm, uint64_t startAddr, uint64_t endAddr, std::string_view funcName)
{
	if (fullDsm.empty()) return {};

	static const std::regex fnRe(R"(; function:\s*(.+?)\s+at\s+(0x[0-9a-fA-F]+)\s+--\s+(0x[0-9a-fA-F]+))");

	std::vector<std::string_view> out;
	bool inFn = false;
	const std::vector<std::string_view> lines = splitLines(fullDsm);
	for (std::string_view line: lines)
	{
		std::match_results<std::string_view::const_iterator> m;
		if (contains(line, "; function:") && std::regex_search(line.begin(), line.end(), m, fnRe))
		{
			const std::string_view name =
				trimView(std::string_view(&*m[1].first, static_cast<std::size_t>(m[1].length())));
			const uint64_t s =
				parseAddress(std::string_view(&*m[2].first, static_cast<std::size_t>(m[2].length()))).value_or(0);
			inFn = (s == startAddr) || (!funcName.empty() && name == funcName);
			if (inFn)
			{
				out.clear();
				out.push_back(line);
			}
			continue;
		}
		if (inFn)
		{
			if (line.starts_with("; function:")) break;
			out.push_back(line);
		}
	}

	if (out.empty())
	{
		for (std::string_view line: lines)
		{
			const auto m = leadingInstrAddress(line);
			if (!m) continue;
			if (m->first >= startAddr && (endAddr == 0 || m->first < endAddr)) out.push_back(line);
		}
	}
	return join(out);
}

std::string extractLlvmForFunction(std::string_view fullLl, std::string_view funcName)
{
	if (fullLl.empty() || funcName.empty()) return {};

	std::size_t start = fullLl.find(std::string("define ") + std::string(funcName));
	if (start == std::string_view::npos)
	{
		// LLVM prints "define dso_local i32 @main(i32 %argc) {": find the
		// define line that names @funcName( and start at its beginning.
		const std::string needle = "@" + std::string(funcName) + "(";
		for (std::size_t p = fullLl.find(needle); p != std::string_view::npos; p = fullLl.find(needle, p + 1))
		{
			const std::size_t lineStart = fullLl.rfind('\n', p);
			const std::size_t ls = lineStart == std::string_view::npos ? 0 : lineStart + 1;
			if (fullLl.substr(ls, p - ls).starts_with("define "))
			{
				start = ls;
				break;
			}
		}
	}
	if (start == std::string_view::npos) return {};

	int depth = 0;
	std::size_t i = start;
	for (; i < fullLl.size(); ++i)
	{
		const char c = fullLl[i];
		if (c == '{')
			++depth;
		else if (c == '}')
		{
			--depth;
			if (depth == 0)
			{
				++i;
				break;
			}
		}
	}
	return std::string(fullLl.substr(start, i - start));
}

std::string extractCForFunctionByName(std::string_view fullC, std::string_view funcName)
{
	if (fullC.empty() || funcName.empty()) return {};

	// What the Qt regex (?:^|\n)([\w\s\*]+?\bNAME\s*\() matched, without a
	// backtracking regex over the whole file: for each "NAME (" preceded by a
	// word boundary, the match starts at the earliest line start reachable
	// backwards through [\w\s*] characters. Definitions ("...) {") are
	// preferred over prototypes ("...);"), which the Qt version would return
	// together with everything up to the next function body.
	struct Candidate
	{
		std::size_t begin;
		bool definition;
	};
	std::vector<Candidate> candidates;
	const bool nameStartsWord = isWordChar(funcName.front());
	for (std::size_t p = fullC.find(funcName); p != std::string_view::npos; p = fullC.find(funcName, p + 1))
	{
		if (p == 0) continue; // the group before the name needs at least one character
		if (nameStartsWord && isWordChar(fullC[p - 1])) continue;
		if (!nameStartsWord && !isWordChar(fullC[p - 1])) continue;
		std::size_t q = p + funcName.size();
		while (q < fullC.size() && isSpace(fullC[q]))
			++q;
		if (q >= fullC.size() || fullC[q] != '(') continue;

		std::size_t run = p;
		while (run > 0)
		{
			const char c = fullC[run - 1];
			if (isWordChar(c) || isSpace(c) || c == '*')
				--run;
			else
				break;
		}
		std::size_t begin = std::string_view::npos;
		for (std::size_t s = run; s < p; ++s)
			if (s == 0 || fullC[s - 1] == '\n')
			{
				begin = s;
				break;
			}
		if (begin == std::string_view::npos) continue;

		bool definition = false;
		if (const std::size_t close = matchParen(fullC, q); close != std::string_view::npos)
		{
			std::size_t k = close;
			while (k < fullC.size() && isSpace(fullC[k]))
				++k;
			definition = k < fullC.size() && fullC[k] != ';';
		}
		candidates.push_back(Candidate{begin, definition});
	}
	if (candidates.empty()) return {};

	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		return a.begin < b.begin;
	});
	auto chosen = std::find_if(candidates.begin(), candidates.end(), [](const Candidate& c) { return c.definition; });
	if (chosen == candidates.end()) chosen = candidates.begin();

	const std::size_t bodyStart = chosen->begin;
	int depth = 0;
	bool seenBrace = false;
	std::size_t i = bodyStart;
	for (; i < fullC.size(); ++i)
	{
		const char c = fullC[i];
		if (c == '{')
		{
			++depth;
			seenBrace = true;
		}
		else if (c == '}' && seenBrace)
		{
			--depth;
			if (depth == 0)
			{
				++i;
				break;
			}
		}
	}
	return std::string(trimView(fullC.substr(bodyStart, i - bodyStart)));
}

std::string extractCForFunction(const fs::path& cPath, int startLine, int endLine, std::string_view funcName)
{
	if (startLine > 0) return readLineRangeFromFile(cPath, startLine, endLine);
	if (funcName.empty() || cPath.empty()) return {};
	return extractCForFunctionByName(readTextFileIfExists(cPath), funcName);
}

std::vector<FunctionLineMapEntry>
buildIdentityLineMapping(std::string_view asmText, std::string_view irText, std::string_view cText)
{
	const auto lineCount = [](std::string_view text) -> int {
		if (text.empty()) return 0;
		return static_cast<int>(std::count(text.begin(), text.end(), '\n') + 1);
	};
	const int n = std::min({lineCount(asmText), lineCount(irText), lineCount(cText)});
	std::vector<FunctionLineMapEntry> mapping;
	mapping.reserve(static_cast<std::size_t>(n));
	for (int i = 1; i <= n; ++i)
		mapping.push_back(FunctionLineMapEntry{i, i, i});
	return mapping;
}

FunctionViewData
buildFunctionViewData(const DecompileArtifacts& art, uint64_t funcAddr, std::string_view funcName, bool includeC)
{
	FunctionViewData v;
	v.address = funcAddr;
	v.name = std::string(funcName);
	for (const auto& f: art.functions)
	{
		if (f.address == funcAddr)
		{
			v.endAddress = f.address + static_cast<uint64_t>(std::max(f.sizeBytes, 1));
			v.startLine = f.startLine;
			v.endLine = f.endLine;
			break;
		}
	}

	v.assemblyText = extractDsmForFunction(art.fullDsm, funcAddr, v.endAddress, funcName);
	v.irText = extractLlvmForFunction(art.fullLl, funcName);
	if (includeC)
	{
		v.cText = extractCForFunction(art.cPath, v.startLine, v.endLine, funcName);
		v.lineMapping = buildIdentityLineMapping(v.assemblyText, v.irText, v.cText);
	}

	if (const auto bIt = art.cfgBlocks.find(funcAddr); bIt != art.cfgBlocks.end() && !bIt->second.empty())
	{
		v.cfgBlocks = bIt->second;
		if (v.cfgBlocks.front().instrs.empty()) fillBlockInstructionsFromDsm(v.cfgBlocks, art.fullDsm);
		if (const auto eIt = art.cfgEdges.find(funcAddr); eIt != art.cfgEdges.end()) v.cfgEdges = eIt->second;
	}
	return v;
}

} // namespace retdec::gui::model
