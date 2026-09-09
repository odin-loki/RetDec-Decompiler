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

// Python 3.10's opcode numbering, which 3.8 and 3.9 share except for the
// entries in applyVersion38Overrides / applyVersion39Overrides below, and
// which 3.11 and 3.12 diverge from through their own override tables.
//
// Generated from CPython's Lib/opcode.py on the 3.8, 3.9, 3.10, 3.11 and 3.12
// branches; tests/pyc_parser/py_opcode_table_test.cpp pins every entry.
static const OpcodeInfo kBase310[] = {
    OP(  1, "POP_TOP",                    Normal,         false,  -1),
    OP(  2, "ROT_TWO",                    Normal,         false,  0),
    OP(  3, "ROT_THREE",                  Normal,         false,  0),
    OP(  4, "DUP_TOP",                    Normal,         false,  1),
    OP(  5, "DUP_TOP_TWO",                Normal,         false,  2),
    OP(  6, "ROT_FOUR",                   Normal,         false,  0),
    OP(  9, "NOP",                        Normal,         false,  0),
    OP( 10, "UNARY_POSITIVE",             Normal,         false,  0),
    OP( 11, "UNARY_NEGATIVE",             Normal,         false,  0),
    OP( 12, "UNARY_NOT",                  Normal,         false,  0),
    OP( 15, "UNARY_INVERT",               Normal,         false,  0),
    OP( 16, "BINARY_MATRIX_MULTIPLY",     Normal,         false,  -1),
    OP( 17, "INPLACE_MATRIX_MULTIPLY",    Normal,         false,  -1),
    OP( 19, "BINARY_POWER",               Normal,         false,  -1),
    OP( 20, "BINARY_MULTIPLY",            Normal,         false,  -1),
    OP( 22, "BINARY_MODULO",              Normal,         false,  -1),
    OP( 23, "BINARY_ADD",                 Normal,         false,  -1),
    OP( 24, "BINARY_SUBTRACT",            Normal,         false,  -1),
    OP( 25, "BINARY_SUBSCR",              Normal,         false,  -1),
    OP( 26, "BINARY_FLOOR_DIVIDE",        Normal,         false,  -1),
    OP( 27, "BINARY_TRUE_DIVIDE",         Normal,         false,  -1),
    OP( 28, "INPLACE_FLOOR_DIVIDE",       Normal,         false,  -1),
    OP( 29, "INPLACE_TRUE_DIVIDE",        Normal,         false,  -1),
    OP( 30, "GET_LEN",                    Normal,         false,  1),
    OP( 31, "MATCH_MAPPING",              Normal,         false,  1),
    OP( 32, "MATCH_SEQUENCE",             Normal,         false,  1),
    OP( 33, "MATCH_KEYS",                 Normal,         false,  OpcodeInfo::kUnknownEffect),
    OP( 34, "COPY_DICT_WITHOUT_KEYS",     Normal,         false,  0),
    OP( 49, "WITH_EXCEPT_START",          Normal,         false,  1),
    OP( 50, "GET_AITER",                  Normal,         false,  0),
    OP( 51, "GET_ANEXT",                  Normal,         false,  1),
    OP( 52, "BEFORE_ASYNC_WITH",          Normal,         false,  1),
    OP( 54, "END_ASYNC_FOR",              Normal,         false,  -7),
    OP( 55, "INPLACE_ADD",                Normal,         false,  -1),
    OP( 56, "INPLACE_SUBTRACT",           Normal,         false,  -1),
    OP( 57, "INPLACE_MULTIPLY",           Normal,         false,  -1),
    OP( 59, "INPLACE_MODULO",             Normal,         false,  -1),
    OP( 60, "STORE_SUBSCR",               Normal,         false,  -3),
    OP( 61, "DELETE_SUBSCR",              Normal,         false,  -2),
    OP( 62, "BINARY_LSHIFT",              Normal,         false,  -1),
    OP( 63, "BINARY_RSHIFT",              Normal,         false,  -1),
    OP( 64, "BINARY_AND",                 Normal,         false,  -1),
    OP( 65, "BINARY_XOR",                 Normal,         false,  -1),
    OP( 66, "BINARY_OR",                  Normal,         false,  -1),
    OP( 67, "INPLACE_POWER",              Normal,         false,  -1),
    OP( 68, "GET_ITER",                   Normal,         false,  0),
    OP( 69, "GET_YIELD_FROM_ITER",        Normal,         false,  0),
    OP( 70, "PRINT_EXPR",                 Normal,         false,  -1),
    OP( 71, "LOAD_BUILD_CLASS",           Normal,         false,  1),
    OP( 72, "YIELD_FROM",                 Normal,         false,  -1),
    OP( 73, "GET_AWAITABLE",              Normal,         false,  0),
    OP( 74, "LOAD_ASSERTION_ERROR",       Normal,         false,  1),
    OP( 75, "INPLACE_LSHIFT",             Normal,         false,  -1),
    OP( 76, "INPLACE_RSHIFT",             Normal,         false,  -1),
    OP( 77, "INPLACE_AND",                Normal,         false,  -1),
    OP( 78, "INPLACE_XOR",                Normal,         false,  -1),
    OP( 79, "INPLACE_OR",                 Normal,         false,  -1),
    OP( 82, "LIST_TO_TUPLE",              Normal,         false,  0),
    OP( 83, "RETURN_VALUE",               Return,         false,  -1),
    OP( 84, "IMPORT_STAR",                Normal,         false,  -1),
    OP( 85, "SETUP_ANNOTATIONS",          Normal,         false,  0),
    OP( 86, "YIELD_VALUE",                Normal,         false,  0),
    OP( 87, "POP_BLOCK",                  Normal,         false,  0),
    OP( 89, "POP_EXCEPT",                 Normal,         false,  -3),
    OP( 90, "STORE_NAME",                 Normal,         true,   -1),
    OP( 91, "DELETE_NAME",                Normal,         true,   0),
    OP( 92, "UNPACK_SEQUENCE",            Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP( 93, "FOR_ITER",                   Jump,           true,   1),
    OP( 94, "UNPACK_EX",                  Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP( 95, "STORE_ATTR",                 Normal,         true,   -2),
    OP( 96, "DELETE_ATTR",                Normal,         true,   -1),
    OP( 97, "STORE_GLOBAL",               Normal,         true,   -1),
    OP( 98, "DELETE_GLOBAL",              Normal,         true,   0),
    OP( 99, "ROT_N",                      Normal,         true,   0),
    OP(100, "LOAD_CONST",                 Normal,         true,   1),
    OP(101, "LOAD_NAME",                  Normal,         true,   1),
    OP(102, "BUILD_TUPLE",                Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP(103, "BUILD_LIST",                 Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP(104, "BUILD_SET",                  Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP(105, "BUILD_MAP",                  Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP(106, "LOAD_ATTR",                  Normal,         true,   0),
    OP(107, "COMPARE_OP",                 Normal,         true,   -1),
    OP(108, "IMPORT_NAME",                Import,         true,   -1),
    OP(109, "IMPORT_FROM",                Normal,         true,   1),
    OP(110, "JUMP_FORWARD",               JumpForward,    true,   0),
    OP(111, "JUMP_IF_FALSE_OR_POP",       Jump,           true,   OpcodeInfo::kUnknownEffect),
    OP(112, "JUMP_IF_TRUE_OR_POP",        Jump,           true,   OpcodeInfo::kUnknownEffect),
    OP(113, "JUMP_ABSOLUTE",              JumpAbsolute,   true,   0),
    OP(114, "POP_JUMP_IF_FALSE",          Jump,           true,   -1),
    OP(115, "POP_JUMP_IF_TRUE",           Jump,           true,   -1),
    OP(116, "LOAD_GLOBAL",                Normal,         true,   1),
    OP(117, "IS_OP",                      Normal,         true,   -1),
    OP(118, "CONTAINS_OP",                Normal,         true,   -1),
    OP(119, "RERAISE",                    Raise,          true,   OpcodeInfo::kUnknownEffect),
    OP(121, "JUMP_IF_NOT_EXC_MATCH",      JumpAbsolute,   true,   -2),
    OP(122, "SETUP_FINALLY",              Normal,         true,   6),
    OP(124, "LOAD_FAST",                  Normal,         true,   1),
    OP(125, "STORE_FAST",                 Normal,         true,   -1),
    OP(126, "DELETE_FAST",                Normal,         true,   0),
    OP(129, "GEN_START",                  Normal,         true,   -1),
    OP(130, "RAISE_VARARGS",              Raise,          true,   OpcodeInfo::kUnknownEffect),
    OP(131, "CALL_FUNCTION",              Call,           true,   OpcodeInfo::kUnknownEffect),
    OP(132, "MAKE_FUNCTION",              Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP(133, "BUILD_SLICE",                Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP(135, "LOAD_CLOSURE",               Normal,         true,   1),
    OP(136, "LOAD_DEREF",                 Normal,         true,   1),
    OP(137, "STORE_DEREF",                Normal,         true,   -1),
    OP(138, "DELETE_DEREF",               Normal,         true,   0),
    OP(141, "CALL_FUNCTION_KW",           Call,           true,   OpcodeInfo::kUnknownEffect),
    OP(142, "CALL_FUNCTION_EX",           Call,           true,   OpcodeInfo::kUnknownEffect),
    OP(143, "SETUP_WITH",                 Normal,         true,   7),
    OP(144, "EXTENDED_ARG",               ExtendedArg,    true,   0),
    OP(145, "LIST_APPEND",                Normal,         true,   -1),
    OP(146, "SET_ADD",                    Normal,         true,   -1),
    OP(147, "MAP_ADD",                    Normal,         true,   -2),
    OP(148, "LOAD_CLASSDEREF",            Normal,         true,   1),
    OP(152, "MATCH_CLASS",                Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP(154, "SETUP_ASYNC_WITH",           Normal,         true,   5),
    OP(155, "FORMAT_VALUE",               Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP(156, "BUILD_CONST_KEY_MAP",        Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP(157, "BUILD_STRING",               Normal,         true,   OpcodeInfo::kUnknownEffect),
    OP(160, "LOAD_METHOD",                Normal,         true,   1),
    OP(161, "CALL_METHOD",                Call,           true,   OpcodeInfo::kUnknownEffect),
    OP(162, "LIST_EXTEND",                Normal,         true,   -1),
    OP(163, "SET_UPDATE",                 Normal,         true,   -1),
    OP(164, "DICT_MERGE",                 Normal,         true,   -1),
    OP(165, "DICT_UPDATE",                Normal,         true,   -1),
};

static const size_t kBase310Size = sizeof(kBase310)/sizeof(kBase310[0]);

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

// Python 3.8: the opcodes it uses that 3.10 numbers differently or dropped.
static void applyVersion38Overrides(uint8_t opcode, OpcodeInfo& info) {
    switch (opcode) {
    case  53: info = { 53, "BEGIN_FINALLY", OpcodeKind::Normal, true, 6}; break;
    case  81: info = { 81, "WITH_CLEANUP_START", OpcodeKind::Normal, true, 2}; break;
    case  82: info = { 82, "WITH_CLEANUP_FINISH", OpcodeKind::Normal, true, -3}; break;
    case  88: info = { 88, "END_FINALLY", OpcodeKind::Normal, true, -6}; break;
    case 149: info = {149, "BUILD_LIST_UNPACK", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 150: info = {150, "BUILD_MAP_UNPACK", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 151: info = {151, "BUILD_MAP_UNPACK_WITH_CALL", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 152: info = {152, "BUILD_TUPLE_UNPACK", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 153: info = {153, "BUILD_SET_UNPACK", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 158: info = {158, "BUILD_TUPLE_UNPACK_WITH_CALL", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 162: info = {162, "CALL_FINALLY", OpcodeKind::Normal, true, 1}; break;
    case 163: info = {163, "POP_FINALLY", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    default: break; // the base table's entry stands
    }
}

// Python 3.9: RERAISE alone; 3.10 moved it from 48 to 119.
static void applyVersion39Overrides(uint8_t opcode, OpcodeInfo& info) {
    switch (opcode) {
    case  48: info = { 48, "RERAISE", OpcodeKind::Raise, true, OpcodeInfo::kUnknownEffect}; break;
    default: break; // the base table's entry stands
    }
}

// Python 3.11 renumbered a large part of the table.
static void applyVersion311Overrides(uint8_t opcode, OpcodeInfo& info) {
    switch (opcode) {
    case   0: info = {  0, "CACHE", OpcodeKind::Normal, true, 0}; break;
    case   2: info = {  2, "PUSH_NULL", OpcodeKind::Normal, true, 1}; break;
    case  35: info = { 35, "PUSH_EXC_INFO", OpcodeKind::Normal, true, 1}; break;
    case  36: info = { 36, "CHECK_EXC_MATCH", OpcodeKind::Normal, true, 0}; break;
    case  37: info = { 37, "CHECK_EG_MATCH", OpcodeKind::Normal, true, 0}; break;
    case  53: info = { 53, "BEFORE_WITH", OpcodeKind::Normal, true, 1}; break;
    case  75: info = { 75, "RETURN_GENERATOR", OpcodeKind::Normal, true, 1}; break;
    case  87: info = { 87, "ASYNC_GEN_WRAP", OpcodeKind::Normal, true, 0}; break;
    case  88: info = { 88, "PREP_RERAISE_STAR", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case  99: info = { 99, "SWAP", OpcodeKind::Normal, true, 0}; break;
    case 114: info = {114, "POP_JUMP_FORWARD_IF_FALSE", OpcodeKind::JumpForward, true, -1}; break;
    case 115: info = {115, "POP_JUMP_FORWARD_IF_TRUE", OpcodeKind::JumpForward, true, -1}; break;
    case 120: info = {120, "COPY", OpcodeKind::Normal, true, 1}; break;
    case 122: info = {122, "BINARY_OP", OpcodeKind::Normal, true, -1}; break;
    case 123: info = {123, "SEND", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 128: info = {128, "POP_JUMP_FORWARD_IF_NOT_NONE", OpcodeKind::JumpForward, true, -1}; break;
    case 129: info = {129, "POP_JUMP_FORWARD_IF_NONE", OpcodeKind::JumpForward, true, -1}; break;
    case 131: info = {131, "GET_AWAITABLE", OpcodeKind::Normal, true, 0}; break;
    case 134: info = {134, "JUMP_BACKWARD_NO_INTERRUPT", OpcodeKind::JumpBackward, true, 0}; break;
    case 135: info = {135, "MAKE_CELL", OpcodeKind::Normal, true, 0}; break;
    case 136: info = {136, "LOAD_CLOSURE", OpcodeKind::Normal, true, 1}; break;
    case 137: info = {137, "LOAD_DEREF", OpcodeKind::Normal, true, 1}; break;
    case 138: info = {138, "STORE_DEREF", OpcodeKind::Normal, true, -1}; break;
    case 139: info = {139, "DELETE_DEREF", OpcodeKind::Normal, true, 0}; break;
    case 140: info = {140, "JUMP_BACKWARD", OpcodeKind::JumpBackward, true, 0}; break;
    case 149: info = {149, "COPY_FREE_VARS", OpcodeKind::Normal, true, 0}; break;
    case 151: info = {151, "RESUME", OpcodeKind::Normal, true, 0}; break;
    case 166: info = {166, "PRECALL", OpcodeKind::Normal, true, 0}; break;
    case 171: info = {171, "CALL", OpcodeKind::Call, true, OpcodeInfo::kUnknownEffect}; break;
    case 172: info = {172, "KW_NAMES", OpcodeKind::Normal, true, 0}; break;
    case 173: info = {173, "POP_JUMP_BACKWARD_IF_NOT_NONE", OpcodeKind::JumpBackward, true, -1}; break;
    case 174: info = {174, "POP_JUMP_BACKWARD_IF_NONE", OpcodeKind::JumpBackward, true, -1}; break;
    case 175: info = {175, "POP_JUMP_BACKWARD_IF_FALSE", OpcodeKind::JumpBackward, true, -1}; break;
    case 176: info = {176, "POP_JUMP_BACKWARD_IF_TRUE", OpcodeKind::JumpBackward, true, -1}; break;
    default: break; // the base table's entry stands
    }
}

// Python 3.12, on top of 3.11.
static void applyVersion312Overrides(uint8_t opcode, OpcodeInfo& info) {
    applyVersion311Overrides(opcode, info);
    switch (opcode) {
    case   3: info = {  3, "INTERPRETER_EXIT", OpcodeKind::Normal, true, 0}; break;
    case   4: info = {  4, "END_FOR", OpcodeKind::Normal, true, -2}; break;
    case   5: info = {  5, "END_SEND", OpcodeKind::Normal, true, -1}; break;
    case  17: info = { 17, "RESERVED", OpcodeKind::Normal, true, 0}; break;
    case  26: info = { 26, "BINARY_SLICE", OpcodeKind::Normal, true, -2}; break;
    case  27: info = { 27, "STORE_SLICE", OpcodeKind::Normal, true, -4}; break;
    case  55: info = { 55, "CLEANUP_THROW", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case  87: info = { 87, "LOAD_LOCALS", OpcodeKind::Normal, true, 1}; break;
    case 114: info = {114, "POP_JUMP_IF_FALSE", OpcodeKind::Jump, true, -1}; break;
    case 115: info = {115, "POP_JUMP_IF_TRUE", OpcodeKind::Jump, true, -1}; break;
    case 121: info = {121, "RETURN_CONST", OpcodeKind::Return, true, 1}; break;
    case 127: info = {127, "LOAD_FAST_CHECK", OpcodeKind::Normal, true, 1}; break;
    case 128: info = {128, "POP_JUMP_IF_NOT_NONE", OpcodeKind::Jump, true, -1}; break;
    case 129: info = {129, "POP_JUMP_IF_NONE", OpcodeKind::Jump, true, -1}; break;
    case 141: info = {141, "LOAD_SUPER_ATTR", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 143: info = {143, "LOAD_FAST_AND_CLEAR", OpcodeKind::Normal, true, 1}; break;
    case 150: info = {150, "YIELD_VALUE", OpcodeKind::Normal, true, 0}; break;
    case 173: info = {173, "CALL_INTRINSIC_1", OpcodeKind::Normal, true, 0}; break;
    case 174: info = {174, "CALL_INTRINSIC_2", OpcodeKind::Normal, true, -1}; break;
    case 175: info = {175, "LOAD_FROM_DICT_OR_GLOBALS", OpcodeKind::Normal, true, 0}; break;
    case 176: info = {176, "LOAD_FROM_DICT_OR_DEREF", OpcodeKind::Normal, true, 0}; break;
    case 237: info = {237, "INSTRUMENTED_LOAD_SUPER_ATTR", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 238: info = {238, "INSTRUMENTED_POP_JUMP_IF_NONE", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 239: info = {239, "INSTRUMENTED_POP_JUMP_IF_NOT_NONE", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 240: info = {240, "INSTRUMENTED_RESUME", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 241: info = {241, "INSTRUMENTED_CALL", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 242: info = {242, "INSTRUMENTED_RETURN_VALUE", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 243: info = {243, "INSTRUMENTED_YIELD_VALUE", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 244: info = {244, "INSTRUMENTED_CALL_FUNCTION_EX", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 245: info = {245, "INSTRUMENTED_JUMP_FORWARD", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 246: info = {246, "INSTRUMENTED_JUMP_BACKWARD", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 247: info = {247, "INSTRUMENTED_RETURN_CONST", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 248: info = {248, "INSTRUMENTED_FOR_ITER", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 249: info = {249, "INSTRUMENTED_POP_JUMP_IF_FALSE", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 250: info = {250, "INSTRUMENTED_POP_JUMP_IF_TRUE", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 251: info = {251, "INSTRUMENTED_END_FOR", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 252: info = {252, "INSTRUMENTED_END_SEND", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 253: info = {253, "INSTRUMENTED_INSTRUCTION", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    case 254: info = {254, "INSTRUMENTED_LINE", OpcodeKind::Normal, true, OpcodeInfo::kUnknownEffect}; break;
    default: break; // the base table's entry stands
    }
}

OpcodeInfo opcodeInfo(uint8_t opcode, const PythonVersion& ver) {
    OpcodeInfo info = lookupFromTable(opcode, kBase310, kBase310Size);

    if (ver.atLeast(3, 12)) {
        applyVersion312Overrides(opcode, info);
    } else if (ver.atLeast(3, 11)) {
        applyVersion311Overrides(opcode, info);
    } else if (ver.atLeast(3, 10)) {
        // The base table is 3.10.
    } else if (ver.atLeast(3, 9)) {
        applyVersion39Overrides(opcode, info);
    } else {
        applyVersion38Overrides(opcode, info);
    }

    if (ver.atLeast(3, 11)) {
        // From 3.11 every instruction carries an argument byte.
        info.hasArg = true;
    } else if (opcode < 90) {
        // Below HAVE_ARGUMENT, which is 90, nothing does.
        //
        // What used to stand here also rewrote opcode 90 itself into
        // EXTENDED_ARG "because EXTENDED_ARG = 90 in 3.8-3.10". It is not: 90
        // is STORE_NAME and *is* HAVE_ARGUMENT, and EXTENDED_ARG is 144 in
        // every version this parser reads. 0x5A and 0x90 are not the same
        // number. The cost was every module-level and class-level assignment
        // in a 3.8-3.10 .pyc: STORE_NAME was swallowed as a prefix, its
        // operand was shifted into the extended-argument accumulator and
        // OR-ed into the next instruction's, and a genuine EXTENDED_ARG was
        // never accumulated at all.
        info.hasArg = false;
    }

    return info;
}

uint8_t opcodeByName(const char* name, const PythonVersion& ver) {
    (void)ver;
    for (size_t i = 0; i < kBase310Size; ++i) {
        if (std::strcmp(kBase310[i].name, name) == 0) {
            return kBase310[i].opcode;
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
