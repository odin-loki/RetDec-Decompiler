/**
 * @file src/type_inference/abi_seeder.cpp
 * @brief ABI parameter/return-type seeding, struct recovery, and TypeInferencePass.
 *
 * ## ABI seeding
 *
 * Calling conventions define which registers carry which argument and which
 * register holds the return value.  We seed the type propagation with these
 * ABI-mandated types:
 *
 * ### System V x86-64
 *   Integer args: RDI, RSI, RDX, RCX, R8, R9  (in order, 64-bit each)
 *   Float args:   XMM0–XMM7                    (128-bit float vectors)
 *   Return (int): RAX                           (64-bit integer)
 *   Return (fp):  XMM0                          (128-bit float)
 *
 * ### Windows x64
 *   Integer args: RCX, RDX, R8, R9  (4 args max in registers)
 *   Float args:   XMM0–XMM3 (mirror integer slots)
 *   Return (int): RAX
 *   Return (fp):  XMM0
 *
 * ### x86-32 cdecl
 *   All args on stack (no register seeding needed here; handled by var recovery).
 *   Return (int): EAX
 *   Return (fp):  ST0 (x87) or XMM0 (SSE)
 *
 * ### AArch64 (AAPCS64)
 *   Integer args: X0–X7  (64-bit each)
 *   Float args:   V0–V7  (128-bit)
 *   Return (int): X0
 *   Return (fp):  V0
 *
 * ## Struct recovery implementation
 *
 * Pattern recognition over all Load/Store instructions:
 *
 *   For a Load `v = LOAD base + k`:
 *     If `base` is a known Pointer value and k is a constant offset,
 *     record (base, k, access_width) in a per-pointer-value map.
 *
 *   After all accesses are collected:
 *     If a pointer has ≥ 2 distinct offsets → create a StructLayout,
 *     emit IsStruct(pointer, layout_id).
 *     If a pointer has only offset-0 accesses → leave as plain pointer.
 *
 * Array pattern:
 *   For LOAD base + phi_val * stride (where phi_val is a Phi node):
 *     Emit IsArray(base, 0, stride, Unknown).
 *
 * ## TypeInferencePass
 *
 * Orchestrates:
 *   1. Register all SSA values with TypePropagation.
 *   2. WidthSeeder → emit HasWidth constraints.
 *   3. emitInstructionConstraints() → emit signedness/pointer/float.
 *   4. AbiSeeder → emit ABI parameter/return seeds.
 *   5. StructRecovery → emit IsStruct / IsArray.
 *   6. TypePropagation::run() → fixpoint.
 *   7. collectStats().
 */

#include "retdec/type_inference/type_inference.h"
#include "retdec/ssa/ssa.h"
#include <algorithm>
#include <unordered_map>

namespace retdec {
namespace type_inference {

using namespace retdec::ssa;

// ═══════════════════════════════════════════════════════════════════════════════
// AbiSeeder
// ═══════════════════════════════════════════════════════════════════════════════

void AbiSeeder::seed(TypePropagation& prop, const AbiSeedInfo& info) const {
    // Seed parameter types
    for (auto& p : info.params) {
        prop.addValue(p.ssaId);

        if (p.type.kind == TypeKind::Float) {
            prop.addConstraint(TypeConstraint::isFloat(p.ssaId, p.type.width));
        } else if (p.type.kind == TypeKind::Integer) {
            if (p.type.width > 0)
                prop.addConstraint(TypeConstraint::hasWidth(p.ssaId, p.type.width));
            if (p.type.sign == Signedness::Signed)
                prop.addConstraint(TypeConstraint::isSigned(p.ssaId));
            else if (p.type.sign == Signedness::Unsigned)
                prop.addConstraint(TypeConstraint::isUnsigned(p.ssaId));
        } else if (p.type.kind == TypeKind::Pointer) {
            prop.addConstraint(TypeConstraint::isPointer(p.ssaId));
        }

        TypeConstraint ptc;
        ptc.kind       = ConstraintKind::ParamType;
        ptc.lhsId      = p.ssaId;
        ptc.width      = p.type.width;
        ptc.sign       = p.type.sign;
        ptc.paramIndex = p.paramIndex;
        prop.addConstraint(ptc);
    }

    // Seed return type
    if (info.retVal.has_value()) {
        auto& rv = info.retVal.value();
        prop.addValue(rv.ssaId);
        if (rv.type.kind == TypeKind::Float)
            prop.addConstraint(TypeConstraint::isFloat(rv.ssaId, rv.type.width));
        else if (rv.type.kind == TypeKind::Integer && rv.type.width > 0)
            prop.addConstraint(TypeConstraint::hasWidth(rv.ssaId, rv.type.width));

        TypeConstraint rtc;
        rtc.kind  = ConstraintKind::ReturnType;
        rtc.lhsId = rv.ssaId;
        rtc.width = rv.type.width;
        rtc.sign  = rv.type.sign;
        prop.addConstraint(rtc);
    }
}

AbiSeedInfo AbiSeeder::buildSysVx64Seed(
    const SSAFunction& fn,
    const std::vector<uint32_t>& argValueIds,
    const std::vector<IrType>&   argTypes,
    std::optional<IrType>        retType) const
{
    AbiSeedInfo info;
    info.abi = AbiSeedInfo::Abi::SysV_x64;

    for (std::size_t i = 0; i < argValueIds.size(); ++i) {
        AbiSeedInfo::ParamSeed ps;
        ps.ssaId      = argValueIds[i];
        ps.type       = (i < argTypes.size()) ? argTypes[i] : IrType::integer(64);
        ps.paramIndex = (uint8_t)i;
        info.params.push_back(ps);
    }

    // Locate the return value SSA ID by scanning for a Ret instruction in the
    // function and using its first operand (the value placed in RAX/X0/etc.).
    if (retType.has_value()) {
        ssa::ValueId retValId = ssa::kInvalidValue;
        for (const auto& blkPtr : fn.blocks()) {
            for (const ssa::IrInstr* instr : blkPtr->instrs) {
                if (!instr || instr->op != ssa::IrInstr::Op::Ret) continue;
                if (!instr->uses.empty())
                    retValId = instr->uses[0].valueId;
                break;
            }
            if (retValId != ssa::kInvalidValue) break;
        }

        if (retValId != ssa::kInvalidValue) {
            AbiSeedInfo::ReturnSeed rs;
            rs.ssaId = retValId;
            rs.type  = retType.value();
            info.retVal = rs;
        }
    }

    return info;
}

// ═══════════════════════════════════════════════════════════════════════════════
// StructRecovery
// ═══════════════════════════════════════════════════════════════════════════════

void StructRecovery::collectPatterns(const SSAFunction& fn,
                                       std::vector<AccessPattern>& out) const {
    for (auto& blkPtr : fn.blocks()) {
        for (auto* instr : blkPtr->instrs) {
            if (instr->op != IrInstr::Op::Load &&
                instr->op != IrInstr::Op::Store) continue;

            // Look for a MemRef use that has a non-zero base register
            for (auto& use : instr->uses) {
                const IrValue* v = fn.value(use.valueId);
                if (!v || v->kind != ValueKind::MemRef) continue;
                if (v->memIsStack) continue;  // stack handled separately
                if (v->memBaseReg == UINT32_MAX) continue;

                AccessPattern ap;
                ap.baseId  = v->memBaseReg;  // treat base reg ID as pointer SSA ID
                ap.offset  = v->memOffset;
                ap.width   = v->memWidth;
                ap.isWrite = (instr->op == IrInstr::Op::Store);
                out.push_back(ap);
            }
        }
    }
}

bool StructRecovery::isLoopVarying(const SSAFunction& fn,
                                     uint32_t valueId) const {
    const IrValue* v = fn.value(valueId);
    if (!v) return false;
    // A phi node is the canonical loop-varying value
    return v->kind == ValueKind::Phi;
}

StructRecovery::Result StructRecovery::run(const SSAFunction& fn) const {
    Result res;

    std::vector<AccessPattern> patterns;
    collectPatterns(fn, patterns);

    // Group by base pointer ID
    std::unordered_map<uint32_t, std::vector<AccessPattern>> byBase;
    for (auto& ap : patterns)
        byBase[ap.baseId].push_back(ap);

    uint32_t nextStructId = 1000;  // struct IDs start at 1000

    for (auto& [baseId, accesses] : byBase) {
        // Collect unique offsets
        std::unordered_map<int64_t, uint8_t> offsetWidths;
        for (auto& ap : accesses)
            offsetWidths[ap.offset] = std::max(offsetWidths[ap.offset], ap.width);

        if (offsetWidths.size() < 2) {
            // Check for array pattern (loop-varying index)
            // We look for any multiply-by-constant pattern in the accesses;
            // for now, if there's a Phi base value, it's likely an array.
            bool isArray = isLoopVarying(fn, baseId);
            if (isArray && !accesses.empty()) {
                // Infer stride from the access width
                uint8_t stride = accesses[0].width;
                res.constraints.push_back(
                    TypeConstraint::isArray(baseId, 0, stride, TypeKind::Unknown));
                ++res.arraysFound;
            }
            continue;
        }

        // Two or more distinct offsets → struct
        uint32_t sid = nextStructId++;
        StructLayout layout;
        layout.id   = sid;
        layout.name = "struct_" + std::to_string(sid);

        for (auto& [off, w] : offsetWidths) {
            StructField f;
            f.offset = (uint32_t)(off >= 0 ? off : 0);
            f.width  = w;
            f.name   = "field_" + std::to_string(f.offset);
            layout.addField(std::move(f));
        }

        // Compute total size from last field end
        uint32_t maxEnd = 0;
        for (auto& f : layout.fields) {
            uint32_t end = f.offset + f.width;
            if (end > maxEnd) maxEnd = end;
        }
        layout.size = maxEnd;

        res.constraints.push_back(TypeConstraint::isStruct(baseId, sid));
        res.layouts.push_back(std::move(layout));
        ++res.structsFound;
    }

    return res;
}

// ═══════════════════════════════════════════════════════════════════════════════
// TypeInferencePass — instruction constraint emission
// ═══════════════════════════════════════════════════════════════════════════════

void TypeInferencePass::emitInstructionConstraints(const SSAFunction& fn) {
    for (auto& blkPtr : fn.blocks()) {
        for (auto* instr : blkPtr->instrs) {
            uint32_t defId = instr->defValue;

            switch (instr->op) {
            case IrInstr::Op::Add:
            case IrInstr::Op::Sub:
            case IrInstr::Op::Mul:
                // Integer arithmetic — propagate widths between operands
                if (defId != UINT32_MAX && !instr->uses.empty()) {
                    for (auto& use : instr->uses)
                        prop_.addConstraint(TypeConstraint::sameWidth(defId, use.valueId));
                }
                break;

            case IrInstr::Op::Div:
                // Division: SAR/IDIV → signed; SHR/DIV → unsigned
                // We conservatively mark as integer
                if (defId != UINT32_MAX) {
                    // No sign information from generic Div
                }
                break;

            case IrInstr::Op::Sar:
                // Arithmetic right shift → value must be signed
                if (!instr->uses.empty())
                    prop_.addConstraint(TypeConstraint::isSigned(instr->uses[0].valueId));
                if (defId != UINT32_MAX)
                    prop_.addConstraint(TypeConstraint::isSigned(defId));
                break;

            case IrInstr::Op::Shr:
                // Logical right shift → unsigned
                if (!instr->uses.empty())
                    prop_.addConstraint(TypeConstraint::isUnsigned(instr->uses[0].valueId));
                if (defId != UINT32_MAX)
                    prop_.addConstraint(TypeConstraint::isUnsigned(defId));
                break;

            case IrInstr::Op::And:
            case IrInstr::Op::Or:
            case IrInstr::Op::Xor:
                // Bitwise → unsigned integer
                if (defId != UINT32_MAX)
                    prop_.addConstraint(TypeConstraint::isUnsigned(defId));
                for (auto& use : instr->uses)
                    prop_.addConstraint(TypeConstraint::isUnsigned(use.valueId));
                break;

            case IrInstr::Op::Load:
                // If a MemRef is dereferenced, the base is a pointer
                for (auto& use : instr->uses) {
                    const IrValue* v = fn.value(use.valueId);
                    if (v && v->kind == ValueKind::MemRef && !v->memIsStack)
                        prop_.addConstraint(TypeConstraint::isPointer(v->memBaseReg));
                }
                break;

            case IrInstr::Op::CondBranch:
                // Condition operand is boolean
                if (!instr->uses.empty())
                    prop_.addConstraint({ConstraintKind::IsBool, instr->uses[0].valueId});
                break;

            default:
                break;
            }

            // SameWidth between def and uses for most instructions
            if (defId != UINT32_MAX) {
                for (auto& use : instr->uses) {
                    if (instr->op == IrInstr::Op::Shl ||
                        instr->op == IrInstr::Op::Shr ||
                        instr->op == IrInstr::Op::Sar) {
                        // Only the first operand shares width with result
                        if (&use == &instr->uses[0])
                            prop_.addConstraint(TypeConstraint::sameWidth(defId, use.valueId));
                        break;
                    }
                }
            }
        }
    }
}

// ─── TypeInferencePass::run ───────────────────────────────────────────────────

void TypeInferencePass::run(const SSAFunction& fn, const Config& cfg) {
    structs_.clear();

    // Register all values
    for (auto& v : fn.values()) prop_.addValue(v->id);

    // Phase 1: width seeding
    WidthSeeder ws;
    auto widths = ws.run(fn);
    for (auto& [id, w] : widths.widths)
        prop_.addConstraint(TypeConstraint::hasWidth(id, w));

    // Phase 2a: instruction-semantic constraints
    emitInstructionConstraints(fn);

    // Phase 2b: ABI seeding
    if (cfg.abiSeed.abi != AbiSeedInfo::Abi::Unknown) {
        AbiSeeder abi;
        abi.seed(prop_, cfg.abiSeed);
    }

    // Phase 2c: struct / array recovery
    if (cfg.enableStructRecovery || cfg.enableArrayRecovery) {
        StructRecovery sr;
        auto srResult = sr.run(fn);
        for (auto& c : srResult.constraints) prop_.addConstraint(c);
        structs_ = std::move(srResult.layouts);
        stats_.structsRecovered = srResult.structsFound;
        stats_.arraysRecovered  = srResult.arraysFound;
    }

    // Phase 3: propagate
    prop_.run();

    // Collect stats
    collectStats(fn);
}

void TypeInferencePass::collectStats(const SSAFunction& fn) {
    stats_.totalConstraints     = prop_.constraintCount();
    stats_.typeClasses          = prop_.classCount();
    stats_.valuesWithKnownType  = 0;
    stats_.valuesWithKnownWidth = 0;
    stats_.valuesWithKnownSign  = 0;
    stats_.pointerValues        = 0;

    for (auto& v : fn.values()) {
        const IrType& t = typeOf(v->id);
        if (t.width > 0) ++stats_.valuesWithKnownWidth;
        if (t.isKnown()) {
            ++stats_.valuesWithKnownType;
            if (t.sign != Signedness::Unknown) ++stats_.valuesWithKnownSign;
            if (t.isPointer()) ++stats_.pointerValues;
        }
    }
}

const IrType& TypeInferencePass::typeOf(uint32_t ssaId) const {
    return prop_.typeOf(ssaId);
}

} // namespace type_inference
} // namespace retdec
