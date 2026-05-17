/**
 * @file src/var_recovery/var_namer.cpp
 * @brief Variable naming + VarRecoveryPass orchestration.
 *
 * ## Naming strategy
 *
 * Variables are named in priority order:
 *
 *   1. DWARF DW_AT_name  — if a DWARF variable's CFA-relative offset
 *      matches the slot's baseOffset within ±1 byte (to tolerate minor
 *      DWARF alignment rounding), use the DWARF name.
 *
 *   2. Callee-save register names  — "saved_rbx", "saved_r12", etc.
 *      Recognised by matching the slot offset against the PrologueInfo
 *      calleeSaves list.
 *
 *   3. Argument names  — slots with positive offsets (above RBP) that
 *      correspond to stack-passed arguments are named "arg0", "arg1", …
 *      We detect argument slots by checking if their offset falls in the
 *      argument area: offset >= +8 (return address is at +8 on x86-64;
 *      first stack arg is at +16).
 *
 *   4. Auto-generated names  — based on the widest access observed:
 *        1 byte  → b<n>    (byte variable)
 *        2 bytes → w<n>    (word)
 *        4 bytes → d<n>    (dword / int)
 *        8 bytes → q<n>    (qword / long)
 *        pointer-width → ptr<n>  (assigned by the caller if type known)
 *       16+ bytes → v<n>  (vector / struct)
 *
 *   Auto counters are per-type and per-function, so a function with
 *   3 dword variables gets d0, d1, d2.
 *
 * ## VarRecoveryPass
 *
 * Orchestrates the full pipeline:
 *   1. PrologueParser::parse()
 *   2. AbiRegionCarver::carve()
 *   3. DVSA::run()
 *   4. Assemble VariableCandidate list from DVSA slots
 *   5. VariableNamer::name()
 *   6. Compute DWARF match count
 */

#include "retdec/var_recovery/var_recovery.h"
#include <algorithm>
#include <cstring>

namespace retdec {
namespace var_recovery {

// ─── Type prefix helpers ──────────────────────────────────────────────────────

// Index into the counters array:  0=b, 1=w, 2=d, 3=q, 4=ptr, 5=v
static int prefixIndex(uint8_t maxAccess) {
    switch (maxAccess) {
    case 1:  return 0;
    case 2:  return 1;
    case 4:  return 2;
    case 8:  return 3;
    default: return (maxAccess >= 16) ? 5 : 4;
    }
}

const char* VariableNamer::typePrefix(uint8_t maxAccess) const {
    static const char* prefixes[] = { "b", "w", "d", "q", "ptr", "v" };
    return prefixes[prefixIndex(maxAccess)];
}

std::string VariableNamer::autoName(const FrameSlot& slot,
                                      uint32_t counters[6]) const {
    int idx = prefixIndex(slot.maxAccess);
    std::string name = std::string(typePrefix(slot.maxAccess))
                       + std::to_string(counters[idx]++);
    return name;
}

// ─── Naming pass ──────────────────────────────────────────────────────────────

void VariableNamer::name(std::vector<VariableCandidate>& candidates,
                           const PrologueInfo& prologue,
                           const std::vector<DwarfVarInfo>& dwarf) const {

    uint32_t counters[6] = {0, 0, 0, 0, 0, 0};  // b, w, d, q, ptr, v
    uint32_t argCounter  = 0;

    for (auto& cand : candidates) {
        if (!cand.name.empty()) continue;  // already named

        int64_t off = cand.slot.baseOffset;

        // 1. Callee-save name
        if (cand.isCalleeSave) {
            for (auto& [reg, saveOff] : prologue.calleeSaves) {
                if (saveOff == off) {
                    cand.name = std::string("saved_") + regName(reg);
                    break;
                }
            }
            if (!cand.name.empty()) continue;
        }

        // 2. DWARF name (match within ±1 byte to handle DWARF alignment)
        for (auto& di : dwarf) {
            if (std::abs(di.frameOffset - off) <= 1) {
                cand.name = di.name;
                cand.isDwarfNamed = true;
                break;
            }
        }
        if (!cand.name.empty()) continue;

        // 3. Stack argument
        // On x86-64: first stack arg is at [RBP+16] (above ret addr at +8)
        // On x86-32: first stack arg is at [EBP+8]
        int64_t firstArgOff = (prologue.arch == Arch::X86_64) ? 16 : 8;
        if (off >= firstArgOff) {
            cand.isArg = true;
            cand.name  = "arg" + std::to_string(argCounter++);
            continue;
        }

        // 4. Auto name
        if (cand.isUnion) {
            cand.name = "u" + std::to_string(counters[5]++);
        } else {
            cand.name = autoName(cand.slot, counters);
        }
    }
}

// ─── VarRecoveryPass ─────────────────────────────────────────────────────────

VarRecoveryPass::Result VarRecoveryPass::run(
    const ssa::SSAFunction& fn,
    const std::vector<RawInstr>& prologueInstrs,
    const Config& cfg) const {

    Result result;

    // 1. Parse prologue
    PrologueParser parser(cfg.abi, cfg.arch);
    result.prologue = parser.parse(prologueInstrs);

    // 2. Carve ABI regions
    AbiRegionCarver carver;
    carver.carve(result.prologue);

    // 3. DVSA
    DVSA dvsa;
    result.dvsaStats = dvsa.run(fn, result.prologue);

    uint32_t nextId = 0;

    // 4. Assemble normal variable candidates from DVSA slots
    for (auto& slot : result.dvsaStats.slots) {
        VariableCandidate cand;
        cand.id   = nextId++;
        cand.slot = slot;
        cand.isUnion = false;

        // Check if this slot corresponds to a callee-save region
        for (auto& region : result.prologue.abiRegions) {
            if (region.kind == RegionKind::CalleeSave &&
                region.offset == slot.baseOffset) {
                cand.isCalleeSave = true;
                cand.name = region.name;
                break;
            }
        }

        // Collect SSA value IDs that reference this slot
        for (auto& acc : slot.accesses) {
            if (acc.ssaValue != UINT32_MAX)
                cand.ssaValueIds.push_back(acc.ssaValue);
        }

        result.candidates.push_back(std::move(cand));
    }

    // 5. Assemble union candidates
    for (auto& slot : result.dvsaStats.unionSlots) {
        VariableCandidate cand;
        cand.id      = nextId++;
        cand.slot    = slot;
        cand.isUnion = true;

        // Sub-slots for the union members
        for (auto& acc : slot.accesses) {
            FrameSlot sub;
            sub.baseOffset = acc.offset;
            sub.totalSize  = acc.size;
            sub.maxAccess  = acc.size;
            sub.hasWrite   = acc.isWrite;
            sub.hasRead    = !acc.isWrite;
            sub.accesses   = {acc};
            cand.unionMembers.push_back(sub);
            if (acc.ssaValue != UINT32_MAX)
                cand.ssaValueIds.push_back(acc.ssaValue);
        }

        result.candidates.push_back(std::move(cand));
    }

    // 6. Name all candidates
    VariableNamer namer;
    namer.name(result.candidates, result.prologue, cfg.dwarf);

    // 7. Count DWARF matches
    result.dwarfMatchCount = 0;
    for (auto& c : result.candidates)
        if (c.isDwarfNamed) ++result.dwarfMatchCount;

    return result;
}

} // namespace var_recovery
} // namespace retdec
