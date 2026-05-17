/**
 * @file src/bc_module/bc_instr.cpp
 * @brief BcInstruction — stack-effect table and operand accessors.
 */

#include "retdec/bc_module/bc_instr.h"

#include <stdexcept>

namespace retdec {
namespace bc_module {

// ─── Stack-effect table ───────────────────────────────────────────────────────

BcStackEffect stackEffectOf(BcOpcode op) noexcept {
    // pop, push
    using Op = BcOpcode;
    switch (op) {
    // Constants / Loads
    case Op::Nop:           return {0, 0};
    case Op::PushNull:      return {0, 1};
    case Op::PushInt:       return {0, 1};
    case Op::PushLong:      return {0, 1};
    case Op::PushFloat:     return {0, 1};
    case Op::PushDouble:    return {0, 1};
    case Op::PushString:    return {0, 1};
    case Op::PushTrue:      return {0, 1};
    case Op::PushFalse:     return {0, 1};
    case Op::LoadClass:     return {0, 1};
    case Op::LoadLocal:     return {0, 1};
    case Op::StoreLocal:    return {1, 0};
    // Arithmetic
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Rem:
    case Op::Shl: case Op::Shr: case Op::UShr:
    case Op::And: case Op::Or:  case Op::Xor:
    case Op::FAdd: case Op::FSub: case Op::FMul: case Op::FDiv: case Op::FRem:
        return {2, 1};
    case Op::Neg: case Op::FNeg:
        return {1, 1};
    // Comparison
    case Op::CmpEq: case Op::CmpNe: case Op::CmpLt:
    case Op::CmpGe: case Op::CmpGt: case Op::CmpLe:
    case Op::FCmpL: case Op::FCmpG:
        return {2, 1};
    case Op::IsNull: case Op::IsNotNull:
        return {1, 1};
    case Op::Instanceof:
        return {1, 1};
    // Conversion
    case Op::I2L: case Op::I2F: case Op::I2D:
    case Op::L2I: case Op::L2F: case Op::L2D:
    case Op::F2I: case Op::F2L: case Op::F2D:
    case Op::D2I: case Op::D2L: case Op::D2F:
    case Op::I2B: case Op::I2C: case Op::I2S:
    case Op::CheckCast:
        return {1, 1};
    case Op::Box:   return {1, 1};
    case Op::Unbox: return {1, 1};
    // Control flow
    case Op::Goto:      return {0, 0};
    case Op::IfTrue:    case Op::IfFalse:
    case Op::IfNull:    case Op::IfNonNull:
    case Op::IfEq: case Op::IfNe: case Op::IfLt:
    case Op::IfGe: case Op::IfGt: case Op::IfLe:
        return {1, 0};
    case Op::TableSwitch:  case Op::LookupSwitch:
        return {1, 0};
    case Op::Return:      return {0, 0};
    case Op::ReturnValue: return {1, 0};
    case Op::Throw:       return {1, 0};
    // Invocations — variable; computed from method descriptor at lift time
    case Op::InvokeVirtual:   case Op::InvokeInterface:
    case Op::InvokeSpecial:   case Op::InvokeStatic:
    case Op::InvokeDynamic:
    case Op::Callvirt:        case Op::Call:
        return {-1, -1};
    // Field access
    case Op::GetField:  return {1, 1};
    case Op::PutField:  return {2, 0};
    case Op::GetStatic: return {0, 1};
    case Op::PutStatic: return {1, 0};
    // Object / array creation
    case Op::New:          return {0, 1};
    case Op::NewArray:     return {1, 1};
    case Op::MultiNewArray:return {-1, 1}; // dims popped = BcIntOperand
    case Op::ArrayLength:  return {1, 1};
    case Op::ArrayLoad:    return {2, 1};
    case Op::ArrayStore:   return {3, 0};
    // Stack manipulation
    case Op::Pop:          return {1, 0};
    case Op::Pop2:         return {2, 0};
    case Op::Dup:          return {1, 2};
    case Op::DupX1:        return {2, 3};
    case Op::DupX2:        return {3, 4};
    case Op::Dup2:         return {2, 4};
    case Op::Dup2X1:       return {3, 5};
    case Op::Dup2X2:       return {4, 6};
    case Op::Swap:         return {2, 2};
    // Monitor
    case Op::MonitorEnter: case Op::MonitorExit:
        return {1, 0};
    // Python
    case Op::PyLoadName:   return {0, 1};
    case Op::PyStoreName:  return {1, 0};
    case Op::PyBuildList:  return {-1, 1};
    case Op::PyBuildDict:  return {-1, 1};
    case Op::PyBuildTuple: return {-1, 1};
    case Op::PyBuildSet:   return {-1, 1};
    case Op::PyForIter:    return {1, 1};  // pushes next or branches
    case Op::PyGetIter:    return {1, 1};
    case Op::PyImport:     return {2, 1};
    case Op::PyYield:      return {1, 0};
    case Op::PyAwait:      return {1, 1};
    case Op::PyFormat:     return {1, 1};
    // Wasm
    case Op::WasmMemorySize: return {0, 1};
    case Op::WasmMemoryGrow: return {1, 1};
    case Op::WasmLoad:       return {1, 1};
    case Op::WasmStore:      return {2, 0};
    case Op::WasmSelect:     return {3, 1};
    case Op::WasmDrop:       return {1, 0};
    case Op::WasmLocalTee:   return {1, 1};
    case Op::WasmRefNull:    return {0, 1};
    case Op::WasmRefIsNull:  return {1, 1};
    case Op::WasmTableGet:   return {1, 1};
    case Op::WasmTableSet:   return {2, 0};
    // Lua
    case Op::LuaConcat:    return {-1, 1};
    case Op::LuaLength:    return {1, 1};
    case Op::LuaNewTable:  return {0, 1};
    case Op::LuaGetField:  return {1, 1};
    case Op::LuaSetField:  return {2, 0};
    case Op::LuaGetTable:  return {2, 1};
    case Op::LuaSetTable:  return {3, 0};
    case Op::LuaCall:      return {-1, -1};
    case Op::LuaTailCall:  return {-1, 0};
    case Op::LuaReturn:    return {-1, 0};
    case Op::LuaSelf:      return {1, 2};
    case Op::LuaClosure:   return {0, 1};
    case Op::LuaVarArg:    return {0, -1};
    case Op::LuaClose:     return {0, 0};
    // CLR
    case Op::Ldstr:        return {0, 1};
    case Op::LdToken:      return {0, 1};
    case Op::Sizeof:       return {0, 1};
    case Op::Initobj:      return {1, 0};
    case Op::Cpobj:        return {2, 0};
    case Op::Ldobj:        return {1, 1};
    case Op::Stobj:        return {2, 0};
    case Op::Refanytype:   return {1, 1};
    case Op::Mkrefany:     return {1, 1};
    case Op::Arglist:      return {0, 1};
    case Op::Tail:         return {0, 0};
    case Op::Constrained:  return {0, 0};
    case Op::Readonly:     return {0, 0};
    }
    return {0, 0};
}

// ─── BcInstruction accessors ──────────────────────────────────────────────────

int64_t BcInstruction::intOp(size_t idx) const {
    return std::get<BcIntOperand>(operands.at(idx)).value;
}
double BcInstruction::floatOp(size_t idx) const {
    return std::get<BcFloatOperand>(operands.at(idx)).value;
}
std::string BcInstruction::stringOp(size_t idx) const {
    return std::get<BcStringOperand>(operands.at(idx)).value;
}
const BcType& BcInstruction::typeOp(size_t idx) const {
    return std::get<BcTypeOperand>(operands.at(idx)).type;
}
const BcMethodRef& BcInstruction::methodOp(size_t idx) const {
    return std::get<BcMethodRef>(operands.at(idx));
}
const BcFieldRef& BcInstruction::fieldOp(size_t idx) const {
    return std::get<BcFieldRef>(operands.at(idx));
}
uint32_t BcInstruction::localOp(size_t idx) const {
    return std::get<BcLocalOperand>(operands.at(idx)).index;
}
uint32_t BcInstruction::blockOp(size_t idx) const {
    return std::get<BcBlockOperand>(operands.at(idx)).blockId;
}

} // namespace bc_module
} // namespace retdec
