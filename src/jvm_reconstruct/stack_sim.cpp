/**
 * @file src/jvm_reconstruct/stack_sim.cpp
 * @brief JVM operand-stack simulation and type-state propagation.
 */

#include <memory>
#include "retdec/jvm_reconstruct/stack_sim.h"

#include <algorithm>
#include <queue>
#include <stdexcept>

namespace retdec {
namespace jvm_reconstruct {

using namespace bc_module;

// ─── StackSlot ────────────────────────────────────────────────────────────────

bool StackSlot::isWide() const {
    if (!type.isPrim()) return false;
    auto p = type.prim();
    return p == BcPrimType{BcPrimKind::Long} || p == BcPrimType{BcPrimKind::Double};
}

// ─── StackSimResult helpers ──────────────────────────────────────────────────

const StackSlot* StackSimResult::slot(uint32_t id) const {
    if (id < slots.size())
        return &slots[id];
    return nullptr;
}

// ─── JvmStackSim ─────────────────────────────────────────────────────────────

JvmStackSim::JvmStackSim(StackSimOptions opts)
    : opts_(opts) {}

uint32_t JvmStackSim::newSlot(StackSimResult& result,
                               const BcType& type,
                               uint32_t defBlock,
                               uint32_t defOffset) {
    uint32_t id = nextSlotId_++;
    StackSlot slot;
    slot.id        = id;
    slot.name      = "$s" + std::to_string(id);
    slot.type      = type;
    slot.defBlock  = defBlock;
    slot.defOffset = defOffset;
    result.slots.push_back(std::move(slot));
    return id;
}

// ─── Type lattice meet ───────────────────────────────────────────────────────

BcType JvmStackSim::meetTypes(const BcType& a, const BcType& b) {
    if (a == b) return a;

    // Both primitive: must be same type to meet (JVM verifier guarantees this
    // for well-formed bytecode; we fall back to int on mismatch).
    if (a.isPrim() && b.isPrim()) {
        if (a.prim() == b.prim()) return a;
        // Numeric widening meet: int ∧ short → int, etc.
        // Simplified: just use the first type.
        return a;
    }

    // One is void/unknown → use the other.
    if (a.isVoid()) return b;
    if (b.isVoid()) return a;

    // Both references → meet is java.lang.Object.
    if (a.isRef() && b.isRef()) {
        // If both are arrays of same element → keep array type.
        if (a.isArray() && b.isArray()) {
            auto& ra = a.ref(); auto& rb = b.ref();
            if (ra.elementType && rb.elementType &&
                *ra.elementType == *rb.elementType)
                return a;
            // Arrays of different types → Object[]
            BcRefType obj;
            obj.kind = BcRefKind::Class;
            obj.className = "java.lang.Object";
            BcRefType arr;
            arr.kind = BcRefKind::Array;
            arr.elementType = std::make_shared<BcType>(BcType{obj});
            return BcType{arr};
        }
        // Generic object meet.
        BcRefType obj;
        obj.kind = BcRefKind::Class;
        obj.className = "java.lang.Object";
        return BcType{obj};
    }

    return a; // fallback
}

// ─── Infer push type ─────────────────────────────────────────────────────────

BcType JvmStackSim::inferPushType(const BcInstruction& insn,
                                   const StackState& stateBefore,
                                   const StackSimResult& result) const {
    switch (insn.opcode) {
        // Constants
        case BcOpcode::PushInt:    return types::Int();
        case BcOpcode::PushLong:   return types::Long();
        case BcOpcode::PushFloat:  return types::Float();
        case BcOpcode::PushDouble: return types::Double();
        case BcOpcode::PushNull: {
            BcRefType ref; ref.kind = BcRefKind::Class; ref.className = "null";
            return BcType{ref};
        }
        case BcOpcode::PushTrue:
        case BcOpcode::PushFalse:  return types::Bool();
        case BcOpcode::PushString: {
            BcRefType ref; ref.kind = BcRefKind::Class;
            ref.className = "java.lang.String";
            return BcType{ref};
        }
        case BcOpcode::LoadClass: {
            BcRefType ref; ref.kind = BcRefKind::Class;
            ref.className = "java.lang.Class";
            return BcType{ref};
        }

        // Arithmetic: result type matches operands.
        case BcOpcode::Add: case BcOpcode::Sub: case BcOpcode::Mul:
        case BcOpcode::Div: case BcOpcode::Rem: case BcOpcode::Neg:
        case BcOpcode::Shl: case BcOpcode::Shr: case BcOpcode::UShr:
        case BcOpcode::And: case BcOpcode::Or:  case BcOpcode::Xor:
            if (!stateBefore.empty()) {
                if (auto* s = result.slot(stateBefore.slotIds.back()))
                    return s->type;
            }
            return types::Int();

        // Float arithmetic
        case BcOpcode::FAdd: case BcOpcode::FSub: case BcOpcode::FMul:
        case BcOpcode::FDiv: case BcOpcode::FRem: case BcOpcode::FNeg:
            return types::Float();

        // Comparison: always produces int.
        case BcOpcode::CmpEq: case BcOpcode::CmpNe:
        case BcOpcode::CmpLt: case BcOpcode::CmpGe:
        case BcOpcode::CmpGt: case BcOpcode::CmpLe:
        case BcOpcode::FCmpL: case BcOpcode::FCmpG:
            return types::Int();
        case BcOpcode::IsNull: case BcOpcode::IsNotNull:
            return types::Bool();

        // Type conversions
        case BcOpcode::I2L: case BcOpcode::F2L: case BcOpcode::D2L:
            return types::Long();
        case BcOpcode::I2F: case BcOpcode::L2F: case BcOpcode::D2F:
            return types::Float();
        case BcOpcode::I2D: case BcOpcode::L2D: case BcOpcode::F2D:
            return types::Double();
        case BcOpcode::L2I: case BcOpcode::F2I: case BcOpcode::D2I:
        case BcOpcode::I2B: case BcOpcode::I2C: case BcOpcode::I2S:
            return types::Int();

        // Array ops
        case BcOpcode::ArrayLength: return types::Int();
        case BcOpcode::ArrayLoad:
            // Return element type from the array's type.
            if (stateBefore.size() >= 2) {
                // stack: [..., array, index]; array is at size-2.
                uint32_t arrSlotId = stateBefore.slotIds[stateBefore.size() - 2];
                if (auto* s = result.slot(arrSlotId)) {
                    if (s->type.isRef() && s->type.isArray()) {
                        const auto& ref = s->type.ref();
                        if (ref.elementType)
                            return *ref.elementType;
                    }
                }
            }
            return types::Int(); // default element type

        // Object creation
        case BcOpcode::New:
        case BcOpcode::NewArray:
        case BcOpcode::MultiNewArray:
            if (!insn.operands.empty()) {
                if (auto* t = std::get_if<BcTypeOperand>(&insn.operands[0]))
                    return t->type;
            }
            return BcType{BcRefType{BcRefKind::Class, "java.lang.Object"}};

        // instanceof → int (0 or 1 in JVM; boolean semantically)
        case BcOpcode::Instanceof: return types::Bool();

        // checkcast → same reference type
        case BcOpcode::CheckCast:
            if (!insn.operands.empty()) {
                if (auto* t = std::get_if<BcTypeOperand>(&insn.operands[0]))
                    return t->type;
            }
            return BcType{BcRefType{BcRefKind::Class, "java.lang.Object"}};

        // Local variable loads
        case BcOpcode::LoadLocal:
            // Type comes from what was stored; we default to int if unknown.
            return types::Int();

        // Invocations: return type from method descriptor.
        case BcOpcode::InvokeVirtual:
        case BcOpcode::InvokeInterface:
        case BcOpcode::InvokeSpecial:
        case BcOpcode::InvokeStatic:
        case BcOpcode::InvokeDynamic:
        case BcOpcode::Callvirt:
        case BcOpcode::Call:
            if (!insn.operands.empty()) {
                if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0]))
                    return m->descriptor.returnType ? *m->descriptor.returnType : types::Void();
            }
            return types::Void();

        // Fields
        case BcOpcode::GetField:
        case BcOpcode::GetStatic:
            if (!insn.operands.empty()) {
                if (auto* f = std::get_if<BcFieldRef>(&insn.operands[0]))
                    return f->type;
            }
            return types::Int();

        // Dup operations
        case BcOpcode::Dup:
        case BcOpcode::DupX1:
        case BcOpcode::DupX2:
            if (!stateBefore.empty()) {
                if (auto* s = result.slot(stateBefore.slotIds.back()))
                    return s->type;
            }
            return types::Int();
        case BcOpcode::Dup2:
        case BcOpcode::Dup2X1:
        case BcOpcode::Dup2X2:
            if (!stateBefore.empty()) {
                if (auto* s = result.slot(stateBefore.slotIds.back()))
                    return s->type;
            }
            return types::Long();

        // CLR-specific
        case BcOpcode::Ldstr:
            return BcType{BcRefType{BcRefKind::Class, "System.String"}};
        case BcOpcode::Box:
            if (!insn.operands.empty()) {
                if (auto* t = std::get_if<BcTypeOperand>(&insn.operands[0]))
                    return t->type;
            }
            return BcType{BcRefType{BcRefKind::Class, "System.Object"}};

        default:
            return types::Void();
    }
}

// ─── Block processing ─────────────────────────────────────────────────────────

void JvmStackSim::processBlock(const BcBasicBlock& blk,
                                StackState& state,
                                StackSimResult& result) {
    for (const auto& insn : blk.instrs) {
        InstrStackInfo info;
        BcStackEffect effect = stackEffectOf(insn.opcode);

        // Infer the type BEFORE this instruction consumes from stack.
        StackState stateBefore = state;

        // Pop operands.
        int popCount = effect.pop < 0 ? 0 : effect.pop;

        // For invocations, the pop count depends on the descriptor.
        if (insn.opcode == BcOpcode::InvokeVirtual ||
            insn.opcode == BcOpcode::InvokeInterface ||
            insn.opcode == BcOpcode::InvokeSpecial ||
            insn.opcode == BcOpcode::Callvirt) {
            // instance + args
            if (!insn.operands.empty()) {
                if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0]))
                    popCount = 1 + static_cast<int>(m->descriptor.params.size());
            }
        } else if (insn.opcode == BcOpcode::InvokeStatic ||
                   insn.opcode == BcOpcode::Call) {
            if (!insn.operands.empty()) {
                if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0]))
                    popCount = static_cast<int>(m->descriptor.params.size());
            }
        } else if (insn.opcode == BcOpcode::InvokeDynamic) {
            if (!insn.operands.empty()) {
                if (auto* m = std::get_if<BcMethodRef>(&insn.operands[0]))
                    popCount = static_cast<int>(m->descriptor.params.size());
            }
        }

        for (int i = 0; i < popCount && !state.empty(); ++i) {
            uint32_t sid = state.pop();
            info.pops.push_back(sid);
        }

        // Push result(s).
        BcType pushType = inferPushType(insn, stateBefore, result);
        int pushCount = effect.push < 0 ? 0 : effect.push;

        // Override push count for void-returning invocations.
        if ((insn.opcode == BcOpcode::InvokeVirtual ||
             insn.opcode == BcOpcode::InvokeInterface ||
             insn.opcode == BcOpcode::InvokeSpecial ||
             insn.opcode == BcOpcode::InvokeStatic ||
             insn.opcode == BcOpcode::InvokeDynamic ||
             insn.opcode == BcOpcode::Callvirt ||
             insn.opcode == BcOpcode::Call) && pushType.isVoid()) {
            pushCount = 0;
        }

        // Dup2 produces two slots.
        if (insn.opcode == BcOpcode::Dup2 ||
            insn.opcode == BcOpcode::Dup2X1 ||
            insn.opcode == BcOpcode::Dup2X2) {
            // If top of stack is wide, just one slot; else two.
            if (!stateBefore.empty()) {
                if (auto* s = result.slot(stateBefore.slotIds.back())) {
                    if (s->isWide()) pushCount = 1;
                    else pushCount = 2;
                }
            }
        }

        for (int i = 0; i < pushCount; ++i) {
            uint32_t sid = newSlot(result, pushType, blk.id, insn.offset);
            state.push(sid);
            info.pushes.push_back(sid);
        }

        result.instrInfo[insn.id] = std::move(info);
    }
}

// ─── Main simulation ──────────────────────────────────────────────────────────

StackSimResult JvmStackSim::simulate(const BcCFG& cfg,
                                      const BcMethod& method) {
    (void)method;
    StackSimResult result;
    nextSlotId_ = 0;

    if (cfg.blocks().empty())
        return result;

    // Initialize entry state (empty stack).
    result.blockEntryStates[0] = StackState{};

    // BFS/worklist over blocks in dominance order (RPO approximation via
    // a simple BFS from the entry block).
    std::queue<uint32_t> worklist;
    worklist.push(0);

    // Seed exception handler blocks: they start with one exception-object
    // slot on the stack (JVM spec §2.11.8).
    for (uint32_t i = 0; i < cfg.blockCount(); ++i) {
        const BcBasicBlock& blk = cfg.block(i);
        if (blk.isExceptionHandler && !result.blockEntryStates.count(i)) {
            StackState ehState;
            uint32_t slotId = nextSlotId_++;
            StackSlot slot;
            slot.id       = slotId;
            slot.name     = "$exc" + std::to_string(slotId);
            slot.type     = types::Class("java.lang.Throwable");
            slot.defBlock  = i;
            slot.defOffset = 0;
            result.slots.push_back(slot);
            ehState.slotIds.push_back(slotId);
            result.blockEntryStates[i] = ehState;
            worklist.push(i);
        }
    }
    std::unordered_map<uint32_t, bool> visited;

    for (int iter = 0; iter < opts_.maxIter && !worklist.empty(); ++iter) {
        uint32_t blockId = worklist.front();
        worklist.pop();

        if (blockId >= cfg.blockCount())
            continue;

        if (visited.count(blockId) && !opts_.meetAtJoins)
            continue;
        visited[blockId] = true;

        // Get the entry state for this block.
        StackState state;
        if (result.blockEntryStates.count(blockId))
            state = result.blockEntryStates[blockId];

        // Process the block.
        StackState entryStateCopy = state;
        processBlock(cfg.block(blockId), state, result);
        result.blockExitStates[blockId] = state;

        // Propagate to successors.
        const BcBasicBlock& blk = cfg.block(blockId);
        for (uint32_t succId : blk.succs) {
            if (succId >= cfg.blockCount()) continue;

            if (!result.blockEntryStates.count(succId)) {
                result.blockEntryStates[succId] = state;
                worklist.push(succId);
            } else if (opts_.meetAtJoins) {
                // Merge stack states at the join point.
                auto& existingState = result.blockEntryStates[succId];
                bool changed = false;
                size_t minLen = std::min(existingState.slotIds.size(),
                                         state.slotIds.size());
                for (size_t i = 0; i < minLen; ++i) {
                    uint32_t oldSlot = existingState.slotIds[i];
                    uint32_t newSlot = state.slotIds[i];
                    if (oldSlot != newSlot) {
                        // Meet the types: create a phi-slot.
                        const StackSlot* sa = result.slot(oldSlot);
                        const StackSlot* sb = result.slot(newSlot);
                        if (sa && sb) {
                            BcType metType = meetTypes(sa->type, sb->type);
                            uint32_t phiId = nextSlotId_++;
                            StackSlot phi;
                            phi.id = phiId;
                            phi.name = "$phi" + std::to_string(phiId);
                            phi.type = metType;
                            phi.defBlock = succId;
                            phi.defOffset = 0;
                            result.slots.push_back(phi);
                            existingState.slotIds[i] = phiId;
                            changed = true;
                        }
                    }
                }
                if (changed && !visited.count(succId))
                    worklist.push(succId);
            }
        }

        // Exception handlers receive one slot (the exception object).
        for (const auto& eh : cfg.handlers()) {
            if (eh.handlerBlock >= cfg.blockCount()) continue;
            if (!result.blockEntryStates.count(eh.handlerBlock)) {
                StackState ehState;
                BcType exType;
                if (eh.catchType.has_value())
                    exType = *eh.catchType;
                else {
                    BcRefType obj; obj.kind = BcRefKind::Class;
                    obj.className = "java.lang.Throwable";
                    exType = BcType{obj};
                }
                uint32_t sid = newSlot(result, exType, eh.handlerBlock, 0);
                ehState.push(sid);
                result.blockEntryStates[eh.handlerBlock] = ehState;
                worklist.push(eh.handlerBlock);
            }
        }
    }

    return result;
}

} // namespace jvm_reconstruct
} // namespace retdec
