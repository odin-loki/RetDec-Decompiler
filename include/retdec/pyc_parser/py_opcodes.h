/**
 * @file include/retdec/pyc_parser/py_opcodes.h
 * @brief Python bytecode opcode tables for CPython 3.8–3.12.
 *
 * ## Overview
 *
 * Each CPython feature release has a distinct set of opcodes.  We model this
 * with a per-version opcode table that maps an opcode byte to:
 *
 *   - Name (e.g. "LOAD_FAST", "CALL_FUNCTION")
 *   - Flags: has_arg (opcode >= HAVE_ARGUMENT), is_jump, is_jump_forward,
 *     is_jump_absolute, consumes stack, produces stack
 *   - Stack effect (+n = pushes n, -n = pops n; UNKNOWN = depends on arg)
 *   - Whether it's an extended arg prefix (EXTENDED_ARG = 0x5A = 90)
 *
 * ## Python 3.11+ wordcode → instruction units
 *
 * In Python 3.11+, every instruction is exactly 2 bytes (opcode + arg).
 * EXTENDED_ARG still works to form 4-byte, 6-byte, 8-byte sequences.
 * In Python 3.12 some specialised adaptive opcodes appear in co_code;
 * we treat them as their base opcode for decompilation purposes.
 *
 * ## HAVE_ARGUMENT threshold
 *
 *   Python 3.8–3.10: HAVE_ARGUMENT = 90 (opcodes >= 90 take an argument)
 *   Python 3.11+:    Every instruction takes an argument (always 2 bytes)
 *
 * Source: CPython/Lib/opcode.py for each version.
 */

#ifndef RETDEC_PYC_PARSER_PY_OPCODES_H
#define RETDEC_PYC_PARSER_PY_OPCODES_H

#include "retdec/pyc_parser/pyc_magic.h"

#include <cstdint>
#include <string>

namespace retdec {
namespace pyc_parser {

// ─── OpcodeInfo ───────────────────────────────────────────────────────────────

enum class OpcodeKind : uint8_t {
    Normal,
    Jump,           ///< Jump (may be forward or backward)
    JumpForward,    ///< Jump to offset relative to current (3.8-3.9)
    JumpAbsolute,   ///< Jump to absolute offset
    JumpBackward,   ///< Backward jump (3.11+ JUMP_BACKWARD)
    Call,           ///< Function call
    Return,         ///< RETURN_VALUE / RETURN_CONST
    Raise,          ///< RAISE_VARARGS
    Import,         ///< IMPORT_NAME
    ExtendedArg,    ///< EXTENDED_ARG (opcode 90 / 144 in 3.12)
    Unknown,        ///< Unrecognised (possibly specialised adaptive)
};

struct OpcodeInfo {
    uint8_t     opcode  = 0;
    const char* name    = "<unknown>";
    OpcodeKind  kind    = OpcodeKind::Normal;
    bool        hasArg  = false;    ///< Takes an argument byte
    int8_t      stackEffect = 0;    ///< Net stack change (UNKNOWN = -128)

    static constexpr int8_t kUnknownEffect = -128;

    bool isJump()   const {
        return kind == OpcodeKind::Jump ||
               kind == OpcodeKind::JumpForward ||
               kind == OpcodeKind::JumpAbsolute ||
               kind == OpcodeKind::JumpBackward;
    }
    bool isReturn() const { return kind == OpcodeKind::Return; }
    bool isCall()   const { return kind == OpcodeKind::Call; }
};

// ─── Opcode table API ─────────────────────────────────────────────────────────

/**
 * @brief Return the OpcodeInfo for an opcode byte in the given Python version.
 *
 * In Python 3.11+ every instruction has an argument, so `hasArg` is always true.
 * For unknown / adaptive opcodes in 3.12, the returned info has kind=Unknown.
 */
OpcodeInfo opcodeInfo(uint8_t opcode, const PythonVersion& ver);

/**
 * @brief Return the opcode byte for a given name in the given Python version.
 *
 * Returns 0xFF if the name does not exist in that version.
 */
uint8_t opcodeByName(const char* name, const PythonVersion& ver);

/**
 * @brief The HAVE_ARGUMENT threshold for the given version.
 *
 * For versions < 3.11: opcodes >= 90 take an argument.
 * For versions >= 3.11: all opcodes take an argument (threshold = 0).
 */
uint8_t haveArgument(const PythonVersion& ver);

/**
 * @brief True if the given version uses 2-byte instruction units (3.6+).
 *
 * Python 3.6 introduced "wordcode" (2-byte instructions).
 * Before 3.6 instructions could be 1 or 3 bytes.
 * We only support 3.8+ so this always returns true.
 */
constexpr bool isWordcode(const PythonVersion&) { return true; }

// ─── Commonly-referenced opcode constants ─────────────────────────────────────

// EXTENDED_ARG is the same across all supported versions
static constexpr uint8_t OP_EXTENDED_ARG     = 144; // 0x90

// RESUME is new in 3.11 (first instruction in every code object)
static constexpr uint8_t OP_RESUME_311       = 151; // 0x97

// Common opcodes (same value across 3.8-3.10, may differ in 3.11+)
static constexpr uint8_t OP_LOAD_FAST        =  124; // 0x7C  (all versions)
static constexpr uint8_t OP_STORE_FAST       =  125; // 0x7D
static constexpr uint8_t OP_LOAD_CONST       =  100; // 0x64
static constexpr uint8_t OP_LOAD_GLOBAL      =  116; // 0x74  (3.8-3.10)
static constexpr uint8_t OP_LOAD_GLOBAL_311  =  116; // same byte, new semantics in 3.11
static constexpr uint8_t OP_RETURN_VALUE     =   83; // 0x53  (3.8-3.11)
static constexpr uint8_t OP_RETURN_CONST_312 =  121; // 0x79  (3.12+)
static constexpr uint8_t OP_POP_TOP          =    1; // 0x01
static constexpr uint8_t OP_CALL_FUNCTION    =  131; // 0x83  (3.8-3.10)
static constexpr uint8_t OP_CALL_311         =  171; // 0xAB  (3.11+)
static constexpr uint8_t OP_MAKE_FUNCTION    = 132;  // 0x84
static constexpr uint8_t OP_BUILD_TUPLE      = 102;  // 0x66
static constexpr uint8_t OP_BUILD_LIST       = 103;  // 0x67
static constexpr uint8_t OP_BINARY_OP_311    = 122;  // 0x7A  (3.11+)
static constexpr uint8_t OP_COMPARE_OP       = 107;  // 0x6B

} // namespace pyc_parser
} // namespace retdec

#endif // RETDEC_PYC_PARSER_PY_OPCODES_H
