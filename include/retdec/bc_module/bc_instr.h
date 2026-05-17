/**
 * @file include/retdec/bc_module/bc_instr.h
 * @brief BcInstruction — unified bytecode instruction model.
 *
 * ## Opcode coverage
 *
 * The opcode enum is the union of all significant operations across the
 * supported managed platforms, grouped by semantic category.  Lifters map
 * each platform-specific bytecode onto the appropriate BcOpcode; the source
 * emitter then renders it idiomatically in the target language.
 *
 * ## Stack effect
 *
 * Every opcode has a statically known or computable stack effect:
 *   - `stackPop`  — number of values consumed from the operand stack.
 *   - `stackPush` — number of values pushed onto the operand stack.
 *
 * For instructions whose effect depends on the method descriptor (e.g.
 * `Invoke*`), the effect is computed at lift-time and stored in the
 * instruction itself rather than derived from the opcode alone.
 *
 * ## Operands
 *
 * Each instruction carries zero or more typed operands stored in a
 * `std::vector<BcOperand>`.  The variant covers the union of all operand
 * kinds that appear across JVM, CLR, Wasm, Python, and Lua bytecodes.
 */

#ifndef RETDEC_BC_MODULE_BC_INSTR_H
#define RETDEC_BC_MODULE_BC_INSTR_H

#include "retdec/bc_module/bc_type.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace retdec {
namespace bc_module {

// ─── Operand kinds ────────────────────────────────────────────────────────────

/** Integer constant operand (fits in int64). */
struct BcIntOperand   { int64_t value = 0; };
/** Floating-point constant operand. */
struct BcFloatOperand { double  value = 0.0; };
/** Interned string operand (string pool index or literal value). */
struct BcStringOperand{ std::string value; };
/** Type reference operand (for new, checkcast, instanceof, etc.). */
struct BcTypeOperand  { BcType type; };
/** Fully-qualified method reference: "owner.name:descriptor". */
struct BcMethodRef {
    std::string owner;       ///< "java/lang/StringBuilder"
    std::string name;        ///< "append"
    BcFuncType  descriptor;
    bool        isInterface = false;
};
/** Field reference: "owner.name:type". */
struct BcFieldRef {
    std::string owner;
    std::string name;
    BcType      type;
    bool        isStatic = false;
};
/** Local variable slot index (JVM) or variable index (Wasm/CLR). */
struct BcLocalOperand { uint32_t index = 0; };
/** Branch target: block index within the same BcMethod. */
struct BcBlockOperand { uint32_t blockId = 0; };
/** Switch table: (key → blockId) pairs + default block. */
struct BcSwitchTable  {
    std::vector<std::pair<int64_t, uint32_t>> cases;
    uint32_t defaultBlock = 0;
};

using BcOperand = std::variant<
    BcIntOperand,
    BcFloatOperand,
    BcStringOperand,
    BcTypeOperand,
    BcMethodRef,
    BcFieldRef,
    BcLocalOperand,
    BcBlockOperand,
    BcSwitchTable
>;

// ─── Opcode enum ─────────────────────────────────────────────────────────────

enum class BcOpcode : uint16_t {
    // ── Constants / Loads ──────────────────────────────────────────────────
    Nop,
    PushNull,          ///< push null reference
    PushInt,           ///< push int32 immediate (BcIntOperand)
    PushLong,          ///< push int64 immediate
    PushFloat,         ///< push float32 immediate (BcFloatOperand)
    PushDouble,        ///< push float64 immediate
    PushString,        ///< push string constant (BcStringOperand)
    PushTrue,          ///< push boolean true
    PushFalse,         ///< push boolean false
    LoadClass,         ///< push Class<T> constant (BcTypeOperand) — JVM ldc class
    LoadLocal,         ///< load local variable (BcLocalOperand)
    StoreLocal,        ///< store local variable (BcLocalOperand)
    // ── Arithmetic ─────────────────────────────────────────────────────────
    Add, Sub, Mul, Div, Rem, Neg,
    Shl, Shr, UShr, And, Or, Xor,
    LAnd, LOr,          ///< short-circuit logical AND / OR (high-level IR)
    // ── Floating point ─────────────────────────────────────────────────────
    FAdd, FSub, FMul, FDiv, FRem, FNeg,
    // ── Comparison ─────────────────────────────────────────────────────────
    CmpEq, CmpNe, CmpLt, CmpGe, CmpGt, CmpLe,
    FCmpL, FCmpG,        ///< JVM fcmpl / fcmpg (NaN handling)
    LCmp,                ///< JVM lcmp -- push -1/0/1 for long compare
    IsNull, IsNotNull,
    Instanceof,          ///< BcTypeOperand
    // ── Type conversion ─────────────────────────────────────────────────────
    I2L, I2F, I2D, L2I, L2F, L2D, F2I, F2L, F2D, D2I, D2L, D2F,
    I2B, I2C, I2S,       ///< JVM narrowing conversions
    CheckCast,           ///< BcTypeOperand — throws ClassCastException on failure
    Box, Unbox,          ///< CLR value-type boxing / unboxing (BcTypeOperand)
    // ── Control flow ────────────────────────────────────────────────────────
    Goto,                ///< BcBlockOperand
    IfTrue,              ///< conditional branch if top-of-stack != 0 (BcBlockOperand)
    IfFalse,             ///< conditional branch if top-of-stack == 0
    IfNull,              ///< branch if null
    IfNonNull,           ///< branch if not null
    IfEq, IfNe, IfLt, IfGe, IfGt, IfLe,    ///< compare-and-branch
    TableSwitch,         ///< BcSwitchTable
    LookupSwitch,        ///< BcSwitchTable (sparse)
    Return,              ///< void return
    ReturnValue,         ///< return top-of-stack
    Throw,               ///< throw top-of-stack (must be Throwable / Exception)
    // ── Method invocation ────────────────────────────────────────────────────
    InvokeVirtual,       ///< BcMethodRef — virtual dispatch
    InvokeInterface,     ///< BcMethodRef — interface dispatch
    InvokeSpecial,       ///< BcMethodRef — <init>, super, private
    InvokeStatic,        ///< BcMethodRef — static method
    InvokeDynamic,       ///< BcMethodRef — lambda / closure bootstrap
    // CLR equivalents:
    Callvirt,            ///< CLR callvirt
    Call,                ///< CLR call (non-virtual)
    // ── Field access ────────────────────────────────────────────────────────
    GetField,            ///< BcFieldRef (instance)
    PutField,            ///< BcFieldRef (instance)
    GetStatic,           ///< BcFieldRef (static)
    PutStatic,           ///< BcFieldRef (static)
    // ── Object / array creation ──────────────────────────────────────────────
    New,                 ///< BcTypeOperand — allocate; does NOT call <init>
    NewArray,            ///< BcTypeOperand + length on stack
    MultiNewArray,       ///< BcTypeOperand + dimension count (BcIntOperand)
    ArrayLength,
    ArrayLoad,           ///< stack: [array, index] → [value]
    ArrayStore,          ///< stack: [array, index, value] → []
    // ── Stack manipulation ────────────────────────────────────────────────────
    Pop, Pop2,           ///< discard one/two stack slots
    Dup, DupX1, DupX2,  ///< JVM dup family
    Dup2, Dup2X1, Dup2X2,
    Swap,
    // ── Monitor / synchronisation ─────────────────────────────────────────────
    MonitorEnter,
    MonitorExit,
    // ── Python-specific ───────────────────────────────────────────────────────
    PyLoadName,          ///< LOAD_NAME, LOAD_FAST, LOAD_DEREF (BcStringOperand)
    PyStoreName,         ///< STORE_NAME, STORE_FAST (BcStringOperand)
    PyBuildList,         ///< BUILD_LIST (BcIntOperand = count)
    PyBuildDict,         ///< BUILD_MAP
    PyBuildTuple,        ///< BUILD_TUPLE
    PyBuildSet,          ///< BUILD_SET
    PyForIter,           ///< FOR_ITER (BcBlockOperand = exit)
    PyGetIter,           ///< GET_ITER
    PyImport,            ///< IMPORT_NAME (BcStringOperand)
    PyYield,             ///< YIELD_VALUE
    PyAwait,             ///< GET_AWAITABLE
    PyFormat,            ///< FORMAT_VALUE
    // ── Wasm-specific ─────────────────────────────────────────────────────────
    WasmMemorySize,      ///< memory.size
    WasmMemoryGrow,      ///< memory.grow
    WasmLoad,            ///< i32.load, i64.load, f32.load, f64.load + alignment
    WasmStore,           ///< i32.store, i64.store, …
    WasmSelect,          ///< select
    WasmDrop,            ///< drop
    WasmLocalTee,        ///< local.tee (BcLocalOperand)
    WasmRefNull,         ///< ref.null
    WasmRefIsNull,       ///< ref.is_null
    WasmTableGet,        ///< table.get
    WasmTableSet,        ///< table.set
    // ── Lua-specific ──────────────────────────────────────────────────────────
    LuaConcat,           ///< .. operator (BcIntOperand = count)
    LuaLength,           ///< # operator
    LuaNewTable,         ///< {} (BcIntOperand = hint sizes)
    LuaGetField,         ///< t[k] — key is BcStringOperand
    LuaSetField,         ///< t[k] = v — key is BcStringOperand
    LuaGetTable,         ///< t[k] — key on stack
    LuaSetTable,         ///< t[k] = v — key and value on stack
    LuaCall,             ///< CALL (BcIntOperand = nargs, nresults)
    LuaTailCall,         ///< TAILCALL
    LuaReturn,           ///< RETURN (BcIntOperand = nresults)
    LuaSelf,             ///< SELF (method call sugar: pushes table + method)
    LuaClosure,          ///< CLOSURE (BcMethodRef = proto)
    LuaVarArg,           ///< VARARG (BcIntOperand = count, 0 = all)
    LuaClose,            ///< JMP + CLOSE: close upvalues (BcLocalOperand)
    // ── CLR-specific ──────────────────────────────────────────────────────────
    Ldstr,               ///< CLR ldstr (BcStringOperand)
    LdToken,             ///< CLR ldtoken (BcTypeOperand or BcMethodRef)
    Sizeof,              ///< CLR sizeof (BcTypeOperand)
    Initobj,             ///< CLR initobj (zero-init value type)
    Cpobj,               ///< CLR cpobj
    Ldobj, Stobj,        ///< CLR ldobj / stobj
    Refanytype,          ///< CLR refanytype / refanyval
    Mkrefany,            ///< CLR mkrefany
    Arglist,             ///< CLR arglist (variable arg list)
    Tail,                ///< CLR tail. prefix
    Constrained,         ///< CLR constrained. prefix (BcTypeOperand)
    Readonly,            ///< CLR readonly. prefix

    // ── Dalvik/DEX-specific ──────────────────────────────────────────────────
    // Register-based ops: operands include explicit register numbers
    // (as BcLocalOperand) rather than an implicit operand stack.
    DALVIK_NOP,
    DALVIK_MOVE,            ///< vA = vB  (int/obj)
    DALVIK_MOVE_WIDE,       ///< vA,vA+1 = vB,vB+1 (long/double)
    DALVIK_MOVE_RESULT,     ///< vA = result of last invoke
    DALVIK_MOVE_EXCEPTION,  ///< vA = current exception
    DALVIK_RETURN_VOID,     ///< return (void)
    DALVIK_RETURN,          ///< return vA (int/obj)
    DALVIK_RETURN_WIDE,     ///< return vA,vA+1 (long/double)
    DALVIK_CONST,           ///< vA = #lit  (int/float, BcIntOperand or BcFloatOperand)
    DALVIK_CONST_WIDE,      ///< vA,vA+1 = #lit64
    DALVIK_CONST_STRING,    ///< vA = string@idx
    DALVIK_CONST_CLASS,     ///< vA = class@idx (BcTypeOperand)
    DALVIK_MONITOR_ENTER,   ///< monitorenter vA
    DALVIK_MONITOR_EXIT,    ///< monitorexit vA
    DALVIK_CHECK_CAST,      ///< check-cast vA, type@idx
    DALVIK_INSTANCE_OF,     ///< vA = vB instanceof type@idx
    DALVIK_ARRAY_LENGTH,    ///< vA = length(vB)
    DALVIK_NEW_INSTANCE,    ///< vA = new type@idx
    DALVIK_NEW_ARRAY,       ///< vA = new type@idx[vB]
    DALVIK_FILLED_NEW_ARRAY,///< result = new type@idx{vC,vD,...}
    DALVIK_FILL_ARRAY_DATA, ///< fill vA from payload at offset
    DALVIK_THROW,           ///< throw vA
    DALVIK_GOTO,            ///< goto offset (BcBlockOperand)
    DALVIK_SWITCH,          ///< packed-switch or sparse-switch vA, payload@off
    DALVIK_CMP,             ///< vA = cmp(vB, vC)  [float/long/double compare]
    DALVIK_IF,              ///< if vA op vB goto label
    DALVIK_IF_Z,            ///< if vA op 0 goto label
    DALVIK_AGET,            ///< vA = vB[vC]
    DALVIK_APUT,            ///< vB[vC] = vA
    DALVIK_IGET,            ///< vA = vB.field@idx
    DALVIK_IPUT,            ///< vB.field@idx = vA
    DALVIK_SGET,            ///< vA = class.field@idx
    DALVIK_SPUT,            ///< class.field@idx = vA
    DALVIK_INVOKE_VIRTUAL,  ///< invoke-virtual {vC,...}, method@idx
    DALVIK_INVOKE_SUPER,    ///< invoke-super
    DALVIK_INVOKE_DIRECT,   ///< invoke-direct (<init>, private)
    DALVIK_INVOKE_STATIC,   ///< invoke-static
    DALVIK_INVOKE_INTERFACE,///< invoke-interface
    DALVIK_INVOKE_CUSTOM,   ///< invoke-custom (DEX 038+)
    DALVIK_RSUB_INT,        ///< vA = #lit - vB
    DALVIK_NEG_INT,   DALVIK_NOT_INT,
    DALVIK_NEG_LONG,  DALVIK_NOT_LONG,
    DALVIK_NEG_FLOAT, DALVIK_NEG_DOUBLE,
    DALVIK_INT_TO_LONG,   DALVIK_INT_TO_FLOAT,  DALVIK_INT_TO_DOUBLE,
    DALVIK_LONG_TO_INT,   DALVIK_LONG_TO_FLOAT, DALVIK_LONG_TO_DOUBLE,
    DALVIK_FLOAT_TO_INT,  DALVIK_FLOAT_TO_LONG, DALVIK_FLOAT_TO_DOUBLE,
    DALVIK_DOUBLE_TO_INT, DALVIK_DOUBLE_TO_LONG,DALVIK_DOUBLE_TO_FLOAT,
    DALVIK_INT_TO_BYTE, DALVIK_INT_TO_CHAR, DALVIK_INT_TO_SHORT,
    DALVIK_ADD_INT,  DALVIK_SUB_INT,  DALVIK_MUL_INT,  DALVIK_DIV_INT,  DALVIK_REM_INT,
    DALVIK_AND_INT,  DALVIK_OR_INT,   DALVIK_XOR_INT,
    DALVIK_SHL_INT,  DALVIK_SHR_INT,  DALVIK_USHR_INT,
    DALVIK_ADD_LONG, DALVIK_SUB_LONG, DALVIK_MUL_LONG, DALVIK_DIV_LONG, DALVIK_REM_LONG,
    DALVIK_AND_LONG, DALVIK_OR_LONG,  DALVIK_XOR_LONG,
    DALVIK_SHL_LONG, DALVIK_SHR_LONG, DALVIK_USHR_LONG,
    DALVIK_ADD_FLOAT, DALVIK_SUB_FLOAT, DALVIK_MUL_FLOAT, DALVIK_DIV_FLOAT, DALVIK_REM_FLOAT,
    DALVIK_ADD_DOUBLE,DALVIK_SUB_DOUBLE,DALVIK_MUL_DOUBLE,DALVIK_DIV_DOUBLE,DALVIK_REM_DOUBLE,

    // ── .NET CIL opcodes (ECMA-335 §III) ────────────────────────────────────
    DOTNET_NOP, DOTNET_BREAK,
    // Load / Store
    DOTNET_LDARG_0, DOTNET_LDARG_1, DOTNET_LDARG_2, DOTNET_LDARG_3,
    DOTNET_LDLOC_0, DOTNET_LDLOC_1, DOTNET_LDLOC_2, DOTNET_LDLOC_3,
    DOTNET_STLOC_0, DOTNET_STLOC_1, DOTNET_STLOC_2, DOTNET_STLOC_3,
    DOTNET_LDARG_S, DOTNET_LDARGA_S, DOTNET_STARG_S,
    DOTNET_LDLOC_S, DOTNET_LDLOCA_S, DOTNET_STLOC_S,
    DOTNET_LDARG, DOTNET_LDARGA, DOTNET_STARG,
    DOTNET_LDLOC, DOTNET_LDLOCA, DOTNET_STLOC,
    DOTNET_LDNULL,
    DOTNET_LDC_I4_M1, DOTNET_LDC_I4_0, DOTNET_LDC_I4_1,
    DOTNET_LDC_I4_2,  DOTNET_LDC_I4_3, DOTNET_LDC_I4_4,
    DOTNET_LDC_I4_5,  DOTNET_LDC_I4_6, DOTNET_LDC_I4_7, DOTNET_LDC_I4_8,
    DOTNET_LDC_I4_S, DOTNET_LDC_I4, DOTNET_LDC_I8, DOTNET_LDC_R4, DOTNET_LDC_R8,
    DOTNET_DUP, DOTNET_POP,
    // Arithmetic
    DOTNET_ADD, DOTNET_SUB, DOTNET_MUL, DOTNET_DIV, DOTNET_DIV_UN,
    DOTNET_REM, DOTNET_REM_UN, DOTNET_NEG,
    DOTNET_AND, DOTNET_OR, DOTNET_XOR, DOTNET_NOT,
    DOTNET_SHL, DOTNET_SHR, DOTNET_SHR_UN,
    DOTNET_ADD_OVF, DOTNET_ADD_OVF_UN, DOTNET_MUL_OVF, DOTNET_MUL_OVF_UN,
    DOTNET_SUB_OVF, DOTNET_SUB_OVF_UN,
    // Comparison
    DOTNET_CEQ, DOTNET_CGT, DOTNET_CGT_UN, DOTNET_CLT, DOTNET_CLT_UN,
    // Conversion
    DOTNET_CONV_I1, DOTNET_CONV_U1, DOTNET_CONV_I2, DOTNET_CONV_U2,
    DOTNET_CONV_I4, DOTNET_CONV_U4, DOTNET_CONV_I8, DOTNET_CONV_U8,
    DOTNET_CONV_R4, DOTNET_CONV_R8, DOTNET_CONV_I, DOTNET_CONV_U,
    DOTNET_CONV_R_UN,
    DOTNET_CONV_OVF_I, DOTNET_CONV_OVF_U,
    DOTNET_CONV_OVF_I1, DOTNET_CONV_OVF_U1, DOTNET_CONV_OVF_I2, DOTNET_CONV_OVF_U2,
    DOTNET_CONV_OVF_I4, DOTNET_CONV_OVF_U4, DOTNET_CONV_OVF_I8, DOTNET_CONV_OVF_U8,
    DOTNET_CONV_OVF_I1_UN, DOTNET_CONV_OVF_U1_UN,
    DOTNET_CONV_OVF_I2_UN, DOTNET_CONV_OVF_U2_UN,
    DOTNET_CONV_OVF_I4_UN, DOTNET_CONV_OVF_U4_UN,
    DOTNET_CONV_OVF_I8_UN, DOTNET_CONV_OVF_U8_UN,
    DOTNET_CONV_OVF_I_UN,  DOTNET_CONV_OVF_U_UN,
    // Branching
    DOTNET_BR, DOTNET_BR_S,
    DOTNET_BRTRUE, DOTNET_BRTRUE_S, DOTNET_BRFALSE, DOTNET_BRFALSE_S,
    DOTNET_BEQ, DOTNET_BEQ_S, DOTNET_BNE_UN, DOTNET_BNE_UN_S,
    DOTNET_BGE, DOTNET_BGE_S, DOTNET_BGE_UN, DOTNET_BGE_UN_S,
    DOTNET_BGT, DOTNET_BGT_S, DOTNET_BGT_UN, DOTNET_BGT_UN_S,
    DOTNET_BLE, DOTNET_BLE_S, DOTNET_BLE_UN, DOTNET_BLE_UN_S,
    DOTNET_BLT, DOTNET_BLT_S, DOTNET_BLT_UN, DOTNET_BLT_UN_S,
    DOTNET_SWITCH,
    // Method calls
    DOTNET_CALL, DOTNET_CALLI, DOTNET_CALLVIRT, DOTNET_TAIL_CALL,
    DOTNET_RET,
    // Objects and types
    DOTNET_NEWOBJ, DOTNET_NEWARR, DOTNET_INITOBJ,
    DOTNET_CASTCLASS, DOTNET_ISINST,
    DOTNET_LDTOKEN, DOTNET_BOX, DOTNET_UNBOX, DOTNET_UNBOX_ANY,
    DOTNET_SIZEOF, DOTNET_REFANYTYPE, DOTNET_REFANYVAL, DOTNET_MKREFANY, DOTNET_CKFINITE,
    // Fields
    DOTNET_LDFLD, DOTNET_LDSFLD, DOTNET_LDFLDA, DOTNET_LDSFLDA,
    DOTNET_STFLD, DOTNET_STSFLD,
    // Arrays
    DOTNET_LDELEM, DOTNET_LDELEM_I1, DOTNET_LDELEM_U1,
    DOTNET_LDELEM_I2, DOTNET_LDELEM_U2, DOTNET_LDELEM_I4, DOTNET_LDELEM_U4,
    DOTNET_LDELEM_I8,  DOTNET_LDELEM_I, DOTNET_LDELEM_R4, DOTNET_LDELEM_R8,
    DOTNET_LDELEM_REF,
    DOTNET_STELEM, DOTNET_STELEM_I1, DOTNET_STELEM_I2, DOTNET_STELEM_I4,
    DOTNET_STELEM_I8,  DOTNET_STELEM_I, DOTNET_STELEM_R4, DOTNET_STELEM_R8,
    DOTNET_STELEM_REF,
    DOTNET_LDELEMA, DOTNET_LDLEN,
    // Pointers and unsafe
    DOTNET_LDIND_I1, DOTNET_LDIND_U1, DOTNET_LDIND_I2, DOTNET_LDIND_U2,
    DOTNET_LDIND_I4, DOTNET_LDIND_U4, DOTNET_LDIND_I8, DOTNET_LDIND_I,
    DOTNET_LDIND_R4, DOTNET_LDIND_R8, DOTNET_LDIND_REF,
    DOTNET_STIND_REF, DOTNET_STIND_I1, DOTNET_STIND_I2, DOTNET_STIND_I4,
    DOTNET_STIND_I8, DOTNET_STIND_I, DOTNET_STIND_R4, DOTNET_STIND_R8,
    DOTNET_CPOBJ, DOTNET_LDOBJ, DOTNET_STOBJ, DOTNET_CPBLK, DOTNET_INITBLK,
    DOTNET_LOCALLOC,
    // Exceptions
    DOTNET_THROW, DOTNET_RETHROW, DOTNET_ENDFINALLY, DOTNET_ENDFILTER,
    DOTNET_LEAVE, DOTNET_LEAVE_S,
    // Misc
    DOTNET_LDSTR, DOTNET_LDFTN, DOTNET_LDVIRTFTN,
    DOTNET_JMPI, DOTNET_ARGLIST,
    DOTNET_VOLATILE, DOTNET_UNALIGNED, DOTNET_CONSTRAINED, DOTNET_READONLY,
    DOTNET_NO,

    // ── Python CPython bytecode (PYTHON_*) ────────────────────────────────────
    // Stack and general
    PYTHON_POP_TOP, PYTHON_ROT_TWO, PYTHON_ROT_THREE, PYTHON_ROT_FOUR, PYTHON_ROT_N,
    PYTHON_DUP_TOP, PYTHON_DUP_TOP_TWO, PYTHON_COPY, PYTHON_PUSH_NULL,
    PYTHON_NOP, PYTHON_RESUME, PYTHON_CACHE,

    // Unary
    PYTHON_UNARY_POSITIVE, PYTHON_UNARY_NEGATIVE, PYTHON_UNARY_NOT, PYTHON_UNARY_INVERT,

    // Binary (3.8-3.10)
    PYTHON_BINARY_ADD, PYTHON_BINARY_SUBTRACT, PYTHON_BINARY_MULTIPLY,
    PYTHON_BINARY_FLOOR_DIVIDE, PYTHON_BINARY_TRUE_DIVIDE, PYTHON_BINARY_MODULO,
    PYTHON_BINARY_POWER, PYTHON_BINARY_LSHIFT, PYTHON_BINARY_RSHIFT,
    PYTHON_BINARY_AND, PYTHON_BINARY_OR, PYTHON_BINARY_XOR,
    PYTHON_BINARY_MATRIX_MULTIPLY,
    PYTHON_BINARY_SUBSCR,
    // Binary unified (3.11+)
    PYTHON_BINARY_OP,
    // Inplace
    PYTHON_INPLACE_ADD, PYTHON_INPLACE_SUBTRACT, PYTHON_INPLACE_MULTIPLY,
    PYTHON_INPLACE_FLOOR_DIVIDE, PYTHON_INPLACE_TRUE_DIVIDE, PYTHON_INPLACE_MODULO,
    PYTHON_INPLACE_POWER, PYTHON_INPLACE_LSHIFT, PYTHON_INPLACE_RSHIFT,
    PYTHON_INPLACE_AND, PYTHON_INPLACE_OR, PYTHON_INPLACE_XOR,
    PYTHON_INPLACE_MATRIX_MULTIPLY,
    // Store subscr / delete
    PYTHON_STORE_SUBSCR, PYTHON_DELETE_SUBSCR,

    // Load / store / delete variables
    PYTHON_LOAD_CONST,
    PYTHON_LOAD_FAST, PYTHON_STORE_FAST, PYTHON_DELETE_FAST,
    PYTHON_LOAD_FAST_CHECK, PYTHON_LOAD_FAST_AND_CLEAR,
    PYTHON_LOAD_NAME, PYTHON_STORE_NAME, PYTHON_DELETE_NAME,
    PYTHON_LOAD_GLOBAL, PYTHON_STORE_GLOBAL, PYTHON_DELETE_GLOBAL,
    PYTHON_LOAD_ATTR, PYTHON_STORE_ATTR, PYTHON_DELETE_ATTR,
    PYTHON_LOAD_DEREF, PYTHON_STORE_DEREF, PYTHON_DELETE_DEREF,
    PYTHON_LOAD_CLOSURE, PYTHON_LOAD_CLASSDEREF,
    PYTHON_COPY_FREE_VARS,

    // Compare / is / in
    PYTHON_COMPARE_OP, PYTHON_IS_OP, PYTHON_CONTAINS_OP,

    // Jumps
    PYTHON_JUMP_FORWARD, PYTHON_JUMP_ABSOLUTE, PYTHON_JUMP_BACKWARD,
    PYTHON_JUMP_BACKWARD_NO_INTERRUPT,
    PYTHON_POP_JUMP_IF_TRUE, PYTHON_POP_JUMP_IF_FALSE,
    PYTHON_POP_JUMP_IF_NONE, PYTHON_POP_JUMP_IF_NOT_NONE,
    PYTHON_POP_JUMP_FORWARD_IF_TRUE, PYTHON_POP_JUMP_FORWARD_IF_FALSE,
    PYTHON_POP_JUMP_FORWARD_IF_NONE, PYTHON_POP_JUMP_FORWARD_IF_NOT_NONE,
    PYTHON_POP_JUMP_BACKWARD_IF_TRUE, PYTHON_POP_JUMP_BACKWARD_IF_FALSE,
    PYTHON_POP_JUMP_BACKWARD_IF_NONE, PYTHON_POP_JUMP_BACKWARD_IF_NOT_NONE,
    PYTHON_JUMP_IF_TRUE_OR_POP, PYTHON_JUMP_IF_FALSE_OR_POP,
    PYTHON_FOR_ITER, PYTHON_SEND,

    // Return / raise
    PYTHON_RETURN_VALUE, PYTHON_RETURN_CONST, PYTHON_RETURN_GENERATOR,
    PYTHON_RAISE_VARARGS, PYTHON_RERAISE,

    // Calls (3.8-3.10)
    PYTHON_CALL_FUNCTION, PYTHON_CALL_FUNCTION_KW, PYTHON_CALL_FUNCTION_EX,
    PYTHON_CALL_METHOD, PYTHON_LOAD_METHOD,
    // Calls (3.11+)
    PYTHON_CALL, PYTHON_PRECALL, PYTHON_KW_NAMES, PYTHON_PUSH_EXC_INFO,
    PYTHON_CALL_INTRINSIC_1, PYTHON_CALL_INTRINSIC_2,

    // Functions
    PYTHON_MAKE_FUNCTION, PYTHON_BUILD_SLICE,

    // Sequence / collection builders
    PYTHON_BUILD_TUPLE, PYTHON_BUILD_LIST, PYTHON_BUILD_SET, PYTHON_BUILD_MAP,
    PYTHON_BUILD_CONST_KEY_MAP, PYTHON_BUILD_STRING,
    PYTHON_LIST_APPEND, PYTHON_SET_ADD, PYTHON_MAP_ADD,
    PYTHON_LIST_EXTEND, PYTHON_SET_UPDATE, PYTHON_DICT_MERGE, PYTHON_DICT_UPDATE,

    // Unpacking
    PYTHON_UNPACK_SEQUENCE, PYTHON_UNPACK_EX,

    // Import
    PYTHON_IMPORT_NAME, PYTHON_IMPORT_FROM, PYTHON_IMPORT_STAR,

    // Exception handling
    PYTHON_POP_EXCEPT, PYTHON_POP_BLOCK, PYTHON_END_FINALLY,
    PYTHON_SETUP_WITH, PYTHON_BEFORE_WITH, PYTHON_WITH_EXCEPT_START,
    PYTHON_BEFORE_ASYNC_WITH, PYTHON_END_ASYNC_FOR,
    PYTHON_CHECK_EXC_MATCH, PYTHON_CLEANUP_THROW, PYTHON_STOPITERATION_ERROR,

    // Generators / async
    PYTHON_YIELD_VALUE, PYTHON_YIELD_FROM,
    PYTHON_GET_ITER, PYTHON_GET_YIELD_FROM_ITER,
    PYTHON_GET_AWAITABLE, PYTHON_GET_AITER, PYTHON_GET_ANEXT,
    PYTHON_ASYNC_GEN_WRAP,

    // Pattern matching (3.10+)
    PYTHON_MATCH_CLASS, PYTHON_MATCH_SEQUENCE, PYTHON_MATCH_MAPPING,

    // Misc
    PYTHON_FORMAT_VALUE, PYTHON_PRINT_EXPR,
    PYTHON_LOAD_BUILD_CLASS, PYTHON_SETUP_ANNOTATIONS, PYTHON_LOAD_ASSERTION_ERROR,
    PYTHON_EXTENDED_ARG,
    PYTHON_UNKNOWN,  ///< Unrecognised / adaptive opcode
};

// ─── Stack-effect descriptor ──────────────────────────────────────────────────

/**
 * Describes how an opcode modifies the operand stack.
 * Negative pop/push counts signal "variable" — must be computed from the
 * instruction's actual operands (e.g. Invoke* depends on method descriptor).
 */
struct BcStackEffect {
    int8_t pop  = 0;   ///< slots consumed  (≥0 or -1 for "variable")
    int8_t push = 0;   ///< slots produced  (≥0 or -1 for "variable")

    int net() const { return (int)push - (int)pop; }
    bool isVariable() const { return pop < 0 || push < 0; }
};

/// Look up the static stack effect for an opcode.
/// Returns {-1,-1} for opcodes with variable effects.
BcStackEffect stackEffectOf(BcOpcode op) noexcept;

// ─── BcInstruction ────────────────────────────────────────────────────────────

/**
 * @brief One instruction in a BcMethod's instruction list.
 *
 * Instructions are stored in a flat vector inside each BcBasicBlock.
 * The `id` is a method-level sequential index assigned by the lifter.
 */
struct BcInstruction {
    uint32_t            id       = 0;     ///< Sequential instruction index
    uint32_t            offset   = 0;     ///< Bytecode offset in original binary
    BcOpcode            opcode   = BcOpcode::Nop;
    std::vector<BcOperand> operands;

    /// Pre-computed stack effect (set by lifter; may differ from the static table
    /// for variable-effect opcodes like Invoke*).
    BcStackEffect effect;

    /// Source location (line number, -1 if unavailable).
    int32_t line = -1;

    // Operand accessors (convenience).
    int64_t     intOp(size_t idx = 0)    const;
    double      floatOp(size_t idx = 0)  const;
    std::string stringOp(size_t idx = 0) const;
    const BcType&      typeOp(size_t idx = 0)   const;
    const BcMethodRef& methodOp(size_t idx = 0) const;
    const BcFieldRef&  fieldOp(size_t idx = 0)  const;
    uint32_t    localOp(size_t idx = 0)  const;
    uint32_t    blockOp(size_t idx = 0)  const;
};

} // namespace bc_module
} // namespace retdec

#endif // RETDEC_BC_MODULE_BC_INSTR_H
