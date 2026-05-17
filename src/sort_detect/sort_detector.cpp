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

SortResult InsertionSortDetector::detect(const ssa::SSAFunction& fn) const {
    SortResult result;
    result.algorithm = SortAlgorithm::InsertionSort;

    InsertionSortFingerprint isf;
    auto ev = isf.analyse(fn);
    result.confidence = ev.found ? ev.confidence : 0.0f;

    // Variant: insertion sort is typically small and standalone.
    result.compilerVariant = CompilerVariant::Unknown;
    return result;
}

// ─── SortDetector ─────────────────────────────────────────────────────────────

SortDetector::SortDetector(Config cfg) : cfg_(std::move(cfg)) {
    if (cfg_.runIntrosort)  detectors_.push_back(std::make_unique<IntrosortDetector>());
    if (cfg_.runMerge)      detectors_.push_back(std::make_unique<MergesortDetector>());
    if (cfg_.runHeap)       detectors_.push_back(std::make_unique<HeapsortDetector>());
    if (cfg_.runRadix)      detectors_.push_back(std::make_unique<RadixsortDetector>());
    if (cfg_.runInsertion)  detectors_.push_back(std::make_unique<InsertionSortDetector>());
}

bool SortDetector::passesPreflight(const ssa::SSAFunction& fn) const {
    if (static_cast<int>(fn.blockCount()) < cfg_.minBlocks) return false;
    int instrCount = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (blk) instrCount += static_cast<int>(blk->instrs.size());
    }
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
