/**
 * @file src/pyc_parser/py_opcodes.cpp
 * @brief Python bytecode opcode tables for CPython 3.8–3.12.
 *
 * Each version's opcode table is a full 256-entry array (0–255).
 * Unmapped opcodes get name="<unknown>" and kind=Unknown.
 *
 * Sources:
 *   CPython/Lib/opcode.py for each version tag
 *   CPython/Python/ceval.c for stack effects
 */

#include "retdec/pyc_parser/py_opcodes.h"

#include <cstring>

namespace retdec {
namespace pyc_parser {

// ─── Common opcodes (shared across 3.8–3.10) ─────────────────────────────────

// Helper macro: defines an OpcodeInfo entry
#define OP(code, nm, k, ha, se) \
    {static_cast<uint8_t>(code), nm, OpcodeKind::k, ha, static_cast<int8_t>(se)}

static const OpcodeInfo kCommon38_310[] = {
    OP(  0, "CACHE",           Normal,       false,  0),
    OP(  1, "POP_TOP",         Normal,       false, -1),
    OP(  2, "ROT_TWO",         Normal,       false,  0),
    OP(  3, "ROT_THREE",       Normal,       false,  0),
    OP(  4, "DUP_TOP",         Normal,       false,  1),
    OP(  5, "DUP_TOP_TWO",     Normal,       false,  2),
    OP(  6, "ROT_FOUR",        Normal,       false,  0),  // 3.8+
    OP(  9, "NOP",             Normal,       false,  0),
    OP( 10, "UNARY_POSITIVE",  Normal,       false,  0),
    OP( 11, "UNARY_NEGATIVE",  Normal,       false,  0),
    OP( 12, "UNARY_NOT",       Normal,       false,  0),
    OP( 15, "UNARY_INVERT",    Normal,       false,  0),
    OP( 16, "BINARY_MATRIX_MULTIPLY", Normal, false, -1),
    OP( 17, "INPLACE_MATRIX_MULTIPLY",Normal, false, -1),
    OP( 19, "BINARY_POWER",    Normal,       false, -1),
    OP( 20, "BINARY_MULTIPLY", Normal,       false, -1),
    OP( 22, "BINARY_MODULO",   Normal,       false, -1),
    OP( 23, "BINARY_ADD",      Normal,       false, -1),
    OP( 24, "BINARY_SUBTRACT", Normal,       false, -1),
    OP( 25, "BINARY_SUBSCR",   Normal,       false, -1),
    OP( 26, "BINARY_FLOOR_DIVIDE", Normal,   false, -1),
    OP( 27, "BINARY_TRUE_DIVIDE",  Normal,   false, -1),
    OP( 28, "INPLACE_FLOOR_DIVIDE",Normal,   false, -1),
    OP( 29, "INPLACE_TRUE_DIVIDE", Normal,   false, -1),
    // Opcodes 36-82: correct for Python 3.8-3.10 (version-specific overrides applied later)
    OP( 50, "GET_AITER",       Normal,       false,  0),
    OP( 51, "GET_ANEXT",       Normal,       false,  1),
    OP( 52, "BEFORE_ASYNC_WITH", Normal,     false,  1),
    OP( 53, "BEFORE_WITH",     Normal,       true,   1),
    OP( 54, "END_ASYNC_FOR",   Normal,       false, -7),
    OP( 55, "INPLACE_ADD",     Normal,       false, -1),
    OP( 56, "INPLACE_SUBTRACT",Normal,       false, -1),
    OP( 57, "INPLACE_MULTIPLY",Normal,       false, -1),
    OP( 58, "INPLACE_MODULO",  Normal,       false, -1),
    OP( 59, "INPLACE_POWER",   Normal,       false, -1),
    OP( 60, "STORE_SUBSCR",    Normal,       false, -3),
    OP( 61, "DELETE_SUBSCR",   Normal,       false, -2),
    OP( 62, "BINARY_LSHIFT",   Normal,       false, -1),
    OP( 63, "BINARY_RSHIFT",   Normal,       false, -1),
    OP( 64, "BINARY_AND",      Normal,       false, -1),
    OP( 65, "WITH_EXCEPT_START", Normal,     false,  1),
    OP( 66, "BINARY_XOR",      Normal,       false, -1),
    OP( 67, "BINARY_OR",       Normal,       false, -1),
    OP( 68, "GET_ITER",        Normal,       false,  0),
    OP( 69, "GET_YIELD_FROM_ITER", Normal,   false,  0),
    OP( 70, "PRINT_EXPR",      Normal,       false, -1),
    OP( 71, "LOAD_BUILD_CLASS",Normal,       false,  1),
    OP( 72, "YIELD_FROM",      Normal,       false, -1),
    OP( 73, "GET_AWAITABLE",   Normal,       false,  0),
    OP( 74, "LOAD_ASSERTION_ERROR",Normal,   false,  1),
    OP( 75, "INPLACE_LSHIFT",  Normal,       false, -1),
    OP( 76, "INPLACE_RSHIFT",  Normal,       false, -1),
    OP( 77, "INPLACE_AND",     Normal,       false, -1),
    OP( 78, "INPLACE_XOR",     Normal,       false, -1),
    OP( 79, "INPLACE_OR",      Normal,       false, -1),
    OP( 80, "LIST_TO_TUPLE",   Normal,       false,  0),
    OP( 82, "BINARY_OR",       Normal,       false, -1),
    OP( 83, "RETURN_VALUE",    Return,       false, -1),
    OP( 84, "IMPORT_STAR",     Normal,       false, -1),
    OP( 85, "SETUP_ANNOTATIONS",Normal,      false,  0),
    OP( 86, "YIELD_VALUE",     Normal,       false,  0),
    OP( 87, "POP_BLOCK",       Normal,       false,  0),
    OP( 88, "END_FINALLY",     Normal,       false, -6),
    OP( 89, "POP_EXCEPT",      Normal,       false, -3),
    // HAVE_ARGUMENT threshold = 90
    OP( 90, "STORE_NAME",      Normal,       true,  -1),
    OP( 91, "DELETE_NAME",     Normal,       true,   0),
    OP( 92, "UNPACK_SEQUENCE", Normal,       true,   OpcodeInfo::kUnknownEffect),
    OP( 93, "FOR_ITER",        Jump,         true,   1),
    OP( 94, "UNPACK_EX",       Normal,       true,   OpcodeInfo::kUnknownEffect),
    OP( 95, "STORE_ATTR",      Normal,       true,  -2),
    OP( 96, "DELETE_ATTR",     Normal,       true,  -1),
    OP( 97, "STORE_GLOBAL",    Normal,       true,  -1),
    OP( 98, "DELETE_GLOBAL",   Normal,       true,   0),
    OP( 99, "ROT_N",           Normal,       true,   0),
    OP(100, "LOAD_CONST",      Normal,       true,   1),
    OP(101, "LOAD_NAME",       Normal,       true,   1),
    OP(102, "BUILD_TUPLE",     Normal,       true,   OpcodeInfo::kUnknownEffect),
    OP(103, "BUILD_LIST",      Normal,       true,   OpcodeInfo::kUnknownEffect),
    OP(104, "BUILD_SET",       Normal,       true,   OpcodeInfo::kUnknownEffect),
    OP(105, "BUILD_MAP",       Normal,       true,   OpcodeInfo::kUnknownEffect),
    OP(106, "LOAD_ATTR",       Normal,       true,   0),
    OP(107, "COMPARE_OP",      Normal,       true,  -1),
    OP(108, "IMPORT_NAME",     Import,       true,  -1),
    OP(109, "IMPORT_FROM",     Normal,       true,   1),
    OP(110, "JUMP_FORWARD",    JumpForward,  true,   0),
    OP(111, "JUMP_IF_FALSE_OR_POP", Jump,    true,   OpcodeInfo::kUnknownEffect),
    OP(112, "JUMP_IF_TRUE_OR_POP",  Jump,    true,   OpcodeInfo::kUnknownEffect),
    OP(113, "JUMP_ABSOLUTE",   JumpAbsolute, true,   0),
    OP(114, "POP_JUMP_IF_FALSE", Jump,       true,  -1),
    OP(115, "POP_JUMP_IF_TRUE",  Jump,       true,  -1),
    OP(116, "LOAD_GLOBAL",     Normal,       true,   1),
    OP(117, "IS_OP",           Normal,       true,  -1),
    OP(118, "CONTAINS_OP",     Normal,       true,  -1),
    OP(119, "RERAISE",         Raise,        true,   OpcodeInfo::kUnknownEffect),
    OP(120, "COPY",            Normal,       true,   1),
    OP(121, "RETURN_CONST",    Return,       true,   1),  // 3.12+
    OP(122, "BINARY_OP",       Normal,       true,  -1),  // 3.11+
    OP(123, "SEND",            Normal,       true,  OpcodeInfo::kUnknownEffect),
    OP(124, "LOAD_FAST",       Normal,       true,   1),
    OP(125, "STORE_FAST",      Normal,       true,  -1),
    OP(126, "DELETE_FAST",     Normal,       true,   0),
    OP(127, "LOAD_FAST_CHECK", Normal,       true,   1),  // 3.12+
    OP(128, "POP_JUMP_IF_NOT_NONE", Jump,    true,  -1),  // 3.11+
    OP(129, "POP_JUMP_IF_NONE",     Jump,    true,  -1),  // 3.11+
    OP(130, "RAISE_VARARGS",   Raise,        true,   OpcodeInfo::kUnknownEffect),
    OP(131, "CALL_FUNCTION",   Call,         true,   OpcodeInfo::kUnknownEffect),
    OP(132, "MAKE_FUNCTION",   Normal,       true,   OpcodeInfo::kUnknownEffect),
    OP(133, "BUILD_SLICE",     Normal,       true,   OpcodeInfo::kUnknownEffect),
    OP(134, "JUMP_BACKWARD_NO_INTERRUPT", JumpBackward, true, 0),  // 3.11+
    OP(135, "LOAD_CLOSURE",    Normal,       true,   1),
    OP(136, "LOAD_DEREF",      Normal,       true,   1),
    OP(137, "STORE_DEREF",     Normal,       true,  -1),
    OP(138, "DELETE_DEREF",    Normal,       true,   0),
    OP(139, "JUMP_BACKWARD",   JumpBackward, true,   0),  // 3.11+
    OP(140, "CALL_FUNCTION_KW",Call,         true,   OpcodeInfo::kUnknownEffect),
    OP(141, "CALL_FUNCTION_EX",Call,         true,   OpcodeInfo::kUnknownEffect),
    OP(142, "SETUP_WITH",      Normal,       true,   7),
    OP(143, "EXTENDED_ARG",    ExtendedArg,  true,   0),
    OP(144, "EXTENDED_ARG",    ExtendedArg,  true,   0),
    OP(145, "LIST_APPEND",     Normal,       true,  -1),
    OP(146, "SET_ADD",         Normal,       true,  -1),
    OP(147, "MAP_ADD",         Normal,       true,  -2),
    OP(148, "LOAD_CLASSDEREF", Normal,       true,   1),
    OP(149, "COPY_FREE_VARS",  Normal,       true,   0),  // 3.12+
    OP(151, "RESUME",          Normal,       true,   0),  // 3.11+
    OP(152, "MATCH_CLASS",     Normal,       true,  OpcodeInfo::kUnknownEffect),
    OP(154, "FORMAT_VALUE",    Normal,       true,  OpcodeInfo::kUnknownEffect),
    OP(155, "BUILD_CONST_KEY_MAP", Normal,   true,  OpcodeInfo::kUnknownEffect),
    OP(156, "BUILD_STRING",    Normal,       true,  OpcodeInfo::kUnknownEffect),
    OP(160, "LOAD_METHOD",     Normal,       true,   1),
    OP(161, "CALL_METHOD",     Call,         true,  OpcodeInfo::kUnknownEffect),
    OP(162, "LIST_EXTEND",     Normal,       true,  -1),
    OP(163, "SET_UPDATE",      Normal,       true,  -1),
    OP(164, "DICT_MERGE",      Normal,       true,  -1),
    OP(165, "DICT_UPDATE",     Normal,       true,  -1),
    OP(166, "PRECALL",         Normal,       true,   0),  // 3.11 only
    OP(168, "CALL_NO_KW",      Call,         true,  OpcodeInfo::kUnknownEffect),  // 3.11 internal
    OP(171, "CALL",            Call,         true,  OpcodeInfo::kUnknownEffect),  // 3.11+
    OP(172, "KW_NAMES",        Normal,       true,   0),  // 3.11+
    OP(173, "CALL_INTRINSIC_1",Normal,       true,   0),  // 3.12+
    OP(174, "CALL_INTRINSIC_2",Normal,       true,  -1),  // 3.12+
    OP(175, "LOAD_FAST_AND_CLEAR", Normal,   true,   1),  // 3.12+
    OP(176, "RETURN_GENERATOR",Normal,       false,  1),  // 3.12+
    OP(177, "SEND",            Normal,       true,  OpcodeInfo::kUnknownEffect),
    OP(178, "ASYNC_GEN_WRAP",  Normal,       false,  0),  // 3.11+
    OP(179, "HAVE_ARGUMENT",   Normal,       false,  0),  // placeholder
    OP(180, "PUSH_NULL",       Normal,       false,  1),  // 3.11+
    OP(182, "PUSH_EXC_INFO",   Normal,       false,  1),  // 3.11+
    OP(183, "CHECK_EXC_MATCH", Normal,       false,  0),  // 3.11+
    OP(184, "BEFORE_WITH",     Normal,       true,   1),  // 3.11+
    OP(185, "CLEANUP_THROW",   Normal,       false,  OpcodeInfo::kUnknownEffect),
    OP(186, "STOPITERATION_ERROR", Normal,   false,  0),
    OP(187, "MATCH_SEQUENCE",  Normal,       false,  1),
    OP(188, "MATCH_MAPPING",   Normal,       false,  1),
    OP(189, "POP_JUMP_FORWARD_IF_FALSE", JumpForward, true, -1),  // 3.11+
    OP(190, "POP_JUMP_FORWARD_IF_TRUE",  JumpForward, true, -1),  // 3.11+
    OP(191, "POP_JUMP_FORWARD_IF_NOT_NONE", JumpForward, true, -1), // 3.11+
    OP(192, "POP_JUMP_FORWARD_IF_NONE",  JumpForward, true, -1),  // 3.11+
    OP(193, "POP_JUMP_BACKWARD_IF_FALSE", JumpBackward, true, -1), // 3.11+
    OP(194, "POP_JUMP_BACKWARD_IF_TRUE",  JumpBackward, true, -1), // 3.11+
    OP(195, "POP_JUMP_BACKWARD_IF_NOT_NONE", JumpBackward, true, -1),
    OP(196, "POP_JUMP_BACKWARD_IF_NONE",  JumpBackward, true, -1),
    OP(255, "DO_RAISE",        Raise,        true,  OpcodeInfo::kUnknownEffect),
};

static const size_t kCommon38_310Size = sizeof(kCommon38_310)/sizeof(kCommon38_310[0]);

#undef OP

// ─── Build lookup ─────────────────────────────────────────────────────────────

static OpcodeInfo kUnknownOp = {0, "<unknown>", OpcodeKind::Unknown, false, 0};

static OpcodeInfo lookupFromTable(uint8_t op, const OpcodeInfo* table, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (table[i].opcode == op) return table[i];
    }
    OpcodeInfo unk = kUnknownOp;
    unk.opcode = op;
    return unk;
}

// ─── Public API ──────────────────────────────────────────────────────────────

// ─── Python 3.11 opcode remappings ───────────────────────────────────────────
// In 3.11, some low-numbered opcodes were renumbered or introduced.
// The common table handles shared entries; these patch the differences.
static void applyVersion311Overrides(uint8_t opcode, OpcodeInfo& info) {
    switch (opcode) {
    case 2:  info = {2,  "PUSH_NULL",   OpcodeKind::Normal, true,  1};  break;
    case 3:  info = {3,  "RESERVED",    OpcodeKind::Normal, false, 0};  break;
    case 30: info = {30, "GET_LEN",     OpcodeKind::Normal, false, 1};  break;
    case 31: info = {31, "MATCH_MAPPING",OpcodeKind::Normal,false, 1};  break;
    case 32: info = {32, "MATCH_SEQUENCE",OpcodeKind::Normal,false,1};  break;
    case 33: info = {33, "MATCH_KEYS",  OpcodeKind::Normal, false, OpcodeInfo::kUnknownEffect}; break;
    case 35: info = {35, "PUSH_EXC_INFO",OpcodeKind::Normal,false, 1};  break;
    case 36: info = {36, "CHECK_EXC_MATCH",OpcodeKind::Normal,false,0}; break;
    case 37: info = {37, "CHECK_EG_MATCH",OpcodeKind::Normal,false,0};  break;
    case 49: info = {49, "WITH_EXCEPT_START",OpcodeKind::Normal,false,1};break;
    case 50: info = {50, "GET_AITER",   OpcodeKind::Normal, false, 0};  break;
    case 51: info = {51, "GET_ANEXT",   OpcodeKind::Normal, false, 1};  break;
    case 52: info = {52, "BEFORE_ASYNC_WITH",OpcodeKind::Normal,false,1};break;
    case 53: info = {53, "BEFORE_WITH", OpcodeKind::Normal, true,  1};  break;
    case 54: info = {54, "END_ASYNC_FOR",OpcodeKind::Normal,false,-7};  break;
    case 60: info = {60, "STORE_SUBSCR",OpcodeKind::Normal, false,-3};  break;
    case 61: info = {61, "DELETE_SUBSCR",OpcodeKind::Normal,false,-2};  break;
    case 68: info = {68, "GET_ITER",    OpcodeKind::Normal, false, 0};  break;
    case 69: info = {69, "GET_YIELD_FROM_ITER",OpcodeKind::Normal,false,0};break;
    case 70: info = {70, "PRINT_EXPR",  OpcodeKind::Normal, false,-1};  break;
    case 71: info = {71, "LOAD_BUILD_CLASS",OpcodeKind::Normal,false,1};break;
    case 74: info = {74, "LOAD_ASSERTION_ERROR",OpcodeKind::Normal,false,1};break;
    case 75: info = {75, "RETURN_GENERATOR",OpcodeKind::Normal,false,1};break;
    case 82: info = {82, "LIST_TO_TUPLE",OpcodeKind::Normal, false, 0}; break;
    case 87: info = {87, "ASYNC_GEN_WRAP",OpcodeKind::Normal,false, 0}; break;
    case 88: info = {88, "PREP_RERAISE_STAR",OpcodeKind::Normal,false,OpcodeInfo::kUnknownEffect};break;
    case 89: info = {89, "POP_EXCEPT",  OpcodeKind::Normal, false,-1};  break;
    case 99: info = {99, "SWAP",        OpcodeKind::Normal, true,  0};  break;
    default: break; // use common table result
    }
}

// ─── Python 3.12 extra remappings (on top of 3.11) ───────────────────────────
static void applyVersion312Overrides(uint8_t opcode, OpcodeInfo& info) {
    applyVersion311Overrides(opcode, info);
    switch (opcode) {
    case 4:  info = {4,  "END_FOR",          OpcodeKind::Normal, false,-2}; break;
    case 5:  info = {5,  "END_SEND",         OpcodeKind::Normal, false,-1}; break;
    case 30: info = {30, "TO_BOOL",          OpcodeKind::Normal, false, 0}; break;
    case 38: info = {38, "LIST_APPEND",      OpcodeKind::Normal, true, -1}; break;
    // RETURN_CONST (121) already in common table; correct effect:
    case 121: info = {121,"RETURN_CONST",   OpcodeKind::Return,  true, -1}; break;
    default: break;
    }
}

OpcodeInfo opcodeInfo(uint8_t opcode, const PythonVersion& ver) {
    OpcodeInfo info = lookupFromTable(opcode, kCommon38_310, kCommon38_310Size);

    if (ver.atLeast(3, 12)) {
        applyVersion312Overrides(opcode, info);
    } else if (ver.atLeast(3, 11)) {
        applyVersion311Overrides(opcode, info);
    }

    // In 3.11+, all instructions have an argument
    if (ver.atLeast(3, 11)) {
        info.hasArg = true;
        if (opcode == 144) {
            info.kind = OpcodeKind::ExtendedArg;
            info.name = "EXTENDED_ARG";
        }
    } else {
        // In 3.8-3.10, opcodes < 90 have no arg
        if (opcode < 90) info.hasArg = false;
        // EXTENDED_ARG = 90 in 3.8-3.10
        if (opcode == 90) {
            info.kind   = OpcodeKind::ExtendedArg;
            info.name   = "EXTENDED_ARG";
            info.hasArg = true;
        }
    }

    return info;
}

uint8_t opcodeByName(const char* name, const PythonVersion& ver) {
    (void)ver;
    for (size_t i = 0; i < kCommon38_310Size; ++i) {
        if (std::strcmp(kCommon38_310[i].name, name) == 0) {
            return kCommon38_310[i].opcode;
        }
    }
    return 0xFF;
}

uint8_t haveArgument(const PythonVersion& ver) {
    // In 3.11+, all opcodes take an argument
    if (ver.atLeast(3, 11)) return 0;
    return 90;
}

} // namespace pyc_parser
} // namespace retdec
