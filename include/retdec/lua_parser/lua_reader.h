/**
 * @file include/retdec/lua_parser/lua_reader.h
 * @brief Lua compiled bytecode (.luac) reader.
 *
 * Parses Lua 5.1, 5.2, 5.3, and 5.4 bytecode files produced by luac.
 * Reconstructs the full prototype tree with constants, upvalues, nested
 * functions, locals, and line-number debug info.
 */

#ifndef RETDEC_LUA_PARSER_LUA_READER_H
#define RETDEC_LUA_PARSER_LUA_READER_H

#include "retdec/lua_parser/lua_types.h"

#include <string>
#include <vector>

namespace retdec {
namespace lua_parser {

struct LuaReadResult
{
	bool ok = false;
	std::string error;
	LuaModule module;
	std::vector<std::string> warnings;
};

class LuaReader {
public:
	explicit LuaReader(std::vector<uint8_t> bytes);
	explicit LuaReader(const uint8_t* data, size_t size);

	LuaReadResult read();

private:
	std::vector<uint8_t> data_;
	size_t pos_ = 0;
	LuaVersion ver_ = LuaVersion::Unknown;
	bool le_ = true; // little-endian
	int intSz_ = 4;
	int sizetSz_ = 8;
	bool useLeb128_ = false; // Lua 5.4.6+ uses modified LEB128 for ints/sizes
	std::vector<std::string> warnings_;
	// String table for Lua 5.4 string deduplication (index → string)
	std::vector<std::string> stringTable54_;
	/// Prototype nesting depth currently being read; see kMaxProtoDepth.
	unsigned protoDepth_ = 0;

	struct ParseError
	{
		std::string msg;
	};
	void warn(const std::string& msg)
	{
		warnings_.push_back(msg);
	}

	// ── Bounds helpers ───────────────────────────────────────────────────────
	// Bytes of input the file still has to offer at the current position.
	size_t remaining() const
	{
		return pos_ < data_.size() ? data_.size() - pos_ : 0;
	}
	// Validate a count/length that came out of the file against what the rest
	// of the file could actually supply. See lua_reader.cpp for the reasoning.
	size_t checkedSize(size_t n, size_t minBytesPerElem, const char* what);
	size_t checkedCount(int32_t n, size_t minBytesPerElem, const char* what);

	// ── Primitives ────────────────────────────────────────────────────────────
	uint8_t readU8();
	uint32_t readU32();
	uint64_t readU64();
	int32_t readInt(); // fixed-size or LEB128 depending on useLeb128_
	int64_t readLuaInt();
	double readLuaFloat();
	size_t readSizet();
	size_t readLuaSize54();     // Lua 5.4 modified LEB128 (MSB=1 → last byte)
	std::string readString51(); // Lua 5.1/5.2 Pascal-style string
	std::string readString53(); // Lua 5.3+ ULEB-length string
	std::string readString54(); // Lua 5.4.6 modified LEB128 + dedup table
	std::string readString();   // dispatches by version

	// ── Header ────────────────────────────────────────────────────────────────
	LuaVersion parseHeader();

	// ── Prototype parsers ────────────────────────────────────────────────────
	/// A prototype's sub-prototypes are read by recursing into the same
	/// function, so the nesting a file declares becomes native stack depth.
	/// Nesting costs about ten bytes per level on the wire and a few hundred
	/// on the stack, so a file of a few hundred kilobytes used to be enough to
	/// run the stack out. Lua's own compiler cannot emit nesting deeper than
	/// LUAI_MAXCCALLS (200), so no file this refuses was produced by luac.
	static constexpr unsigned kMaxProtoDepth = 200;
	/// Raises protoDepth_ for the duration of one readProto5x() call and
	/// refuses the file once the nesting passes kMaxProtoDepth.
	class ProtoDepthGuard {
	public:
		explicit ProtoDepthGuard(LuaReader& r);
		~ProtoDepthGuard();

	private:
		LuaReader& r_;
	};

	LuaProto readProto51();
	LuaProto readProto52();
	LuaProto readProto53();
	LuaProto readProto54();

	std::vector<LuaInstr> readCode();
	std::vector<LuaConst> readConstants51();
	std::vector<LuaConst> readConstants52();
	std::vector<LuaConst> readConstants53();
	std::vector<LuaConst> readConstants54();
	std::vector<LuaUpvalue> readUpvalues51(int n);
	std::vector<LuaUpvalue> readUpvalues52plus(int n);
	std::vector<LuaProto> readProtos51();
	std::vector<LuaProto> readProtos52();
	std::vector<LuaProto> readProtos53();
	std::vector<LuaProto> readProtos54();
	void readDebugInfo51(LuaProto& proto);
	void readDebugInfo52plus(LuaProto& proto);
	void readDebugInfo54(LuaProto& proto);
};

} // namespace lua_parser
} // namespace retdec

#endif // RETDEC_LUA_PARSER_LUA_READER_H
