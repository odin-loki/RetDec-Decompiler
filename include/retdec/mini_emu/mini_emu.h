/**
 * @file include/retdec/mini_emu/mini_emu.h
 * @brief Emulation-bounded unpacker (Stage 3) — host-side MiniEmu API.
 *
 * MiniEmu is a self-contained x86-64 user-space emulator built on top of a
 * minimal interpretive engine (no external unicorn/QEMU dependency).  It is
 * not intended to be a full CPU emulator; it only needs to be faithful enough
 * to unpack common loaders (UPX, MPRESS, simple custom packers).
 *
 * Termination conditions (any one triggers end-of-emulation):
 *   (a) CALL/JMP to an address outside the original executable sections.
 *   (b) CALL/JMP to a region that received writes (newly unpacked code).
 *   (c) VirtualProtect / mprotect with PROT_EXEC on a non-executable region.
 *   (d) Maximum instruction limit reached (default: 10M).
 *
 * Anti-emulation countermeasures:
 *   - RDTSC: returns monotonically increasing synthetic value.
 *   - CPUID: returns plausible Intel Core i7 values.
 *   - IsDebuggerPresent (via INT 3 detection / direct call): returns 0.
 *   - Sleep / GetTickCount / QueryPerformanceCounter: return immediately.
 *
 * Memory model:
 *   - A flat address space is simulated with std::map<uint64_t, MemPage>.
 *   - Each page is 4 KiB and has read/write/execute permissions.
 *   - Original binary pages are loaded at their virtual addresses.
 *   - Stack is allocated at a fixed high address.
 *
 * UnpackResult contains the full memory state at termination plus a
 * synthetic section table rebuilt from written+executed regions.
 */

#pragma once

#include "retdec/fileformat/lattice/format_result.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace mini_emu {

using FormatResult = retdec::fileformat::lattice::FormatResult;
using SectionInfo  = retdec::fileformat::lattice::SectionInfo;

// ─── Constants ────────────────────────────────────────────────────────────────

constexpr uint64_t kDefaultStackBase   = 0x0000'7FFF'FFFF'0000ULL;
constexpr size_t   kStackSize          = 0x10000; // 64 KiB
constexpr size_t   kPageSize           = 0x1000;  // 4 KiB
constexpr uint64_t kMaxInstructions    = 10'000'000ULL;

// ─── Page permissions ─────────────────────────────────────────────────────────

struct PagePerms {
    bool read    = false;
    bool write   = false;
    bool execute = false;
};

// ─── Memory page ─────────────────────────────────────────────────────────────

struct MemPage {
    std::vector<uint8_t> data;
    PagePerms            perms;
    bool                 wasWritten  = false; ///< received a write during emulation
    bool                 wasExecuted = false; ///< at least one insn executed from here
};

// ─── Emulation stop reason ────────────────────────────────────────────────────

enum class StopReason {
    EnteredNewCode,          ///< (a)/(b) JMP/CALL to written/foreign region
    VProtectExec,            ///< (c) new PROT_EXEC region
    MaxInstructions,         ///< (d) instruction limit reached
    Halt,                    ///< HLT instruction
    Error,                   ///< unrecoverable fetch/decode error
};

std::string stopReasonToString(StopReason r);

// ─── CPU state ────────────────────────────────────────────────────────────────

struct CPUState {
    uint64_t rax = 0, rbx = 0, rcx = 0, rdx = 0;
    uint64_t rsi = 0, rdi = 0, rbp = 0, rsp = 0;
    uint64_t r8  = 0, r9  = 0, r10 = 0, r11 = 0;
    uint64_t r12 = 0, r13 = 0, r14 = 0, r15 = 0;
    uint64_t rip = 0;
    uint64_t rflags = 0x202; ///< IF=1, reserved=1

    uint64_t tscBase = 0; ///< used for RDTSC spoofing
    uint64_t tscStep = 1000; ///< monotonic step per emulated insn
};

// ─── UnpackResult ─────────────────────────────────────────────────────────────

struct UnpackedRegion {
    uint64_t             startVA;
    std::vector<uint8_t> bytes;
    bool                 isCode;   ///< true if this region was executed
};

struct UnpackResult {
    bool     success      = false;
    StopReason stopReason  = StopReason::Error;
    uint64_t   epAfterUnpack = 0; ///< RIP at termination (new entry point)
    uint64_t   instructionsExecuted = 0;

    std::vector<UnpackedRegion> regions;
    std::vector<SectionInfo>    syntheticSections; ///< reconstructed from written+exec pages

    bool needsManualReview = false; ///< true when MaxInstructions reached
};

// ─── MiniEmu ──────────────────────────────────────────────────────────────────

class MiniEmu {
public:
    MiniEmu();
    ~MiniEmu();

    MiniEmu(const MiniEmu &) = delete;
    MiniEmu &operator=(const MiniEmu &) = delete;

    /**
     * Load a binary image and set up the address space.
     * Must be called before run().
     *
     * @param data          Raw binary bytes.
     * @param size          Total file size.
     * @param fmt           Parsed format result (sections, EP, imageBase).
     */
    void load(const uint8_t *data, size_t size, const FormatResult &fmt);

    /**
     * Run the emulator starting at the given entry point.
     * Returns when a termination condition is hit.
     *
     * @param entryPoint    Virtual address to start execution.
     * @param maxInsns      Maximum instructions before forced stop.
     */
    UnpackResult run(uint64_t entryPoint,
                     uint64_t maxInsns = kMaxInstructions);

    /**
     * Read a byte from the emulated address space.
     * Returns false if the address is not mapped/readable.
     */
    bool readByte(uint64_t va, uint8_t &out) const;

    /**
     * Write a byte into the emulated address space.
     * Returns false if the address is not mapped/writable.
     */
    bool writeByte(uint64_t va, uint8_t val);

    /**
     * Map a new page into the address space.
     * Existing pages at overlapping addresses are overwritten.
     */
    void mapPage(uint64_t va, PagePerms perms, const uint8_t *data = nullptr,
                 size_t size = kPageSize);

    const CPUState &cpuState() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mini_emu
} // namespace retdec
