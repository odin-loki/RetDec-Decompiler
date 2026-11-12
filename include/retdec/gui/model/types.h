/**
 * @file include/retdec/gui/model/types.h
 * @brief Data the GUI's model layer hands to its views.
 *
 * Plain C++23, no UI types: the panels display these, the artifact loader and
 * the analysis bridges fill them, and the tests build them by hand. Strings
 * are UTF-8 std::string throughout.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_TYPES_H
#define RETDEC_GUI_MODEL_TYPES_H

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace retdec::gui::model {

// ── Functions ────────────────────────────────────────────────────────────────

struct PatternFlags
{
	bool stl = false;     ///< STL algorithm / container detected
	bool crypto = false;  ///< cryptographic primitive detected
	bool algo = false;    ///< generic well-known algorithm (sort, hash, ...)
	bool design = false;  ///< OO design pattern (singleton, factory, ...)
	bool library = false; ///< statically linked library code
};

struct RecoveryConfidence
{
	float typeInference = 0.f; ///< type inference quality (0-1)
	float structure = 0.f;     ///< control-flow structuring quality (0-1)
	float variable = 0.f;      ///< variable recovery quality (0-1)
	float algorithm = 0.f;     ///< algorithm/pattern recognition quality (0-1)

	/// Weighted composite: 30% type, 30% structure, 20% variable, 20% algorithm.
	float composite() const noexcept
	{
		return typeInference * 0.30f + structure * 0.30f + variable * 0.20f + algorithm * 0.20f;
	}
};

struct FunctionEntry
{
	uint64_t address = 0;
	std::string name;      ///< demangled name (if available)
	std::string rawName;   ///< mangled / raw symbol name
	std::string signature; ///< full C/C++ signature
	int sizeBytes = 0;
	int instrCount = 0;
	std::string cc; ///< calling convention
	PatternFlags patterns;
	RecoveryConfidence confidence;
	std::vector<std::string> tags; ///< user-applied annotation tags
	std::string notes;
	bool isLibrary = false;
	int startLine = -1; ///< first line in the decompiled .c (1-based), -1 unknown
	int endLine = -1;

	/// Name `retdec-decompiler --select-functions` accepts for this function.
	std::string selectFunctionsCliName() const
	{
		if (!name.empty()) return name;
		if (!rawName.empty()) return rawName;
		return std::format("function_{:x}", address);
	}
};

// ── Strings and constants ────────────────────────────────────────────────────

enum class StringType
{
	ASCII,
	UTF8,
	UTF16LE,
	UTF16BE,
	Wide,   ///< wchar_t (platform-specific width)
	Pascal, ///< length-prefixed
	Unknown,
};

enum class StringCategory
{
	Plain,
	FormatString,
	URL,
	FilePath,
	RegEx,
	CryptoConst,
};

struct StringEntry
{
	uint64_t address = 0;
	std::string value;
	StringType type = StringType::ASCII;
	StringCategory category = StringCategory::Plain;
	int length = 0;
	int refCount = 0;
};

enum class ConstantSize
{
	Byte = 1,
	Word = 2,
	Dword = 4,
	Qword = 8
};

enum class ConstantLabel
{
	None,
	MagicNumber,
	CryptoConstant,
	FloatSpecial,
	PortNumber,
	Errno,
	WinError,
	SyscallNr,
	OtherKnown,
};

struct ConstantEntry
{
	uint64_t address = 0;
	ConstantSize size = ConstantSize::Dword;
	uint64_t value = 0;
	std::string hexStr;
	std::string decStr;
	std::string label; ///< human-readable semantic label
	ConstantLabel labelKind = ConstantLabel::None;
	int refCount = 0;
};

// ── Call graph ───────────────────────────────────────────────────────────────

struct CallGraphNode
{
	uint64_t address = 0;
	std::string name;
	int instrCount = 0; ///< used to scale node size
	bool isLibrary = false;
	int moduleId = -1; ///< community / module id for clustering
};

struct CallEdge
{
	uint64_t callerAddress = 0;
	uint64_t calleeAddress = 0;
	int callCount = 1; ///< call frequency (weights arrow width)
	bool isDirect = true;
};

// ── Control-flow graph ───────────────────────────────────────────────────────

struct BlockInstr
{
	uint64_t address = 0;
	std::string text; ///< "mnemonic  operands"
};

enum class EdgeKind : uint8_t
{
	FallThrough,        ///< solid grey
	TrueBranch,         ///< solid green
	FalseBranch,        ///< solid red
	BackEdge,           ///< orange dashed arc
	ExceptionEdge,      ///< solid purple
	UnresolvedIndirect, ///< red dashed
};

struct BasicBlockData
{
	uint64_t id = 0;                ///< unique id (e.g. start address)
	uint64_t address = 0;           ///< start address (display / navigation)
	uint64_t endAddress = 0;        ///< exclusive end (from config basicBlocks)
	std::vector<BlockInstr> instrs; ///< all instructions; previews show the first few
	bool isLoopHeader = false;
	int loopId = -1; ///< >= 0 means part of this loop
	bool isEntry = false;
	bool isExit = false;
};

struct CFGEdgeData
{
	uint64_t from = 0;
	uint64_t to = 0;
	EdgeKind kind = EdgeKind::FallThrough;
};

// ── Type hierarchy ───────────────────────────────────────────────────────────

struct InheritanceLink
{
	enum class Kind : uint8_t
	{
		Public,
		Protected,
		Private,
		Virtual
	};
	std::string base;
	Kind kind = Kind::Public;
};

struct VtableSlot
{
	int index = 0;
	std::string funcName;
	uint64_t funcAddress = 0;
	bool isPure = false;     ///< pure virtual (= 0)
	bool isOverride = false; ///< overrides a base-class slot
};

struct ClassInfo
{
	std::string name;
	uint64_t vtableAddress = 0;
	std::string compiler;   ///< "MSVC", "GCC", "Clang", ...
	std::string moduleName; ///< source module / compilation unit
	std::vector<InheritanceLink> bases;
	std::vector<VtableSlot> vtable;
	bool isAbstract = false;
	int methodCount = 0; ///< total recovered methods
};

// ── Semantic detections and diagnostics ──────────────────────────────────────

struct SemanticDetection
{
	std::string function;
	std::string kind;
	std::string label;
	double confidence = 0.0;
	std::string detail;
};

enum class Severity : uint8_t
{
	Info,
	Warning,
	Error,
};

/// One line in the Problems panel (log warnings/errors and semantic detections).
struct Diagnostic
{
	Severity severity = Severity::Info;
	std::string message;
	std::string source;   ///< "decompiler log", "semantic", ...
	uint64_t address = 0; ///< 0 when the diagnostic is not about an address
	std::string function;
};

/// Address formatting used everywhere in the GUI: 0x-prefixed lowercase hex.
inline std::string hexAddress(uint64_t a)
{
	return std::format("0x{:x}", a);
}

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_TYPES_H
