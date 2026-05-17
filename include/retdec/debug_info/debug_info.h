/**
 * @file include/retdec/debug_info/debug_info.h
 * @brief Debug ground-truth data model and unified extractor interface.
 *
 * ## Overview
 *
 * DebugGroundTruth is the central data store produced by parsing DWARF (via
 * DwarfExtractor) or PDB (via PdbExtractor).  It is fed into TypeInferenceMgr
 * and VariableRecoveryMgr as the highest-priority constraint source, overriding
 * all heuristic inference.
 *
 * ## Data model
 *
 *   DebugTypeKind — primitive, pointer, array, struct, union, enum, typedef,
 *                   function, void
 *   DebugType     — type descriptor with fields for struct layouts, pointed-to
 *                   type, element count, etc.
 *   DebugVar      — variable/parameter: name, type, storage location, live range
 *   StorageLoc    — union: register ID, stack offset, static address
 *   DebugFunc     — function: name, address, size, return type, parameters,
 *                   local variables, linkage name
 *   InlinedSite   — inlined function instance: original name, [lo, hi) range
 *   DebugSourceFile — compile-unit source file path
 *   DebugGroundTruth — top-level container with lookup by VMA
 *
 * ## DWARF location expressions
 *
 * The DebugLocEvaluator evaluates a single DWARF expression (byte stream) at
 * a given PC.  Supported opcodes:
 *   DW_OP_reg0..31, DW_OP_regx         → register storage
 *   DW_OP_fbreg <sleb>                  → frame-base-relative stack slot
 *   DW_OP_addr  <uword>                 → static absolute address
 *   DW_OP_breg0..31 <sleb>             → register + signed offset
 *   DW_OP_stack_value                   → value is in previous stack entry
 *   DW_OP_lit0..31                      → literal 0..31
 *   DW_OP_plus_uconst <uleb>           → add unsigned constant
 */

#ifndef RETDEC_DEBUG_INFO_DEBUG_INFO_H
#define RETDEC_DEBUG_INFO_DEBUG_INFO_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace debug_info {

// ─── Type system ─────────────────────────────────────────────────────────────

enum class DebugTypeKind : uint8_t {
    Void,
    Primitive,   ///< int, char, float, etc.
    Pointer,     ///< T*
    Array,       ///< T[N]
    Struct,      ///< struct/class
    Union,
    Enum,
    Typedef,     ///< typedef alias
    FunctionPtr, ///< function pointer type
    Unknown,
};

struct DebugField {
    std::string  name;
    uint32_t     byteOffset = 0;
    uint32_t     bitOffset  = 0; ///< non-zero for bit fields
    uint32_t     bitSize    = 0; ///< non-zero for bit fields
    uint64_t     typeId     = 0; ///< ID into DebugGroundTruth::types map
};

struct DebugEnumerator {
    std::string name;
    int64_t     value = 0;
};

struct DebugType {
    uint64_t       id       = 0;      ///< Unique ID (e.g. DIE offset for DWARF)
    DebugTypeKind  kind     = DebugTypeKind::Unknown;
    std::string    name;              ///< Base name (without qualifiers)
    uint32_t       byteSize = 0;

    // Primitive: encoding name (e.g. "DW_ATE_signed", "DW_ATE_float")
    std::string    encoding;

    // Pointer / Typedef → pointedToTypeId
    uint64_t       pointedToTypeId = 0;

    // Array → element count + element type
    uint64_t       elementTypeId = 0;
    uint64_t       elementCount  = 0;

    // Struct / Union → fields
    std::vector<DebugField> fields;

    // Enum → enumerators
    uint64_t                    baseTypeId = 0;
    std::vector<DebugEnumerator> enumerators;

    // FunctionPtr → return + parameter types
    uint64_t              returnTypeId = 0;
    std::vector<uint64_t> paramTypeIds;
};

// ─── Variable storage location ────────────────────────────────────────────────

enum class StorageKind : uint8_t {
    Unknown,
    Register,   ///< In a named register
    StackSlot,  ///< Frame-pointer-relative (fbreg + offset)
    StaticAddr, ///< Absolute virtual address
    Optimized,  ///< DW_OP_stack_value: value only in stack / inlined away
};

struct StorageLoc {
    StorageKind kind   = StorageKind::Unknown;
    uint32_t    regNum = 0;         ///< DWARF register number
    int64_t     offset = 0;         ///< Stack offset or addend
    uint64_t    addr   = 0;         ///< Static address

    static StorageLoc reg(uint32_t r)
    { StorageLoc s; s.kind=StorageKind::Register; s.regNum=r; return s; }
    static StorageLoc stack(int64_t off)
    { StorageLoc s; s.kind=StorageKind::StackSlot; s.offset=off; return s; }
    static StorageLoc staticAddr(uint64_t a)
    { StorageLoc s; s.kind=StorageKind::StaticAddr; s.addr=a; return s; }
    static StorageLoc optimized()
    { StorageLoc s; s.kind=StorageKind::Optimized; return s; }
};

// ─── Live range ───────────────────────────────────────────────────────────────

struct LiveRange {
    uint64_t    lo;  ///< First address where location is valid
    uint64_t    hi;  ///< One past last address
    StorageLoc  loc;
};

// ─── Variable / parameter ─────────────────────────────────────────────────────

struct DebugVar {
    std::string           name;
    uint64_t              typeId    = 0;
    bool                  isParam   = false;
    uint32_t              paramIdx  = 0;   ///< 0-based parameter index
    std::vector<LiveRange> liveRanges;     ///< Location may change across ranges

    /// Return the storage location at a given PC (or Unknown if not live).
    StorageLoc locationAt(uint64_t pc) const noexcept;
};

// ─── Inlined function instance ────────────────────────────────────────────────

struct InlinedSite {
    std::string   calleeName;       ///< Original (non-inlined) function name
    std::string   calleeFile;       ///< Source file of the original function
    uint32_t      callLine  = 0;    ///< Source line of the call site
    uint64_t      loAddr    = 0;    ///< First address of the inlined body
    uint64_t      hiAddr    = 0;    ///< One past last address
};

// ─── Function ─────────────────────────────────────────────────────────────────

struct DebugFunc {
    std::string              name;
    std::string              linkageName;   ///< Mangled name (if available)
    std::string              sourceFile;
    uint32_t                 sourceLine = 0;
    uint64_t                 lowPc      = 0;
    uint64_t                 highPc     = 0;
    uint64_t                 returnTypeId = 0;
    bool                     isExternal = false;
    bool                     isInline   = false;
    bool                     noReturn   = false;

    std::vector<DebugVar>    params;
    std::vector<DebugVar>    locals;
    std::vector<InlinedSite> inlinedSites;
};

// ─── Source file record ───────────────────────────────────────────────────────

struct DebugSourceFile {
    std::string compDir;    ///< DW_AT_comp_dir
    std::string path;       ///< Full path (compDir / filename)
    std::string language;   ///< e.g. "C99", "C++14"
};

// ─── Ground truth container ───────────────────────────────────────────────────

struct DebugGroundTruth {
    // All function records, keyed by lowPc.
    std::unordered_map<uint64_t, DebugFunc>  functions;

    // All type records, keyed by unique type ID.
    std::unordered_map<uint64_t, DebugType>  types;

    // All inlined sites (flattened from all functions), searchable by address.
    std::vector<InlinedSite>                 allInlined;

    // Source files from compile units.
    std::vector<DebugSourceFile>             sourceFiles;

    // Diagnostics from the extractor.
    std::vector<std::string>                 diagnostics;

    // ── Lookup helpers ────────────────────────────────────────────────────────

    /// Return the DebugFunc covering [pc], or nullptr.
    const DebugFunc* funcAt(uint64_t pc) const noexcept;

    /// Return the DebugFunc with the given name, or nullptr.
    const DebugFunc* funcByName(const std::string& name) const noexcept;

    /// Return the DebugType for id, or nullptr.
    const DebugType* typeById(uint64_t id) const noexcept;

    /// Return a human-readable type string for a given typeId.
    std::string typeName(uint64_t id) const;

    /// Return all InlinedSites whose range contains [pc].
    std::vector<const InlinedSite*> inlinedAt(uint64_t pc) const;

    bool empty() const noexcept {
        return functions.empty() && types.empty();
    }
};

// ─── DWARF location expression evaluator ─────────────────────────────────────

/**
 * Evaluates a raw DWARF location expression byte sequence.
 *
 * The evaluator does not require the full DWARF library — it parses only the
 * opcodes needed for variable location recovery (DW_OP_reg*, DW_OP_fbreg,
 * DW_OP_addr, DW_OP_breg*, DW_OP_stack_value, DW_OP_lit*, DW_OP_plus_uconst).
 */
class DebugLocEvaluator {
public:
    /**
     * Evaluate the location expression in [expr, expr+len).
     * @param addrSize   Pointer size in bytes (4 or 8).
     * @return           The resolved StorageLoc.
     */
    static StorageLoc evaluate(const uint8_t* expr, std::size_t len,
                               uint8_t addrSize = 8);

    /// Decode a ULEB128 value from `ptr`; advances `ptr` past the value.
    static uint64_t readULEB128(const uint8_t*& ptr, const uint8_t* end) noexcept;

    /// Decode a SLEB128 value from `ptr`; advances `ptr` past the value.
    static int64_t  readSLEB128(const uint8_t*& ptr, const uint8_t* end) noexcept;
};

// ─── Extractor interface ──────────────────────────────────────────────────────

/**
 * Abstract base for debug-info extractors (DWARF, PDB).
 */
class DebugExtractorBase {
public:
    virtual ~DebugExtractorBase() = default;

    /**
     * Extract all debug information and populate `out`.
     * @return true on success (even partial); false on fatal error.
     */
    virtual bool extract(DebugGroundTruth& out) = 0;

    /// Return the extractor name for diagnostics.
    virtual std::string name() const = 0;
};

} // namespace debug_info
} // namespace retdec

#endif // RETDEC_DEBUG_INFO_DEBUG_INFO_H
