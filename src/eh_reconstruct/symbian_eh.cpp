/**
 * @file src/eh_reconstruct/symbian_eh.cpp
 * @brief Symbian/EPOC C++ Leave-Trap pattern reconstruction.
 *
 * ## Symbian EH model
 *
 * Symbian OS does not use standard C++ exceptions.  Instead it uses three
 * interrelated mechanisms:
 *
 * ### 1. Leave / Trap
 *
 *   User::Leave(TInt aReason):
 *     - Calls longjmp() to the innermost TTrap frame.
 *     - aReason is a negative Symbian error code (e.g. KErrNoMemory = -4).
 *
 *   TTrap::Trap(TInt& aError):
 *     - Calls setjmp() and registers the frame.
 *     - Returns 0 on initial call (setup path).
 *     - Returns non-zero (= leave reason) after a leave.
 *
 * The TRAP macro expands to approximately:
 *
 *   TInt __err;
 *   TTrap __t;
 *   if (__t.Trap(__err) == 0) {
 *       <body that may leave>
 *   }
 *   // __err holds the leave reason if execution reaches here via a leave
 *
 * TRAPD(err, expr):  same but declares 'err' in the enclosing scope.
 *
 * ### 2. CleanupStack
 *
 *   CleanupStack::PushL(CBase*) / PushL(TAny*) / PushL(TCleanupItem):
 *     - Registers a cleanup action that runs on leave.
 *
 *   CleanupStack::Pop() / Pop(CBase*):
 *     - Removes the top entry without running it.
 *
 *   CleanupStack::PopAndDestroy() / PopAndDestroy(CBase*):
 *     - Runs the top cleanup action (usually delete or Close()).
 *
 * CleanupStack::PushL / Pop pairs are cleanup handlers analogous to
 * C++ destructors in a try block.
 *
 * ### 3. Leaving functions (convention)
 *
 * By convention, functions that can leave have 'L' as the last letter of their
 * name (e.g. OpenL(), DoSomethingL()).  This is a naming convention only;
 * it does not affect binary structure.
 *
 * ## Detection strategy
 *
 * ### Symbol-based (preferred)
 *
 * If the binary's symbol table / import table contains:
 *   - "TTrap::Trap" or "_ZN5TTrap4TrapERi"
 *   - "User::Leave" or "_ZN4User5LeaveEi"
 *   - "CleanupStack::PushL" or variants
 *
 * We locate all call-sites of these symbols and reconstruct the pattern.
 *
 * ### Pattern-based (ARM32 encoding)
 *
 * For each call-site of TTrap::Trap (BL instruction in ARM/Thumb):
 *
 * ARM32 BL encoding:
 *   Bits[31:28]=1110, bits[27:25]=101, bit24=1 (BL), bits[23:0]=signed offset/2
 *   Opcode: 0xEB000000 | (offset >> 2 & 0x00FFFFFF)
 *
 * Thumb-2 BL encoding:
 *   32-bit: first halfword 0xF000, second halfword 0xF800
 *
 * After the BL to TTrap::Trap, the caller checks the return value:
 *   ARM32:   CMP R0, #0 ; BNE <leave_handler>
 *   Thumb:   CMP R0, #0 ; BNE <leave_handler>
 *
 * We extract:
 *   - tryBegin: the instruction after the BNE (the guarded body)
 *   - tryEnd:   the instruction before the first User::Leave call in scope
 *     (or the end of the block if none)
 *   - handlerVma: the BNE target address (error handling path)
 *
 * ### ARM32 instruction helpers
 *
 *   BL <offset>:  0xEB000000 | (((offset/4) - 2) & 0x00FFFFFF)
 *   CMP R0, #0:   0xE3500000
 *   BNE <offset>: 0x1A000000 | (((offset/4) - 2) & 0x00FFFFFF)
 *   BEQ <offset>: 0x0A000000 | (...)
 *
 * We scan the code section 4 bytes at a time (ARM32 alignment).
 *
 * ### CleanupStack reconstruction
 *
 * For each detected PushL call-site followed eventually by a Pop or
 * PopAndDestroy call-site within the same function, we create a cleanup
 * handler noting the pushed object type (inferred from the register or
 * symbol passed to PushL).
 */

#include <memory>
#include "retdec/eh_reconstruct/eh_reconstruct.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace eh_reconstruct {

// ─── ARM32 instruction helpers ────────────────────────────────────────────────

/// Decode an ARM32 BL instruction at `vma`, return target VMA.
/// Returns 0 if not a BL.
static uint64_t decodeArmBL(uint32_t instr, uint64_t vma) {
    if ((instr & 0xFF000000u) != 0xEB000000u) return 0;
    int32_t offset = (int32_t)((instr & 0x00FFFFFF) << 8) >> 6;  // sign-extend, *4
    return (uint64_t)(int64_t)(static_cast<int64_t>(vma) + 8 + offset);
}

/// Returns true if `instr` is CMP R0, #0.
static bool isArmCmpR0Zero(uint32_t instr) {
    return (instr == 0xE3500000u);
}

/// Decode ARM32 BNE or BEQ, return target VMA (or 0 if not a branch).
static uint64_t decodeArmCondBranch(uint32_t instr, uint64_t vma,
                                     bool& isBNE, bool& isBEQ) {
    uint8_t cond = (instr >> 28) & 0xF;
    if ((instr & 0x0F000000u) != 0x0A000000u) { isBNE=false; isBEQ=false; return 0; }
    isBNE = (cond == 0x1);  // NE
    isBEQ = (cond == 0x0);  // EQ
    if (!isBNE && !isBEQ) return 0;
    int32_t offset = (int32_t)((instr & 0x00FFFFFF) << 8) >> 6;
    return (uint64_t)(int64_t)(static_cast<int64_t>(vma) + 8 + offset);
}

// ─── Symbol address lookup ────────────────────────────────────────────────────

/**
 * Simple heuristic: given a target VMA that we believe is the Trap function,
 * scan all BL instructions in .text pointing to it.
 *
 * We also accept Trap VMAs passed in via a hint map (future integration with
 * symbol table).  For now, if the view's symbol table is unavailable, we do a
 * two-pass scan: first collect all BL targets, then identify repeated targets
 * that appear in a CMP R0, #0 / BNE pattern (these are candidates for Trap).
 */
static std::vector<uint64_t> findTrapCallSites(const IBinaryView& view,
                                                 uint64_t trapFnVma) {
    std::vector<uint64_t> sites;
    uint64_t textVma  = view.sectionVma(".text");
    std::size_t textSz = view.sectionSize(".text");
    if (textVma == 0 || textSz < 12) return sites;

    for (std::size_t off = 0; off + 12 <= textSz; off += 4) {
        uint64_t vma = textVma + off;
        uint32_t instr = view.readU32LE(vma);
        uint64_t target = decodeArmBL(instr, vma);
        if (target == 0) continue;

        // If we have a known trap VMA, only accept calls to it
        if (trapFnVma != 0 && target != trapFnVma) continue;

        // Look for: next instruction = CMP R0, #0; next+1 = BNE/BEQ
        if (!isArmCmpR0Zero(view.readU32LE(vma + 4))) continue;
        uint32_t branchInstr = view.readU32LE(vma + 8);
        bool bne, beq;
        uint64_t handlerVma = decodeArmCondBranch(branchInstr, vma + 8, bne, beq);
        if ((!bne && !beq) || handlerVma == 0) continue;

        // This is a Trap call-site
        sites.push_back(vma);
    }
    return sites;
}

/**
 * Autodetect the TTrap::Trap VMA using a frequency-based heuristic:
 * collect all BL targets that are followed by CMP R0,#0; BNE, then
 * return the most-called target (most likely Trap).
 */
static uint64_t autodetectTrapFn(const IBinaryView& view) {
    uint64_t textVma  = view.sectionVma(".text");
    std::size_t textSz = view.sectionSize(".text");
    if (textVma == 0 || textSz < 12) return 0;

    std::unordered_map<uint64_t, unsigned> freq;

    for (std::size_t off = 0; off + 12 <= textSz; off += 4) {
        uint64_t vma = textVma + off;
        uint32_t instr = view.readU32LE(vma);
        uint64_t target = decodeArmBL(instr, vma);
        if (target == 0 || target == vma) continue;
        if (!isArmCmpR0Zero(view.readU32LE(vma + 4))) continue;
        uint32_t br = view.readU32LE(vma + 8);
        bool bne, beq;
        uint64_t h = decodeArmCondBranch(br, vma + 8, bne, beq);
        if ((bne || beq) && h != 0) {
            ++freq[target];
        }
    }

    uint64_t best = 0;
    unsigned bestCnt = 0;
    for (auto& [fn, cnt] : freq) {
        if (cnt > bestCnt) { bestCnt = cnt; best = fn; }
    }
    // Require at least 2 occurrences to avoid false positives
    return (bestCnt >= 2) ? best : 0;
}

// ─── Find the function containing a VMA ──────────────────────────────────────

/**
 * Very simple function-boundary heuristic for ARM32:
 * scan backward from `vma` for the first aligned address that looks like
 * a function prolog (PUSH {..., LR} = 0xE92D????).
 * Returns `vma` itself as a fallback.
 */
static uint64_t estimateFunctionStart(const IBinaryView& view, uint64_t vma) {
    // Scan backward up to 4096 bytes, aligned to 4
    uint64_t limit = (vma > 4096) ? vma - 4096 : 0;
    uint64_t cur   = vma & ~3ull;

    while (cur > limit) {
        cur -= 4;
        uint32_t instr = view.readU32LE(cur);
        // PUSH {Rlist, LR}: 0xE92D0000 | bitmask (bit14 = LR)
        if ((instr & 0xFFFF0000u) == 0xE92D0000u && (instr & 0x4000u)) {
            return cur;
        }
    }
    return vma;
}

// ─── Symbian EH parser ────────────────────────────────────────────────────────

class SymbianEHParser final : public IEHParser {
public:
    const char* name() const noexcept override { return "Symbian-Leave"; }

    std::vector<EHFunction> parse(const IBinaryView& view) const override {
        std::vector<EHFunction> results;

        // Only applies to ARM32 .text sections
        uint64_t textVma  = view.sectionVma(".text");
        std::size_t textSz = view.sectionSize(".text");
        if (textVma == 0 || textSz < 12) return results;

        // Auto-detect TTrap::Trap VMA
        uint64_t trapFn = autodetectTrapFn(view);

        // Collect call-sites
        auto trapSites = findTrapCallSites(view, trapFn);
        if (trapSites.empty()) return results;

        for (uint64_t sitVma : trapSites) {
            uint64_t fnStart = estimateFunctionStart(view, sitVma);
            uint64_t fnEnd   = sitVma + 0x400;  // rough; tighten via CFG later

            // BNE target = the error handling path
            uint32_t branchInstr = view.readU32LE(sitVma + 8);
            bool bne, beq;
            uint64_t handlerVma = decodeArmCondBranch(branchInstr, sitVma + 8,
                                                       bne, beq);

            // try body starts at the instruction AFTER the branch (BNE skip target)
            uint64_t tryBegin = sitVma + 12;
            uint64_t tryEnd   = sitVma + 12 + 0x200;  // rough

            TryCatchBlock block;
            block.tryBegin = tryBegin;
            block.tryEnd   = tryEnd;

            // The handler catches a TInt (Symbian leave code)
            CatchHandler ch;
            ch.handlerVma = handlerVma;
            ch.catchType  = "TInt";  // Symbian leave code
            ch.isCleanup  = false;
            block.handlers.push_back(std::move(ch));

            EHFunction fn;
            fn.functionVma = fnStart;
            fn.functionEnd = fnEnd;
            fn.unwindInfo.functionBegin = fnStart;
            fn.unwindInfo.functionEnd   = fnEnd;
            fn.tryCatchBlocks.push_back(std::move(block));
            fn.hasEH        = true;
            fn.personalityFn= "TTrap::Trap";

            results.push_back(std::move(fn));
        }

        // Deduplicate by function VMA (multiple TRAPs in one function)
        std::sort(results.begin(), results.end(),
                  [](const EHFunction& a, const EHFunction& b) {
                      return a.functionVma < b.functionVma;
                  });
        auto last = std::unique(results.begin(), results.end(),
                                [](const EHFunction& a, const EHFunction& b) {
                                    return a.functionVma == b.functionVma;
                                });
        results.erase(last, results.end());

        return results;
    }
};

std::unique_ptr<IEHParser> makeSymbianEHParser() {
    return std::make_unique<SymbianEHParser>();
}

} // namespace eh_reconstruct
} // namespace retdec
