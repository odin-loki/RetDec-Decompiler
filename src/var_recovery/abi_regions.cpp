/**
 * @file src/var_recovery/abi_regions.cpp
 * @brief ABI-mandated frame region carving.
 *
 * ## Purpose
 *
 * Before DVSA analyses stack accesses, we carve out frame regions that are
 * defined by the ABI rather than the user's variables.  Any MemRef access
 * that falls entirely within a carved region is excluded from the variable
 * partition.
 *
 * ## Carved regions per ABI
 *
 * ### x86-64 System V (Linux / macOS)
 *
 *   +8  [RBP+8]          : return address
 *    0  [RBP+0]          : saved RBP (frame chain)
 *   -8  [RBP-8]          : first callee-save (typically RBX or first pushed reg)
 *   ...  callee-saves at known offsets from prologue scan
 *   -128..RSP             : 128-byte red zone (scratch area; must not be
 *                           disturbed by signal handlers, safe for leaf fns)
 *
 * ### x86-64 Windows
 *
 *   +8  [RSP+8 at call entry] : return address (relative to caller's RSP)
 *   [RSP+0..31]               : shadow space (home area for first 4 int args:
 *                               RCX, RDX, R8, R9 — each 8 bytes)
 *   callee-saves at PUSH offsets
 *   No red zone.
 *
 * ### x86-32
 *
 *   +4  [EBP+4]  : return address
 *    0  [EBP+0]  : saved EBP
 *   callee-saves at PUSH offsets
 *
 * ### AArch64 (AAPCS64)
 *
 *    0  [SP+0]   : saved X29 (frame pointer)
 *    8  [SP+8]   : saved X30 (link register)
 *   callee-saves at STP offsets
 *   No red zone, no shadow space.
 *
 * ### ARM32 (AAPCS)
 *
 *   PUSH offsets : saved R4-R11, LR
 *   SP+0..localSize-1 : local area
 *
 * ## Overlap check
 *
 * `isCarved(off, size)` returns true if [off, off+size) intersects any
 * carved region.  DVSA uses this as a fast exclusion filter.
 */

#include "retdec/var_recovery/var_recovery.h"
#include <algorithm>

namespace retdec {
namespace var_recovery {

// ─── Helpers ──────────────────────────────────────────────────────────────────

static bool overlaps(int64_t aStart, uint8_t aSize,
                      int64_t bStart, uint8_t bSize) {
    return aStart < (bStart + (int64_t)bSize) &&
           bStart < (aStart + (int64_t)aSize);
}

bool AbiRegionCarver::isCarved(const PrologueInfo& info,
                                 int64_t off, uint8_t sz) const {
    for (auto& r : info.abiRegions) {
        if (overlaps(off, sz, r.offset, r.size)) return true;
    }
    return false;
}

static void addRegion(PrologueInfo& info,
                       RegionKind kind, int64_t off, uint8_t sz,
                       std::string name) {
    info.abiRegions.push_back({kind, off, sz, std::move(name)});
}

// ─── x86-64 System V ─────────────────────────────────────────────────────────

void AbiRegionCarver::carveSysVx64(PrologueInfo& info) const {
    // Return address at [RBP+8]
    addRegion(info, RegionKind::ReturnAddress, +8, 8, "return_address");
    // Saved RBP (frame chain) at [RBP+0]
    if (info.hasFramePointer)
        addRegion(info, RegionKind::FrameChain, 0, 8, "saved_rbp");

    // Callee-saved register slots
    for (auto& [reg, off] : info.calleeSaves) {
        if (reg == Reg::RBP) continue;  // already carved as FrameChain
        std::string n = std::string("saved_") + regName(reg);
        addRegion(info, RegionKind::CalleeSave, off, 8, n);
    }

    // Red zone: [RBP-128..RBP-1] (128 bytes below RSP, i.e. below local area)
    if (info.hasRedZone) {
        int64_t rzStart = info.localAreaStart - 128;
        addRegion(info, RegionKind::RedZone, rzStart, 128, "red_zone");
    }
}

// ─── x86-64 Windows ──────────────────────────────────────────────────────────

void AbiRegionCarver::carveWin64(PrologueInfo& info) const {
    // Return address is above the frame; we use offset relative to RBP or RSP.
    // On Win64 without a frame pointer: return addr at [RSP + frameSize]
    int64_t retOff = info.hasFramePointer ? +8 : info.frameSize;
    addRegion(info, RegionKind::ReturnAddress, retOff, 8, "return_address");

    if (info.hasFramePointer)
        addRegion(info, RegionKind::FrameChain, 0, 8, "saved_rbp");

    // Callee saves
    for (auto& [reg, off] : info.calleeSaves) {
        if (reg == Reg::RBP) continue;
        std::string n = std::string("saved_") + regName(reg);
        addRegion(info, RegionKind::CalleeSave, off, 8, n);
    }

    // Shadow space: [RSP+0..31] at the time of the call to this function.
    // This is above our local area; represented as positive offsets from RBP.
    if (info.hasShadowSpace) {
        // RSP+0..31 relative to callee's RSP at function entry = above retaddr
        // We mark it as [retOff+8 .. retOff+8+32)
        addRegion(info, RegionKind::ShadowSpace, retOff + 8, 32, "shadow_space");
    }
}

// ─── x86-32 ──────────────────────────────────────────────────────────────────

void AbiRegionCarver::carveSysVx32(PrologueInfo& info) const {
    addRegion(info, RegionKind::ReturnAddress, +4, 4, "return_address");
    if (info.hasFramePointer)
        addRegion(info, RegionKind::FrameChain, 0, 4, "saved_ebp");

    for (auto& [reg, off] : info.calleeSaves) {
        if (reg == Reg::EBP) continue;
        std::string n = std::string("saved_") + regName(reg);
        addRegion(info, RegionKind::CalleeSave, off, 4, n);
    }
}

// ─── AArch64 ─────────────────────────────────────────────────────────────────

void AbiRegionCarver::carveAArch64(PrologueInfo& info) const {
    // Saved X29 at [SP+0], X30 at [SP+8] (relative to new SP after STP)
    addRegion(info, RegionKind::FrameChain, 0, 8, "saved_x29");
    addRegion(info, RegionKind::ReturnAddress, 8, 8, "saved_x30_lr");

    for (auto& [reg, off] : info.calleeSaves) {
        if (reg == Reg::X29 || reg == Reg::X30) continue;
        std::string n = std::string("saved_") + regName(reg);
        addRegion(info, RegionKind::CalleeSave, off, 8, n);
    }
}

// ─── ARM32 ───────────────────────────────────────────────────────────────────

void AbiRegionCarver::carveARM32(PrologueInfo& info) const {
    for (auto& [reg, off] : info.calleeSaves) {
        std::string n = std::string("saved_") + regName(reg);
        RegionKind kind = (reg == Reg::LR)
                          ? RegionKind::ReturnAddress
                          : (reg == (Reg)((int)Reg::R0 + 11)
                             ? RegionKind::FrameChain
                             : RegionKind::CalleeSave);
        addRegion(info, kind, off, 4, n);
    }
}

// ─── Dispatch ─────────────────────────────────────────────────────────────────

void AbiRegionCarver::carve(PrologueInfo& info) const {
    info.abiRegions.clear();
    switch (info.abi) {
    case ABI::SysV_x86_64: carveSysVx64(info); break;
    case ABI::Win64:        carveWin64(info);   break;
    case ABI::Win32:        [[fallthrough]];
    case ABI::SysV_x86_32: carveSysVx32(info); break;
    case ABI::AAPCS64:      carveAArch64(info); break;
    case ABI::AAPCS32:      carveARM32(info);   break;
    default: break;
    }
    // Sort regions by offset for fast binary-search later
    std::sort(info.abiRegions.begin(), info.abiRegions.end(),
              [](const FrameRegion& a, const FrameRegion& b) {
                  return a.offset < b.offset;
              });
}

} // namespace var_recovery
} // namespace retdec
