/**
 * @file src/mini_emu/mini_unpacker.cpp
 * @brief High-level unpacker: runs MiniEmu, dumps memory, rebuilds sections.
 */

#include "retdec/mini_emu/mini_unpacker.h"
#include "retdec/mini_emu/mini_emu.h"

#include <algorithm>
#include <cstring>

namespace retdec {
namespace mini_emu {

// ─── Prologue scan ───────────────────────────────────────────────────────────

bool MiniUnpacker::looksLikeCode(const uint8_t *data, size_t size)
{
    if (!data || size < 4) return false;
    // Check for common x86-64 function prologues
    for (size_t i = 0; i + 4 <= size; ++i) {
        // push rbp; mov rbp, rsp (55 48 89 E5)
        if (data[i]==0x55 && data[i+1]==0x48 && data[i+2]==0x89 && data[i+3]==0xE5) return true;
        // push rbp (55 8B EC — x86)
        if (data[i]==0x55 && data[i+1]==0x8B && data[i+2]==0xEC) return true;
        // sub rsp, imm8 (48 83 EC)
        if (data[i]==0x48 && data[i+1]==0x83 && data[i+2]==0xEC) return true;
    }
    return false;
}

// ─── Build dump ──────────────────────────────────────────────────────────────

std::vector<uint8_t> MiniUnpacker::buildDump(const MiniEmu &emu,
                                               const std::vector<UnpackedRegion> &regions,
                                               uint64_t &outBase,
                                               std::vector<SectionInfo> &outSections) const
{
    if (regions.empty()) { outBase = 0; return {}; }

    // Find lowest VA
    uint64_t minVA = regions[0].startVA;
    uint64_t maxVA = 0;
    for (const auto &r : regions) {
        minVA = std::min(minVA, r.startVA);
        maxVA = std::max(maxVA, r.startVA + r.bytes.size());
    }
    outBase = minVA;

    // maxVA - minVA is the span between the lowest and highest addresses the
    // emulator touched, and the emulator's stack sits at kDefaultStackBase =
    // 0x00007FFFFFFF0000. Any function that pushes anything leaves a written
    // region there, 128 TiB above the image, and this allocated all of it: a
    // `push rax; hlt` at 0x1000 threw std::bad_alloc out of unpack().
    //
    // So the window comes first, and everything downstream works from the
    // regions inside it. The section walk below indexes the dump by
    // `curBase - minVA`, so a region left outside the window would read past
    // the end of it -- which is what happened when only the allocation was
    // capped: SIGSEGV in looksLikeCode instead of a bad_alloc.
    std::vector<UnpackedRegion> kept;
    kept.reserve(regions.size());
    uint64_t keptMaxVA = minVA;
    for (const auto &r : regions) {
        if (r.startVA < minVA) continue;
        if (r.startVA - minVA >= kMaxDumpBytes) continue;
        keptMaxVA = std::max(keptMaxVA, r.startVA + r.bytes.size());
        kept.push_back(r);
    }
    if (kept.empty()) { outBase = minVA; return {}; }

    // Size the dump from what survived, not from the cap: a 4 KiB image with
    // one stack region 128 TiB away wants a 4 KiB dump, not a 256 MiB one.
    const uint64_t span = std::min<uint64_t>(keptMaxVA - minVA, kMaxDumpBytes);
    const size_t dumpSize = static_cast<size_t>(span);
    (void)maxVA;

    // Build flat dump.
    std::vector<uint8_t> dump(dumpSize, 0);
    for (const auto &r : kept) {
        size_t off = static_cast<size_t>(r.startVA - minVA);
        size_t cplen = std::min(r.bytes.size(), dumpSize - off);
        std::memcpy(dump.data() + off, r.bytes.data(), cplen);
    }

    // Build sections — contiguous executed / written regions
    uint64_t curBase = kept[0].startVA;
    bool     curExec = kept[0].isCode;
    size_t   curSize = kept[0].bytes.size();

    for (size_t ri = 1; ri <= kept.size(); ++ri) {
        bool flush = (ri == kept.size());
        if (!flush) {
            const auto &r = kept[ri];
            if (r.startVA == curBase + curSize) {
                curSize += r.bytes.size();
                curExec = curExec || r.isCode;
                continue;
            }
            flush = true;
        }
        // curBase is inside the window by construction; curSize is the run of
        // adjacent kept regions from it, so clamp it to what the dump holds.
        const size_t secOff = static_cast<size_t>(curBase - minVA);
        const size_t secLen = std::min(curSize, dumpSize - secOff);
        bool isCode = curExec || looksLikeCode(dump.data() + secOff, secLen);
        SectionInfo si;
        si.name           = isCode ? ".dump_code" : ".dump_data";
        si.virtualAddress = curBase;
        si.virtualSize    = curSize;
        si.fileOffset     = secOff;
        si.fileSize       = secLen;
        si.isExecutable   = isCode;
        si.isWritable     = true;
        si.isReadable     = true;
        outSections.push_back(si);

        if (ri < kept.size()) {
            const auto &r = kept[ri];
            curBase = r.startVA;
            curExec = r.isCode;
            curSize = r.bytes.size();
        }
    }

    return dump;
}

// ─── unpack() ────────────────────────────────────────────────────────────────

UnpackerOutput MiniUnpacker::unpack(const uint8_t *data, size_t size,
                                     const FormatResult &fmt,
                                     uint64_t maxInsns) const
{
    UnpackerOutput out;
    if (!data || size == 0) {
        out.errorMsg = "Empty input";
        return out;
    }

    MiniEmu emu;
    emu.load(data, size, fmt);

    uint64_t ep = fmt.entryPoint;
    if (ep == 0) {
        out.errorMsg = "No entry point";
        return out;
    }

    UnpackResult res = emu.run(ep, maxInsns);

    out.stopReason            = res.stopReason;
    out.instructionsExecuted  = res.instructionsExecuted;
    out.needsManualReview     = res.needsManualReview;
    out.success               = res.success && !res.regions.empty();

    if (!res.regions.empty()) {
        uint64_t dumpBase = 0;
        out.dump = buildDump(emu, res.regions, dumpBase, out.sections);
        // Entry point in dump: offset from dumpBase
        if (res.epAfterUnpack >= dumpBase && res.epAfterUnpack < dumpBase + out.dump.size()) {
            out.entryPointOffset = static_cast<uint64_t>(res.epAfterUnpack - dumpBase);
        }
    }

    if (!out.success) {
        out.errorMsg = "Emulation terminated: " + stopReasonToString(res.stopReason);
    }

    return out;
}

} // namespace mini_emu
} // namespace retdec
