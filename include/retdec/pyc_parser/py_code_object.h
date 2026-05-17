/**
 * @file include/retdec/pyc_parser/py_code_object.h
 * @brief Python code object (PyCodeObject) and associated structures.
 *
 * ## Code Object Fields (CPython internal layout)
 *
 * The Python marshal TYPE_CODE object contains these fields in order
 * (exact set varies per version — see notes on each field):
 *
 * ```
 *   co_argcount         int32   — positional argument count (incl. defaults)
 *   co_posonlyargcount  int32   — positional-only args (Python 3.8+)
 *   co_kwonlyargcount   int32   — keyword-only argument count
 *   co_nlocals          int32   — total number of local variables (< 3.11)
 *   co_stacksize        int32   — maximum stack depth
 *   co_flags            int32   — CO_* flags bitmask
 *   co_code             bytes   — raw bytecode (string/bytes object)
 *                                 (embedded in co_linetable in 3.11+)
 *   co_consts           tuple   — constants (literals, nested code objects)
 *   co_names            tuple   — names used (globals, attributes)
 *   co_varnames         tuple   — local variable names
 *   co_freevars         tuple   — free variables (from enclosing scope)
 *   co_cellvars         tuple   — cell variables (captured by inner funcs)
 *   co_filename         str     — source file name
 *   co_name             str     — function/class/lambda name
 *   co_qualname         str     — qualified name (Python 3.11+)
 *   co_firstlineno      int32   — line number of the first line
 *   co_linetable        bytes   — line number table (format varies by version)
 *   co_exceptiontable   bytes   — exception table (Python 3.11+)
 * ```
 *
 * ## CO_FLAGS bitmask
 *
 *   CO_OPTIMIZED      = 0x0001  — uses LOAD_FAST
 *   CO_NEWLOCALS      = 0x0002  — new locals dict
 *   CO_VARARGS        = 0x0004  — *args parameter
 *   CO_VARKEYWORDS    = 0x0008  — **kwargs parameter
 *   CO_NESTED         = 0x0010  — nested function
 *   CO_GENERATOR      = 0x0020  — generator function (yield)
 *   CO_NOFREE         = 0x0040  — no free or cell variables
 *   CO_COROUTINE      = 0x0100  — async def function
 *   CO_ITERABLE_COROUTINE = 0x0100  (overlap with above in some versions)
 *   CO_ASYNC_GENERATOR = 0x0200  — async generator (yield inside async def)
 *
 * ## Line Number Table Formats
 *
 *   Python ≤ 3.9  — co_lnotab: pairs (bytecode_increment, line_increment)
 *   Python 3.10   — co_linetable: variable-length entries (columns added)
 *   Python 3.11+  — co_linetable: compressed with column info (PEP 657)
 *   Python 3.12   — co_linetable: further compressed, exception table separate
 */

#ifndef RETDEC_PYC_PARSER_PY_CODE_OBJECT_H
#define RETDEC_PYC_PARSER_PY_CODE_OBJECT_H

#include "retdec/pyc_parser/pyc_magic.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retdec {
namespace pyc_parser {

// ─── CO_FLAGS ────────────────────────────────────────────────────────────────

enum CoFlags : uint32_t {
    CO_OPTIMIZED        = 0x0001,
    CO_NEWLOCALS        = 0x0002,
    CO_VARARGS          = 0x0004,
    CO_VARKEYWORDS      = 0x0008,
    CO_NESTED           = 0x0010,
    CO_GENERATOR        = 0x0020,
    CO_NOFREE           = 0x0040,
    CO_COROUTINE        = 0x0100,
    CO_ASYNC_GENERATOR  = 0x0200,
};

// ─── LineEntry ────────────────────────────────────────────────────────────────

/**
 * @brief A decoded line-number-table entry.
 *
 * Maps a range of bytecode offsets [start, end) to a source location.
 * Column information is present only for Python 3.11+ (PEP 657).
 */
struct LineEntry {
    uint32_t startOffset = 0;   ///< First bytecode offset covered
    uint32_t endOffset   = 0;   ///< Exclusive end offset
    int32_t  line        = -1;  ///< Source line (1-based; -1 = no info)
    int32_t  endLine     = -1;  ///< Source end line (3.11+; -1 = same as line)
    int32_t  column      = -1;  ///< Source column (0-based; -1 = no info)
    int32_t  endColumn   = -1;  ///< Source end column (3.11+; -1 = no info)
};

// ─── ExceptionEntry ───────────────────────────────────────────────────────────

/**
 * @brief Python 3.11+ exception table entry.
 */
struct ExceptionEntry {
    uint32_t start    = 0;   ///< Start bytecode offset (inclusive)
    uint32_t end      = 0;   ///< End bytecode offset (exclusive)
    uint32_t target   = 0;   ///< Handler entry point
    uint32_t depth    = 0;   ///< Stack depth at the start of the try block
    bool     lasti    = false; ///< Push lasti as part of the handler frame?
};

// ─── PyCodeObject ─────────────────────────────────────────────────────────────

/**
 * @brief Deserialized Python code object from a .pyc marshal stream.
 */
struct PyCodeObject {
    // ── Arguments ──────────────────────────────────────────────────────────────

    int32_t  co_argcount         = 0;  ///< Total positional params
    int32_t  co_posonlyargcount  = 0;  ///< Positional-only params (3.8+)
    int32_t  co_kwonlyargcount   = 0;
    int32_t  co_nlocals          = 0;  ///< Local count (omitted in 3.11+ co_varnames used)
    int32_t  co_stacksize        = 0;
    uint32_t co_flags            = 0;

    // ── Bytecode ───────────────────────────────────────────────────────────────

    std::vector<uint8_t> co_code;      ///< Raw bytecode bytes

    // ── Constants and names ────────────────────────────────────────────────────

    /// Marshal objects: each element is a PyMarshalObject (see py_marshal.h)
    /// For simplicity, we store the decoded value as a string (or recurse).
    struct Const {
        enum class Kind {
            None, True, False, Ellipsis,
            Int, Float, Complex,
            Bytes, Str, Unicode,
            Tuple, FrozenSet,
            Code,   ///< Nested code object
        };
        Kind        kind = Kind::None;
        int64_t     ival = 0;
        double      fval = 0.0;
        std::string sval;
        std::vector<Const> elements;    ///< For Tuple / FrozenSet
        std::shared_ptr<PyCodeObject> code; ///< For nested code objects
    };

    std::vector<Const>   co_consts;    ///< Constant pool
    std::vector<std::string> co_names;     ///< Global / attribute names
    std::vector<std::string> co_varnames;  ///< Local variable names
    std::vector<std::string> co_freevars; ///< Free variable names
    std::vector<std::string> co_cellvars; ///< Cell variable names

    // ── Metadata ───────────────────────────────────────────────────────────────

    std::string co_filename;
    std::string co_name;
    std::string co_qualname;      ///< Qualified name (3.11+)
    int32_t     co_firstlineno = 0;

    // ── Line / exception tables ────────────────────────────────────────────────

    std::vector<uint8_t>     co_linetable;      ///< Raw line table bytes
    std::vector<uint8_t>     co_exceptiontable; ///< Raw exception table (3.11+)

    // ── Decoded tables (populated by decode*() calls) ─────────────────────────

    std::vector<LineEntry>      lineTable;
    std::vector<ExceptionEntry> exceptionTable;

    // ── Python version that produced this object ───────────────────────────────

    PythonVersion version;

    // ── Helpers ────────────────────────────────────────────────────────────────

    bool isGenerator()      const { return (co_flags & CO_GENERATOR) != 0; }
    bool isCoroutine()      const { return (co_flags & CO_COROUTINE) != 0; }
    bool isAsyncGenerator() const { return (co_flags & CO_ASYNC_GENERATOR) != 0; }
    bool hasVarArgs()       const { return (co_flags & CO_VARARGS) != 0; }
    bool hasVarKwargs()     const { return (co_flags & CO_VARKEYWORDS) != 0; }

    /// Resolve a bytecode offset to a 1-based source line (-1 = unknown).
    int32_t lineAt(uint32_t offset) const;

    /// Resolve a bytecode offset to a 0-based source column (-1 = unknown).
    int32_t columnAt(uint32_t offset) const;
};

// ─── LineTable decoders ───────────────────────────────────────────────────────

/**
 * @brief Decode co_lnotab (Python ≤ 3.9) into LineEntry vector.
 *
 * Format: pairs of (byte_increment, line_increment), both uint8_t.
 * byte_increment=0 means same offset; line_increment=0 means same line.
 */
std::vector<LineEntry> decodeLnotab(
    const std::vector<uint8_t>& lnotab,
    int32_t                     firstLineno,
    uint32_t                    codeLen);

/**
 * @brief Decode Python 3.10 co_linetable format.
 *
 * New in 3.10: variable-length entries, column info added.
 * Each entry covers a single instruction.
 */
std::vector<LineEntry> decodeLnotab310(
    const std::vector<uint8_t>& linetable,
    int32_t                     firstLineno,
    uint32_t                    codeLen);

/**
 * @brief Decode Python 3.11+ co_linetable (PEP 657 format).
 *
 * Covers bytecode units (2 bytes each in 3.11+).
 * Each entry: 1-byte code + variable-length data.
 * Encodes line, end_line, column, end_column.
 */
std::vector<LineEntry> decodeLnotab311(
    const std::vector<uint8_t>& linetable,
    int32_t                     firstLineno,
    uint32_t                    codeLen);

/**
 * @brief Decode Python 3.11+ exception table.
 *
 * Format: variable-length ULEB128-encoded triples.
 */
std::vector<ExceptionEntry> decodeExceptionTable(
    const std::vector<uint8_t>& table);

} // namespace pyc_parser
} // namespace retdec

#endif // RETDEC_PYC_PARSER_PY_CODE_OBJECT_H
