/**
 * @file src/jvm_reconstruct/jvm_reconstruct.cpp
 * @brief JVM reconstruction pipeline — top-level orchestration.
 */

#include "retdec/jvm_reconstruct/jvm_reconstruct.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace jvm_reconstruct {

using namespace bc_module;

// ─── JvmReconstructor ────────────────────────────────────────────────────────

JvmReconstructor::JvmReconstructor(ReconstructOptions opts)
    : opts_(opts) {}

// ─── CFG rewriting ───────────────────────────────────────────────────────────

// BcLocalOperand::index is a JVM local slot, not an index into the locals
// vector: ExprContext keys localNames by BcLocalVar::index and JavaExprEmitter
// looks the operand up in that map. Both helpers below take the slot.
BcInstruction
JvmReconstructor::makeStore(uint32_t localSlot, uint32_t slotId, const StackSimResult& sim, uint32_t instrOffset) const
{
	BcInstruction store;
	store.id = UINT32_MAX; // synthetic
	store.offset = instrOffset;
    store.opcode = BcOpcode::StoreLocal;
	store.operands.push_back(BcLocalOperand{localSlot});
	// Carry the defining slot id as an additional int operand for traceability.
	store.operands.push_back(BcIntOperand{static_cast<int64_t>(slotId)});
	(void)sim;
    return store;
}

BcInstruction JvmReconstructor::makeLoad(uint32_t localSlot, const BcType& type, uint32_t instrOffset) const
{
	BcInstruction load;
	load.id = UINT32_MAX;
	load.offset = instrOffset;
    load.opcode = BcOpcode::LoadLocal;
	load.operands.push_back(BcLocalOperand{localSlot});
	load.operands.push_back(BcTypeOperand{type});
	return load;
}

void JvmReconstructor::rewriteCFG(BcCFG& cfg,
                                   BcMethod& method,
                                   const StackSimResult& sim,
                                   const CoalesceResult& coalesce,
                                   const LocalRebuildResult& locals) {
	// For each instruction that produces a surviving stack slot, insert a
	// StoreLocal after it.  For each instruction that consumes a surviving
	// slot, insert a LoadLocal before it.
	//
	// Coalesced (eliminated) slots are skipped — the expression flows inline.
	//
	// A surviving stack slot gets a variable of its own, allocated here.
	//
	// The ids in info.pops and info.pushes are STACK slot ids -- JvmStackSim
	// numbers $s0, $s1, ... from 0 per method -- and were being looked up in
	// locals.slotToLocal, which is keyed by JVM local slot index. Two disjoint
	// spaces both starting at 0: a stack slot whose id happened to equal an
	// existing JVM local slot matched, and the rewrite bound a stack temporary
	// to a completely unrelated, differently typed variable. Where the id did
	// not collide the lookup simply failed and the slot was dropped, so the
	// pass did nothing useful either way.
	//
	// The synthetic slots sit well clear of a method's real ones (the JVM
	// allows at most 65535) and of ExceptionVarIntroducer's 1000 + block.
	static constexpr uint32_t kStackSlotBase = 0x00100000;

	std::vector<BcLocalVar> allLocals = locals.locals;
	std::unordered_map<uint32_t, uint32_t> stackSlotToJvmSlot;

	const auto typeOfStackSlot = [&sim](uint32_t sid) -> BcType {
		for (const auto& s: sim.slots)
			if (s.id == sid) return s.type;
		return types::Int();
	};

	const auto jvmSlotFor = [&](uint32_t sid) -> uint32_t {
		auto it = stackSlotToJvmSlot.find(sid);
		if (it != stackSlotToJvmSlot.end()) return it->second;
		const uint32_t jvmSlot = kStackSlotBase + sid;
		BcLocalVar v;
		v.index = jvmSlot;
		v.name = "$s" + std::to_string(sid);
		v.type = typeOfStackSlot(sid);
		v.isParam = false;
		v.startOffset = 0;
		v.endOffset = UINT32_MAX;
		allLocals.push_back(std::move(v));
		stackSlotToJvmSlot[sid] = jvmSlot;
		return jvmSlot;
	};

	for (auto& blk: cfg.blocks())
	{
		std::vector<BcInstruction> newInstrs;
		newInstrs.reserve(blk.instrs.size() * 2);

        for (auto& insn : blk.instrs) {
            auto infoIt = sim.instrInfo.find(insn.id);
            if (infoIt == sim.instrInfo.end()) {
                newInstrs.push_back(std::move(insn));
                continue;
            }
            const InstrStackInfo& info = infoIt->second;

            // Insert loads for consumed (popped) slots that survive.
            // We insert them before this instruction, in reverse pop order
            // (so the top-of-stack slot is the last load before the insn).
            std::vector<BcInstruction> loads;
            for (uint32_t sid : info.pops) {
                if (coalesce.eliminatedSlots.count(sid)) continue;
				// Only a slot this method already gave a variable to: a pop
				// with no matching push is a value from another block's exit
				// state, which the entry-state merge already accounts for.
				auto it = stackSlotToJvmSlot.find(sid);
				if (it == stackSlotToJvmSlot.end()) continue;
				loads.push_back(makeLoad(it->second, typeOfStackSlot(sid), insn.offset));
			}
			for (auto& ld: loads)
				newInstrs.push_back(std::move(ld));

            // The original instruction.
            newInstrs.push_back(std::move(insn));

            // Insert stores for produced (pushed) slots that survive.
            for (uint32_t sid : info.pushes) {
                if (coalesce.eliminatedSlots.count(sid)) continue;
				newInstrs.push_back(makeStore(jvmSlotFor(sid), sid, sim, newInstrs.back().offset));
			}
		}

		blk.instrs = std::move(newInstrs);
	}

	// Populate method.locals from reconstruction result, including the
	// variables the surviving stack slots were given above.
	method.locals = std::move(allLocals);
	(void)method;
}

// ─── reconstruct ─────────────────────────────────────────────────────────────

ReconstructResult JvmReconstructor::reconstruct(
        BcMethod& method,
        const std::vector<LVTEntry>& lvtEntries) {
    ReconstructResult result;

    // Phase 1+2: Stack simulation.
    JvmStackSim stackSim(opts_.stackSim);
    result.stackSim = stackSim.simulate(method.cfg, method);
    if (result.stackSim.status != StackSimResult::OK) {
        result.status = ReconstructResult::Error;
        result.error  = result.stackSim.error;
        return result;
    }

    // Phase 3: Slot coalescing.
    SlotCoalescer coalescer;
    result.coalesce = coalescer.coalesce(method.cfg, result.stackSim);

    // Phase 4: Local variable reconstruction.
    LocalRebuilder rebuilder(opts_.locals);
    result.locals = rebuilder.rebuild(method, method.cfg,
                                      result.stackSim,
                                      result.coalesce,
                                      lvtEntries);
    if (result.locals.status != LocalRebuildResult::OK) {
        result.status = ReconstructResult::PartialError;
        result.warnings.push_back(result.locals.error);
    }

    // Phase 4b: Exception variable introduction.
    ExceptionVarIntroducer ehIntro;
    ehIntro.introduce(method.cfg, method, result.stackSim, result.locals);

    // Phase 5: Pattern detection.
    if (opts_.detectPatterns) {
        PatternLifter patLifter;
        result.patterns = patLifter.lift(method.cfg, method,
                                          result.stackSim, result.locals);
    }

    // Phase 6: CFG rewriting.
    if (opts_.rewriteCFG) {
        rewriteCFG(method.cfg, method,
                   result.stackSim, result.coalesce, result.locals);
    }

    return result;
}

// ─── reconstructModule ────────────────────────────────────────────────────────

int JvmReconstructor::reconstructModule(BcModule& module) {
    int errors = 0;
    for (auto& cls : module.classes()) {
        for (auto& method : cls.methods) {
            if (method.cfg.blocks().empty()) continue;
            auto result = reconstruct(method);
            if (result.status == ReconstructResult::Error)
                ++errors;
        }
    }
    return errors;
}

} // namespace jvm_reconstruct
} // namespace retdec
