/**
 * @file src/container_detect/container_detector.cpp
 * @brief ContainerDetector orchestrator + TemplateTypeRecoverer + utilities.
 *
 * ## Orchestration
 *
 * `ContainerDetector::analyseFunction` runs all registered per-container
 * detectors in a fixed priority order and returns the detection with the
 * highest confidence, provided it meets the minimum confidence threshold.
 *
 * Priority order (most discriminating detectors first):
 *   1. StringDetector   — SSO threshold is highly specific.
 *   2. SharedPtrDetector— atomic + two-pointer layout is distinctive.
 *   3. MapDetector      — rotation patterns are unique to red-black trees.
 *   4. UnorderedMapDetector — hash + modulo combination.
 *   5. ListDetector     — sentinel + chain traversal.
 *   6. VectorDetector   — three-pointer layout is common but lower priority.
 *
 * After selecting the best match, `TemplateTypeRecoverer` attempts to refine
 * the element type using the element byte-width recovered from the detector.
 *
 * ## Template type recovery
 *
 * `TemplateTypeRecoverer::recoverElementType` uses three strategies in order:
 *
 *   1. **Element byte-width** (from Load stride or Mul constant):
 *      - 1 byte → int8_t / char
 *      - 2 bytes → int16_t
 *      - 4 bytes → int32_t / float
 *      - 8 bytes → int64_t / double / pointer
 *
 *   2. **Comparator function parameter type** (for map/set):
 *      If the function calls an inlined comparator or receives a comparator
 *      object, the parameter type of that call gives the key type.
 *
 *   3. **Hash function input type** (for unordered_map/set):
 *      The first argument of the hash call gives the key type.
 *
 * When none of these succeed, the element type defaults to `int32_t`.
 */

#include <memory>
#include "retdec/container_detect/container_detect.h"
#include "retdec/ssa/ssa.h"

#include <algorithm>
#include <sstream>

namespace retdec {
namespace container_detect {

// ─── RecoveredType utilities ─────────────────────────────────────────────────

std::string RecoveredType::toString() const {
    switch (kind) {
    case Kind::Int8:    return isSigned ? "int8_t"  : "uint8_t";
    case Kind::Int16:   return isSigned ? "int16_t" : "uint16_t";
    case Kind::Int32:   return isSigned ? "int32_t" : "uint32_t";
    case Kind::Int64:   return isSigned ? "int64_t" : "uint64_t";
    case Kind::UInt8:   return "uint8_t";
    case Kind::UInt16:  return "uint16_t";
    case Kind::UInt32:  return "uint32_t";
    case Kind::UInt64:  return "uint64_t";
    case Kind::Float:   return "float";
    case Kind::Double:  return "double";
    case Kind::Pointer: return "void*";
    case Kind::Struct:  return name.empty() ? "struct_t" : name;
    case Kind::String:  return "std::string";
    default:            return "int";
    }
}

// ─── ContainerResult utilities ───────────────────────────────────────────────

std::string ContainerResult::kindName() const noexcept {
    switch (kind) {
    case ContainerKind::Vector:       return "std::vector";
    case ContainerKind::List:         return "std::list";
    case ContainerKind::Deque:        return "std::deque";
    case ContainerKind::Map:          return "std::map";
    case ContainerKind::Set:          return "std::set";
    case ContainerKind::UnorderedMap: return "std::unordered_map";
    case ContainerKind::UnorderedSet: return "std::unordered_set";
    case ContainerKind::String:       return "std::string";
    case ContainerKind::SharedPtr:    return "std::shared_ptr";
    case ContainerKind::UniquePtr:    return "std::unique_ptr";
    case ContainerKind::WeakPtr:      return "std::weak_ptr";
    case ContainerKind::Optional:     return "std::optional";
    case ContainerKind::Variant:      return "std::variant";
    case ContainerKind::Array:        return "std::array";
    default:                          return "unknown";
    }
}

std::string ContainerResult::toString() const {
    std::ostringstream os;
    os << kindName() << " (confidence=" << confidence << ")"
       << " element=" << elementType.toString();
    if (!emittedType.empty()) os << " emitted=\"" << emittedType << "\"";
    return os.str();
}

std::string ContainerResult::cHint() const noexcept {
    switch (kind) {
    case ContainerKind::Vector:       return "vector_like_3ptr";
    case ContainerKind::List:         return "list_like_dllist";
    case ContainerKind::Map:          return "map_like_rbtree";
    case ContainerKind::Set:          return "set_like_rbtree";
    case ContainerKind::UnorderedMap: return "unordered_map_like_hash";
    case ContainerKind::UnorderedSet: return "unordered_set_like_hash";
    case ContainerKind::String:       return "string_like_sso";
    case ContainerKind::SharedPtr:    return "shared_ptr_like_2ptr";
    case ContainerKind::UniquePtr:    return "unique_ptr_like_1ptr";
    case ContainerKind::WeakPtr:      return "weak_ptr_like_2ptr";
    case ContainerKind::Deque:        return "deque_like_chunked";
    case ContainerKind::Array:        return "array_like_fixed";
    default:                          return {};
    }
}

// ─── TemplateTypeRecoverer ───────────────────────────────────────────────────

RecoveredType TemplateTypeRecoverer::fromByteWidth(uint8_t w, bool isSigned) const {
    RecoveredType t;
    t.isSigned   = isSigned;
    t.byteWidth  = w;
    switch (w) {
    case 1:  t.kind = isSigned ? RecoveredType::Kind::Int8  : RecoveredType::Kind::UInt8;  break;
    case 2:  t.kind = isSigned ? RecoveredType::Kind::Int16 : RecoveredType::Kind::UInt16; break;
    case 4:  t.kind = isSigned ? RecoveredType::Kind::Int32 : RecoveredType::Kind::UInt32; break;
    case 8:  t.kind = isSigned ? RecoveredType::Kind::Int64 : RecoveredType::Kind::UInt64; break;
    default: t.kind = RecoveredType::Kind::Unknown; break;
    }
    return t;
}

RecoveredType TemplateTypeRecoverer::fromComparatorParam(const ssa::SSAFunction& fn) const {
    // Look for a call whose callee name contains "compare" or "less".
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = instr->calleeName;
            if (cn.find("compare") == std::string::npos &&
                cn.find("less")    == std::string::npos) continue;
            // First argument width gives the key type.
            if (!instr->uses.empty()) {
                const auto* v = fn.value(instr->uses[0].valueId);
                if (v) return fromByteWidth(static_cast<uint8_t>(v->memWidth));
            }
        }
    }
    return {};
}

RecoveredType TemplateTypeRecoverer::fromHashParam(const ssa::SSAFunction& fn) const {
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (!blk) continue;
        for (const auto* instr : blk->instrs) {
            if (!instr || instr->op != ssa::IrInstr::Op::Call) continue;
            const auto& cn = instr->calleeName;
            if (cn.find("hash") == std::string::npos &&
                cn.find("Hash") == std::string::npos) continue;
            if (!instr->uses.empty()) {
                const auto* v = fn.value(instr->uses[0].valueId);
                if (v) return fromByteWidth(static_cast<uint8_t>(v->memWidth));
            }
        }
    }
    return {};
}

RecoveredType TemplateTypeRecoverer::recoverElementType(
        const ssa::SSAFunction& fn,
        const ContainerResult& partial,
        uint8_t elementByteWidth) const {
    // Strategy 1: explicit element byte-width.
    if (elementByteWidth > 0) return fromByteWidth(elementByteWidth);
    // Strategy 2: already recovered.
    if (partial.elementType.kind != RecoveredType::Kind::Unknown)
        return partial.elementType;
    // Strategy 3: from comparator.
    {
        auto t = fromComparatorParam(fn);
        if (t.kind != RecoveredType::Kind::Unknown) return t;
    }
    // Default.
    return fromByteWidth(4, true);
}

RecoveredType TemplateTypeRecoverer::recoverKeyType(
        const ssa::SSAFunction& fn,
        const ContainerResult& partial) const {
    if (partial.keyType.kind != RecoveredType::Kind::Unknown) return partial.keyType;
    {
        auto t = fromComparatorParam(fn);
        if (t.kind != RecoveredType::Kind::Unknown) return t;
    }
    {
        auto t = fromHashParam(fn);
        if (t.kind != RecoveredType::Kind::Unknown) return t;
    }
    return fromByteWidth(4, true);
}

// ─── ContainerDetector ───────────────────────────────────────────────────────

ContainerDetector::ContainerDetector(Config cfg) : cfg_(std::move(cfg)) {
    // Register detectors in descending specificity order.
    if (cfg_.runString)     detectors_.push_back(std::make_unique<StringDetector>());
    if (cfg_.runSharedPtr)  detectors_.push_back(std::make_unique<SharedPtrDetector>());
    if (cfg_.runMap)        detectors_.push_back(std::make_unique<MapDetector>());
    if (cfg_.runUnordered)  detectors_.push_back(std::make_unique<UnorderedMapDetector>());
    if (cfg_.runList)       detectors_.push_back(std::make_unique<ListDetector>());
    if (cfg_.runVector)     detectors_.push_back(std::make_unique<VectorDetector>());
}

bool ContainerDetector::passesPreflight(const ssa::SSAFunction& fn) const {
    if ((int)fn.blockCount() < cfg_.minBlocks) return false;
    int instrs = 0;
    for (uint32_t b = 0; b < fn.blockCount(); ++b) {
        const auto* blk = fn.block(b);
        if (blk) instrs += static_cast<int>(blk->instrs.size());
    }
    return instrs >= cfg_.minInstrs;
}

ContainerResult ContainerDetector::analyseFunction(const ssa::SSAFunction& fn) const {
    ++stats_.functionsAnalysed;

    if (!passesPreflight(fn)) {
        ++stats_.functionsSkipped;
        return {};
    }

    ContainerResult best;
    best.kind = ContainerKind::Unknown;
    best.confidence = 0.0f;

    for (const auto& det : detectors_) {
        auto result = det->detect(fn);
        if (result.confidence > best.confidence) {
            best = result;
        }
    }

    if (best.confidence < cfg_.minConfidence) {
        best.kind = ContainerKind::Unknown;
        best.confidence = 0.0f;
        return best;
    }

    // Refine element type.
    best.elementType = typeRecoverer_.recoverElementType(fn, best);
    if (best.kind == ContainerKind::Map || best.kind == ContainerKind::Set ||
        best.kind == ContainerKind::UnorderedMap || best.kind == ContainerKind::UnorderedSet) {
        best.keyType = typeRecoverer_.recoverKeyType(fn, best);
    }

    // Refine emittedType with recovered element type.
    if (best.kind == ContainerKind::Vector)
        best.emittedType = "std::vector<" + best.elementType.toString() + ">";
    else if (best.kind == ContainerKind::List)
        best.emittedType = "std::list<" + best.elementType.toString() + ">";
    else if (best.kind == ContainerKind::Map)
        best.emittedType = "std::map<" + best.keyType.toString() + ", " +
                           best.elementType.toString() + ">";
    else if (best.kind == ContainerKind::UnorderedMap)
        best.emittedType = "std::unordered_map<" + best.keyType.toString() + ", " +
                           best.elementType.toString() + ">";

    ++stats_.detections;
    ++stats_.byKind[best.kind];
    return best;
}

ContainerDetector::DetectionMap ContainerDetector::analyseModule(
        const std::vector<const ssa::SSAFunction*>& functions) const {
    DetectionMap result;
    for (const auto* fn : functions) {
        if (!fn) continue;
        auto r = analyseFunction(*fn);
        if (r.kind != ContainerKind::Unknown)
            result[fn->name()] = std::move(r);
    }
    return result;
}

} // namespace container_detect
} // namespace retdec
