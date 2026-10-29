#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
/**
 * @file src/sort_detect/sort_detector.cpp
 * @brief SortDetector orchestrator, ElementTypeRecoverer, and utility implementations.
 *
 * This file contains:
 *   - SortResult::toString / algorithmName
 *   - ElementType::toString
 *   - ElementTypeRecoverer::recover
 *   - InsertionSortDetector::detect
 *   - SortDetector constructor, analyseFunction, analyseModule
 */

#include <memory>
#include "retdec/sort_detect/sort_detect.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <sstream>

namespace retdec {
namespace sort_detect {

// ─── SortAlgorithm string ─────────────────────────────────────────────────────

std::string SortResult::algorithmName() const noexcept {
    switch (algorithm) {
    case SortAlgorithm::Unknown:       return "unknown";
    case SortAlgorithm::Quicksort:     return "quicksort";
    case SortAlgorithm::Introsort:     return "introsort (std::sort)";
    case SortAlgorithm::Mergesort:     return "mergesort (std::stable_sort)";
    case SortAlgorithm::Heapsort:      return "heapsort";
    case SortAlgorithm::Radixsort:     return "radix sort";
    case SortAlgorithm::InsertionSort: return "insertion sort";
    case SortAlgorithm::SelectionSort: return "selection sort";
    case SortAlgorithm::BubbleSort:    return "bubble sort";
    case SortAlgorithm::Timsort:       return "timsort";
    case SortAlgorithm::ShellSort:     return "shell sort";
    }
    return "?";
}

std::string SortResult::toString() const {
    std::ostringstream ss;
    ss << algorithmName()
       << " (confidence=" << confidence << ")"
       << " element=" << elementType.toString();
    if (compilerVariant != CompilerVariant::Unknown) {
        ss << " compiler=";
        switch (compilerVariant) {
        case CompilerVariant::GCC:   ss << "GCC";   break;
        case CompilerVariant::Clang: ss << "Clang"; break;
        case CompilerVariant::MSVC:  ss << "MSVC";  break;
        default: break;
        }
    }
    return ss.str();
}

// ─── ElementType::toString ────────────────────────────────────────────────────

std::string ElementType::toString() const {
    switch (kind) {
    case Kind::Unknown:  return "unknown";
    case Kind::Int8:     return "int8_t";
    case Kind::Int16:    return "int16_t";
    case Kind::Int32:    return "int32_t";
    case Kind::Int64:    return "int64_t";
    case Kind::UInt8:    return "uint8_t";
    case Kind::UInt16:   return "uint16_t";
    case Kind::UInt32:   return "uint32_t";
    case Kind::UInt64:   return "uint64_t";
    case Kind::Float:    return "float";
    case Kind::Double:   return "double";
    case Kind::Pointer:  return "void*";
    case Kind::Struct:   return name.empty() ? "struct" : "struct " + name;
    }
    return "unknown";
}

// ─── ElementTypeRecoverer ─────────────────────────────────────────────────────

ElementType ElementTypeRecoverer::fromCompareWidth(uint8_t bitWidth,
                                                    bool isSigned) const {
    ElementType et;
    et.byteWidth = bitWidth / 8;
    et.isSigned  = isSigned;
    switch (bitWidth) {
    case 8:  et.kind = isSigned ? ElementType::Kind::Int8   : ElementType::Kind::UInt8;  break;
    case 16: et.kind = isSigned ? ElementType::Kind::Int16  : ElementType::Kind::UInt16; break;
    case 32: et.kind = isSigned ? ElementType::Kind::Int32  : ElementType::Kind::UInt32; break;
    case 64: et.kind = isSigned ? ElementType::Kind::Int64  : ElementType::Kind::UInt64; break;
    default: et.kind = ElementType::Kind::Unknown; break;
    }
    return et;
}

ElementType ElementTypeRecoverer::fromComparatorCall(const ssa::SSAFunction& fn,
                                                      uint32_t callInstrId) const {
    // When there is a comparator call, the element type comes from the
    // call's parameter types.  We look at the width of the values passed
    // to the call site.
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->id != callInstrId) continue;
            // The first two arguments should be the elements to compare.
            if (instr->uses.size() < 2) break;
            const auto* arg0 = fn.value(instr->uses[0].valueId);
            if (!arg0) break;
            bool isSigned = (arg0->kind == ssa::ValueKind::VirtualReg);
            return fromCompareWidth(arg0->width, isSigned);
        }
    }
    return ElementType{};
}

ElementType ElementTypeRecoverer::recover(const ssa::SSAFunction& fn,
                                           const SortResult& /* partialResult */) const {
    // Strategy:
    //   1. Find a Compare instruction.
    //   2. Trace its operands back to Load instructions.
    //   3. Read the width of the loaded value.

    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Compare) continue;
            // Try to trace the operands of this Compare back to a Load.
            for (const auto& use : instr->uses) {
                const auto* val = fn.value(use.valueId);
                if (!val) continue;
                if (val->defInstr && val->defInstr->op == ssa::IrInstr::Op::Load) {
                    // Width of the loaded value tells us the element type.
                    bool isSigned = true; // default to signed for comparison
                    return fromCompareWidth(val->width, isSigned);
                }
            }
        }
    }

    // Fallback: look for a comparator call.
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
            // A non-self call with exactly 2 arguments is likely the comparator.
            if (instr->calleeName != fn.name() && instr->uses.size() == 2) {
                return fromComparatorCall(fn, instr->id);
            }
        }
    }

    return ElementType{};
}

// ─── InsertionSortDetector ────────────────────────────────────────────────────

/// `gap /= 2` or `gap >>= 1` in a block on a cycle.
static bool hasHalvingInLoop(const ssa::SSAFunction& fn) {
    for (const auto& blk : fn.blocks()) {
        if (!blk || !ssa::blockOnCycle(fn, blk->id)) continue;
        for (const auto* i : blk->instrs) {
            if (!i) continue;
            const bool shift = i->op == ssa::IrInstr::Op::Shr || i->op == ssa::IrInstr::Op::Sar;
            const bool div = i->op == ssa::IrInstr::Op::Div;
            if (!shift && !div) continue;
            for (const auto& u : i->uses) {
                const auto* v = fn.value(u.valueId);
                if (v && v->kind == ssa::ValueKind::Immediate && v->imm == (shift ? 1u : 2u)) return true;
            }
        }
    }
    return false;
}

/// `j -= gap` in a block on a cycle: the shifting loop steps by a variable,
/// not by one. With the gaps folded to constants (-O2), the halving is gone
/// but the variable step is not.
static bool hasVariableStepInLoop(const ssa::SSAFunction& fn) {
    for (const auto& blk : fn.blocks()) {
        if (!blk || !ssa::blockOnCycle(fn, blk->id)) continue;
        for (const auto* i : blk->instrs) {
            if (!i || i->op != ssa::IrInstr::Op::Sub) continue;
            for (const auto& u : i->uses) {
                if (u.operandIndex != 1) continue;
                const auto* v = fn.value(u.valueId);
                if (!v || v->kind == ssa::ValueKind::Immediate) continue;
                const auto* d = v->defInstr;
                if (d && d->op == ssa::IrInstr::Op::Load) continue;
                return true;
            }
        }
    }
    return false;
}

SortResult InsertionSortDetector::detect(const ssa::SSAFunction& fn) const {
    SortResult result;
    result.algorithm = SortAlgorithm::InsertionSort;

    InsertionSortFingerprint isf;
    auto ev = isf.analyse(fn);
    result.confidence = ev.found ? ev.confidence : 0.0f;
    // A gap that halves on a loop makes it Shell sort: insertion sort's inner
    // step is always one, so it never halves anything inside a loop.
    if (result.confidence > 0.0f && (hasHalvingInLoop(fn) || hasVariableStepInLoop(fn)))
        result.algorithm = SortAlgorithm::ShellSort;

    // Variant: insertion sort is typically small and standalone.
    result.compilerVariant = CompilerVariant::Unknown;
    return result;
}

// ─── SelectionSortDetector ─────

namespace {

bool onStack(const ssa::SSAFunction& fn, const ssa::IrInstr& i) {
    const uint32_t idx = i.op == ssa::IrInstr::Op::Store ? 1 : 0;
    for (const auto& u : i.uses)
        if (u.operandIndex == idx)
            if (const auto* a = fn.value(u.valueId); a && a->isStackSlot) return true;
    return false;
}

struct MemOps { int loads = 0, stores = 0, compares = 0; };

MemOps memOps(const ssa::SSAFunction& fn, const ssa::BasicBlock& blk) {
    MemOps m;
    for (const auto* i : blk.instrs) {
        if (!i) continue;
        if (i->op == ssa::IrInstr::Op::Load && !onStack(fn, *i)) ++m.loads;
        if (i->op == ssa::IrInstr::Op::Store && !onStack(fn, *i)) ++m.stores;
        if (i->op == ssa::IrInstr::Op::Compare) ++m.compares;
    }
    return m;
}

/// Whether @a value is only ever used as a load or store address, like the
/// `a + i*4 - 4` of an element access, rather than as a counter.
bool usedOnlyAsAddress(const ssa::SSAFunction& fn, ssa::ValueId value, int depth = 0) {
    if (depth > 4) return false;
    bool used = false;
    for (const auto& blk : fn.blocks()) {
        if (!blk) continue;
        for (const auto* i : blk->instrs) {
            if (!i) continue;
            for (const auto& u : i->uses) {
                if (u.valueId != value) continue;
                // A copy or cast: follow it.
                if (i->op == ssa::IrInstr::Op::Assign) {
                    if (!usedOnlyAsAddress(fn, i->defValue, depth + 1)) return false;
                    used = true;
                    continue;
                }
                const uint32_t addr = i->op == ssa::IrInstr::Op::Store ? 1 : 0;
                if ((i->op != ssa::IrInstr::Op::Load && i->op != ssa::IrInstr::Op::Store)
                        || u.operandIndex != addr)
                    return false;
                used = true;
            }
        }
    }
    for (const auto& phi : fn.phis())
        if (phi)
            for (const auto& op : phi->operands)
                if (op.second == value) return false;
    return used;
}

/// The bodies of the natural loops found from block 0, one per header (the
/// back edges to one header make one loop).
std::vector<std::set<uint32_t>> loopBodies(const ssa::SSAFunction& fn) {
    std::map<uint32_t, std::set<uint32_t>> byHeader;
    std::vector<int> state(fn.blockCount(), 0); // 0 new, 1 on stack, 2 done
    std::vector<std::pair<uint32_t, size_t>> stack;
    if (!fn.block(0)) return {};
    stack.push_back({0, 0});
    state[0] = 1;
    while (!stack.empty()) {
        auto& [b, next] = stack.back();
        const auto* blk = fn.block(b);
        if (!blk || next >= blk->succs.size()) {
            state[b] = 2;
            stack.pop_back();
            continue;
        }
        const uint32_t h = blk->succs[next++];
        if (h >= state.size()) continue;
        if (state[h] == 0) {
            state[h] = 1;
            stack.push_back({h, 0});
        } else if (state[h] == 1) {
            std::set<uint32_t> body{h};
            std::vector<uint32_t> work{b};
            while (!work.empty()) {
                const uint32_t x = work.back();
                work.pop_back();
                if (!body.insert(x).second) continue;
                if (const auto* xb = fn.block(x))
                    work.insert(work.end(), xb->preds.begin(), xb->preds.end());
            }
            byHeader[h].insert(body.begin(), body.end());
        }
    }
    std::vector<std::set<uint32_t>> loops;
    for (auto& [h, body] : byHeader) loops.push_back(std::move(body));
    return loops;
}

/// Whether @a a and @a b share a loop that is not the outermost one around
/// @a a: the smallest loop holding @a a also holds @a b.
bool inSameInnerLoop(const std::vector<std::set<uint32_t>>& loops, uint32_t a, uint32_t b) {
    const std::set<uint32_t>* inner = nullptr;
    for (const auto& l : loops)
        if (l.count(a) && (!inner || l.size() < inner->size())) inner = &l;
    return inner && inner->count(b);
}

bool decrementsInLoop(const ssa::SSAFunction& fn) {
    for (const auto& blk : fn.blocks()) {
        if (!blk || !ssa::blockOnCycle(fn, blk->id)) continue;
        for (const auto* i : blk->instrs) {
            if (!i || (i->op != ssa::IrInstr::Op::Add && i->op != ssa::IrInstr::Op::Sub)) continue;
            for (const auto& u : i->uses) {
                const auto* v = fn.value(u.valueId);
                if (!v || v->kind != ssa::ValueKind::Immediate) continue;
                const auto s = static_cast<int64_t>(v->imm);
                const bool neg = (s < 0 && s > -16) || (v->imm >= 0xfffffff0ull && v->imm <= 0xffffffffull);
                if (((i->op == ssa::IrInstr::Op::Add && neg) ||
                        (i->op == ssa::IrInstr::Op::Sub && u.operandIndex == 1 && v->imm == 1))
                        && !usedOnlyAsAddress(fn, i->defValue)) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

SortResult SelectionSortDetector::detect(const ssa::SSAFunction& fn) const {
    SortResult result;
    result.algorithm = SortAlgorithm::SelectionSort;
    if (std::getenv("RETDEC_SORT_DIAG"))
        std::fprintf(stderr, "SEL %s nested=%d dec=%d\n", fn.name().c_str(), ssa::hasNestedLoop(fn), decrementsInLoop(fn));
    if (!ssa::hasNestedLoop(fn) || decrementsInLoop(fn)) return result;

    // The scan: compares elements it read and writes nothing; at -O0 one
    // block reads both, at -O2 the minimum stays in a register and each
    // path through the scan reads one. Nothing it branches to swaps either
    // -- that is bubble sort's inner loop.
    int scanLoads = 0;
    bool swap = false;
    std::set<uint32_t> nextToScan, scanBlocks;
    const auto loops = loopBodies(fn);
    for (const auto& blk : fn.blocks()) {
        if (!blk || !ssa::blockOnCycle(fn, blk->id)) continue;
        const auto m = memOps(fn, *blk);
        if (m.loads >= 1 && m.compares >= 1 && m.stores == 0) {
            scanLoads += m.loads;
            scanBlocks.insert(blk->id);
            nextToScan.insert(blk->succs.begin(), blk->succs.end());
        }
    }
    const bool scan = scanLoads >= 2;
    for (const auto& blk : fn.blocks()) {
        if (!blk || !ssa::blockOnCycle(fn, blk->id)) continue;
        const auto m = memOps(fn, *blk);
        // A swap reads both elements first (-O0), or stores exactly the two
        // values the scan already holds in registers (-O2).
        if (m.stores >= 2 && (m.loads >= 2 || (m.loads == 0 && m.stores == 2))) {
            // Bubble sort swaps inside the compare loop; selection sort
            // swaps once the scan loop is done.
            for (const auto sb : scanBlocks)
                if (inSameInnerLoop(loops, sb, blk->id)) return result;
            if (loops.empty() && nextToScan.count(blk->id)) return result;
            swap = true;
        }
    }
    if (std::getenv("RETDEC_SORT_DIAG"))
        std::fprintf(stderr, "SEL %s scan=%d swap=%d\n", fn.name().c_str(), scan, swap);
    if (scan && swap) result.confidence = 0.55f;
    result.compilerVariant = CompilerVariant::Unknown;
    return result;
}

// ─── SortDetector ─────────────────────────────────────────────────────────────

SortDetector::SortDetector(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.runIntrosort)  detectors_.push_back(std::make_unique<IntrosortDetector>());
    if (cfg_.runMerge)      detectors_.push_back(std::make_unique<MergesortDetector>());
    if (cfg_.runHeap)       detectors_.push_back(std::make_unique<HeapsortDetector>());
    if (cfg_.runQuicksort)  detectors_.push_back(std::make_unique<QuicksortDetector>());
    if (cfg_.runBubble)     detectors_.push_back(std::make_unique<BubbleSortDetector>());
    if (cfg_.runRadix)      detectors_.push_back(std::make_unique<RadixsortDetector>());
    if (cfg_.runInsertion)  detectors_.push_back(std::make_unique<InsertionSortDetector>());
    if (cfg_.runSelection)  detectors_.push_back(std::make_unique<SelectionSortDetector>());
}

bool SortDetector::passesPreflight(const ssa::SSAFunction& fn) const {
    if (static_cast<int>(fn.blockCount()) < cfg_.minBlocks) return false;
    int instrCount = 0;
    int selfCalls = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        instrCount += static_cast<int>(blk->instrs.size());
        for (const auto* i : blk->instrs)
            if (i && i->op == ssa::IrInstr::Op::Call && i->calleeName == fn.name()) ++selfCalls;
    }
    // A divide-and-conquer driver -- guard, helper call, two self-calls -- is
    // short by nature, especially at -O0 where locals are untranslated stack
    // slots; the size floor dropped every such quicksort before detection.
    if (selfCalls >= 2) return true;
    return instrCount >= cfg_.minInstrs;
}

CompilerVariant SortDetector::inferVariant(const ssa::SSAFunction& fn,
                                             const SortResult& r) const {
    if (r.compilerVariant != CompilerVariant::Unknown) return r.compilerVariant;

    // Name-based fallback.
    const std::string& name = fn.name();
    if (name.find("__gnu")   != std::string::npos ||
        name.find("libstdc") != std::string::npos)
        return CompilerVariant::GCC;
    if (name.find("_VSTD")   != std::string::npos ||
        name.find("libc++")  != std::string::npos)
        return CompilerVariant::Clang;
    if (name.find("?std@@")  != std::string::npos ||
        name.find("_STL")    != std::string::npos)
        return CompilerVariant::MSVC;
    return CompilerVariant::Unknown;
}

SortResult SortDetector::analyseFunction(const ssa::SSAFunction& fn) const {
    ++stats_.functionsAnalysed;

    if (!passesPreflight(fn)) {
        ++stats_.functionsSkipped;
        return SortResult{};
    }

    SortResult best;
    best.confidence = 0.0f;

    for (const auto& det : detectors_) {
        auto r = det->detect(fn);
        if (r.confidence > best.confidence) best = r;
    }

    if (best.confidence < cfg_.minConfidence)
        return SortResult{};

    // Recover element type.
    best.elementType = typeRecoverer_.recover(fn, best);
    best.compilerVariant = inferVariant(fn, best);

    ++stats_.detections;
    ++stats_.byAlgorithm[best.algorithm];

    return best;
}

SortDetector::DetectionMap SortDetector::analyseModule(
        const std::vector<const ssa::SSAFunction*>& functions) const {
    DetectionMap results;
    for (const auto* fn : functions) {
        if (!fn) continue;
        auto r = analyseFunction(*fn);
        if (r.algorithm != SortAlgorithm::Unknown && r.confidence >= cfg_.minConfidence)
            results[fn->name()] = r;
    }
    return results;
}

} // namespace sort_detect
} // namespace retdec
