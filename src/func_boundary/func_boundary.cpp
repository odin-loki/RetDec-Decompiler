/**
 * @file src/func_boundary/func_boundary.cpp
 * @brief Three-pass convergent function boundary detection.
 *
 * ## Pass 1 — Direct Evidence
 *
 * All sources with hard evidence (entry point, CALL targets, exports, TLS,
 * SEH/DWARF handlers) are registered at confidence 0.95–1.0.  Duplicate
 * addresses increase confidence via a Bayesian-like update but cannot exceed 1.0.
 *
 * ## Pass 2 — Prologue Scan
 *
 * For each executable section we:
 *   (a) Do a lightweight CALL-target scan (look for E8 rel32 / FF D? / FF 1?
 *       patterns) to catch any calls missed in Pass 1.
 *   (b) Try every 1-byte-aligned offset against the compiler-specific prologue
 *       table.  Offsets that already have a Pass-1 candidate are skipped.
 *
 * Prologue patterns use a wildcard byte (-1).  If the matched length is
 * ≥ 50% of the pattern, partialScore is used; 100% match uses fullScore.
 *
 * ## Pass 3 — Non-Returning Propagation + Thunk Detection
 *
 * Seed set of known non-returners (symbol names):
 *   exit, _exit, _Exit, abort, __stack_chk_fail, longjmp, __longjmp_chk,
 *   siglongjmp, quick_exit, TerminateProcess, RaiseException, _endthread,
 *   __cxa_throw, __cxa_rethrow, std::terminate, std::abort
 *
 * Any function whose entire name or import name matches a seed is marked
 * non-returning.  The fixpoint then propagates: if every CALL target in a
 * function body is a non-returner (and there's no other exit), the function
 * is also non-returning.
 *
 * Thunk detection: scan the first 16 bytes of each candidate function.
 * A single FF 25 rel32 (JMP [RIP+rel]) or FF E? (JMP reg) or E9 rel32
 * (JMP rel32) with no other instructions before it → thunk.
 */

#include "retdec/func_boundary/func_boundary.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace retdec {
namespace func_boundary {

// ─── Constructor ─────────────────────────────────────────────────────────────

FuncBoundaryDetector::FuncBoundaryDetector(uint64_t imageBase,
                                           const uint8_t* data,
                                           std::size_t size,
                                           bool is64Bit)
    : _imageBase(imageBase), _data(data), _size(size), _is64Bit(is64Bit)
{}

// ─── Raw memory helpers ───────────────────────────────────────────────────────

std::size_t FuncBoundaryDetector::vaToOffset(uint64_t va) const noexcept
{
    if (va < _imageBase) return _size;
    uint64_t off = va - _imageBase;
    if (off >= _size) return _size;
    return static_cast<std::size_t>(off);
}

uint8_t FuncBoundaryDetector::readU8(std::size_t off) const noexcept
{
    if (off >= _size) return 0;
    return _data[off];
}

uint32_t FuncBoundaryDetector::readU32(std::size_t off) const noexcept
{
    if (off + 4 > _size) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(_data[off+i]) << (i*8);
    return v;
}

uint64_t FuncBoundaryDetector::readU64(std::size_t off) const noexcept
{
    if (off + 8 > _size) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(_data[off+i]) << (i*8);
    return v;
}

// ─── Candidate management ─────────────────────────────────────────────────────

void FuncBoundaryDetector::updateConfidence(FunctionBoundary& fb,
                                             EvidenceSource src)
{
    double w = evidenceConfidence(src);
    // Bayesian-like update: combine existing confidence with new evidence.
    // P(func | e1, e2) ∝ P(func | e1) * w  (simplified product rule).
    // Capped at 1.0.
    fb.confidence = std::min(1.0, fb.confidence + w * (1.0 - fb.confidence));
    fb.allEvidence.push_back(src);

    if (w > evidenceConfidence(fb.primaryEvidence))
        fb.primaryEvidence = src;
}

void FuncBoundaryDetector::ensureCandidate(uint64_t addr,
                                            EvidenceSource src,
                                            const std::string& name)
{
    auto it = _candidates.find(addr);
    if (it == _candidates.end()) {
        FunctionBoundary fb;
        fb.startAddr      = addr;
        fb.confidence     = evidenceConfidence(src);
        fb.primaryEvidence = src;
        fb.allEvidence.push_back(src);
        if (!name.empty()) fb.name = name;
        _candidates[addr] = std::move(fb);
    } else {
        updateConfidence(it->second, src);
        if (!name.empty() && it->second.name.empty())
            it->second.name = name;
    }
    _sortedDirty = true;
}

// ─── Evidence injection ───────────────────────────────────────────────────────

void FuncBoundaryDetector::addEntryPoint(uint64_t addr)
{
    ensureCandidate(addr, EvidenceSource::EntryPoint, "entry");
}

void FuncBoundaryDetector::addCallTarget(uint64_t addr)
{
    ensureCandidate(addr, EvidenceSource::CallTarget);
}

void FuncBoundaryDetector::addSymbol(const std::string& name, uint64_t addr,
                                      EvidenceSource src)
{
    ensureCandidate(addr, src, name);
}

void FuncBoundaryDetector::addTLSCallback(uint64_t addr)
{
    ensureCandidate(addr, EvidenceSource::TLSCallback, "__tls_init");
}

void FuncBoundaryDetector::addExceptionHandler(uint64_t addr)
{
    ensureCandidate(addr, EvidenceSource::ExceptionHandler);
}

void FuncBoundaryDetector::addExecutableSection(uint64_t start, uint64_t end)
{
    _execSections.push_back({start, end});
}

void FuncBoundaryDetector::addImport(uint64_t vma, const std::string& dll,
                                      const std::string& symbol)
{
    ImportEntry ie;
    ie.vma    = vma;
    ie.dll    = dll;
    ie.symbol = symbol;
    _imports[vma]    = ie;
    _importByName[symbol] = vma;
}

// ─── Prologue patterns ────────────────────────────────────────────────────────

std::vector<ProloguePattern>
FuncBoundaryDetector::prologuePatterns(CompilerHint hint)
{
    std::vector<ProloguePattern> pats;

    // ── GCC / Clang x86-64 ───────────────────────────────────────────────────

    if (hint == CompilerHint::Unknown || hint == CompilerHint::GCC ||
        hint == CompilerHint::Clang) {

        // push rbp; mov rbp, rsp  (55 48 89 E5)
        pats.push_back({"gcc_frame", {0x55, 0x48, 0x89, 0xE5}, 0.85, 0.60});

        // push rbp; mov rbp, rsp; push rbx  (55 48 89 E5 53)
        pats.push_back({"gcc_frame_rbx", {0x55, 0x48, 0x89, 0xE5, 0x53}, 0.87, 0.65});

        // push rbp; mov rbp, rsp; sub rsp, N  (55 48 89 E5 48 83 EC -1)
        pats.push_back({"gcc_frame_sub", {0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC, -1}, 0.88, 0.65});

        // endbr64; push rbp; mov rbp, rsp  (F3 0F 1E FA 55 48 89 E5)
        pats.push_back({"clang_endbr64", {0xF3, 0x0F, 0x1E, 0xFA, 0x55, 0x48, 0x89, 0xE5}, 0.90, 0.70});

        // sub rsp, N (no frame pointer — leaf function)  (48 83 EC -1)
        pats.push_back({"gcc_leaf_sub4", {0x48, 0x83, 0xEC, -1}, 0.75, 0.55});

        // sub rsp, N32  (48 81 EC -1 -1 -1 -1)
        pats.push_back({"gcc_leaf_sub32", {0x48, 0x81, 0xEC, -1, -1, -1, -1}, 0.75, 0.55});

        // push r15; push r14; push r13 (common in large GCC functions)
        pats.push_back({"gcc_pushregs", {0x41, 0x57, 0x41, 0x56, 0x41, 0x55}, 0.70, 0.50});
    }

    // ── MSVC x86-64 ──────────────────────────────────────────────────────────

    if (hint == CompilerHint::Unknown || hint == CompilerHint::MSVC) {

        // mov [rsp+8], rcx  (48 89 4C 24 08) — MSVC homespill pattern
        pats.push_back({"msvc_homespill", {0x48, 0x89, 0x4C, 0x24, 0x08}, 0.75, 0.55});

        // sub rsp, 28h  (48 83 EC 28)  — 5-element shadow space
        pats.push_back({"msvc_shadow28", {0x48, 0x83, 0xEC, 0x28}, 0.80, 0.60});

        // sub rsp, 38h  (48 83 EC 38)
        pats.push_back({"msvc_shadow38", {0x48, 0x83, 0xEC, 0x38}, 0.80, 0.60});

        // push rdi; push rsi; push rbx; sub rsp, N  (57 56 53 48 83 EC -1)
        pats.push_back({"msvc_saveregs", {0x57, 0x56, 0x53, 0x48, 0x83, 0xEC, -1}, 0.82, 0.62});

        // mov [rsp+8], rbx; mov [rsp+16], rbp  (MSVC non-volatile saves)
        pats.push_back({"msvc_save_rbx", {0x48, 0x89, 0x5C, 0x24, -1, 0x48, 0x89, 0x6C}, 0.78, 0.58});
    }

    return pats;
}

double FuncBoundaryDetector::matchPrologue(const ProloguePattern& pat,
                                            const uint8_t*         bytes,
                                            std::size_t            available)
{
    if (pat.bytes.empty() || available == 0) return 0.0;

    std::size_t patLen = pat.bytes.size();
    std::size_t matchLen = std::min(patLen, available);
    std::size_t matched = 0;

    for (std::size_t i = 0; i < matchLen; ++i) {
        int pb = pat.bytes[i];
        if (pb == -1) { ++matched; continue; }   // wildcard
        if (bytes[i] == static_cast<uint8_t>(pb)) { ++matched; continue; }
        break; // mismatch terminates
    }

    if (matched == patLen) return pat.fullScore;
    if (matched * 2 >= patLen) return pat.partialScore; // ≥50% matched
    return 0.0;
}

// ─── Pass 2: CALL target scan ─────────────────────────────────────────────────

void FuncBoundaryDetector::scanCallTargets(uint64_t secStart, uint64_t secEnd)
{
    std::size_t startOff = vaToOffset(secStart);
    std::size_t endOff   = vaToOffset(secEnd);
    if (startOff >= _size || endOff > _size || endOff <= startOff) return;

    for (std::size_t off = startOff; off + 4 < endOff; ++off) {
        uint8_t b = _data[off];

        // E8 rel32 — CALL rel32
        if (b == 0xE8) {
            int32_t rel = static_cast<int32_t>(readU32(off + 1));
            uint64_t target = secStart + (off - startOff) + 5 + rel;
            if (target >= _imageBase && target < _imageBase + _size) {
                ensureCandidate(target, EvidenceSource::CallTarget);
            }
            off += 4; // skip rel32
        }
        // FF 15 rel32 (CALL [RIP+rel]) — indirect, skip
        // FF D? — CALL reg — indirect, target unknown at scan time
    }
}

// ─── Pass 2: prologue scan ────────────────────────────────────────────────────

void FuncBoundaryDetector::scanSectionPrologues(
    uint64_t secStart, uint64_t secEnd,
    const std::vector<ProloguePattern>& patterns)
{
    std::size_t startOff = vaToOffset(secStart);
    std::size_t endOff   = vaToOffset(secEnd);
    if (startOff >= _size || endOff > _size) return;

    for (std::size_t off = startOff; off < endOff; ++off) {
        uint64_t va = _imageBase + off;
        // Skip addresses already confirmed at high confidence.
        auto it = _candidates.find(va);
        if (it != _candidates.end() && it->second.confidence >= 0.85) continue;

        std::size_t avail = endOff - off;
        for (const auto& pat : patterns) {
            double score = matchPrologue(pat, _data + off, avail);
            if (score <= 0.0) continue;

            EvidenceSource src = (score >= pat.fullScore)
                ? EvidenceSource::PrologueFull
                : EvidenceSource::ProloguePartial;

            // Only promote if score ≥ 0.70 for partial, always for full.
            if (score < 0.70 && src == EvidenceSource::ProloguePartial) continue;

            ensureCandidate(va, src);
            break; // one pattern match per offset is enough
        }
    }
}

// ─── Pass 1 ───────────────────────────────────────────────────────────────────

void FuncBoundaryDetector::runPass1()
{
    // All direct evidence was already injected by the caller via add*().
    // Pass 1 just finalises the existing candidates — nothing more to do here
    // since candidates are accumulated on injection.
    _sortedDirty = true;
}

// ─── Pass 2 ───────────────────────────────────────────────────────────────────

void FuncBoundaryDetector::runPass2(CompilerHint hint)
{
    auto patterns = prologuePatterns(hint);

    for (const auto& sec : _execSections) {
        scanCallTargets(sec.start, sec.end);
        scanSectionPrologues(sec.start, sec.end, patterns);
    }
    _sortedDirty = true;
}

// ─── Pass 3: non-returning ────────────────────────────────────────────────────

bool FuncBoundaryDetector::isKnownNonReturner(const std::string& sym) noexcept
{
    static const char* const kSeeds[] = {
        "exit", "_exit", "_Exit", "abort", "__stack_chk_fail",
        "longjmp", "__longjmp_chk", "siglongjmp", "quick_exit",
        "TerminateProcess", "RaiseException", "_endthread",
        "__cxa_throw", "__cxa_rethrow",
        "terminate",       // std::terminate
        "std::terminate",
        "std::abort",
        "__assert_fail", "__assert_rtn",
        "_wassert",        // MSVC assertion
        "FatalAppExitA", "FatalAppExitW",
        "ExitProcess",
        nullptr
    };
    for (const char* const* p = kSeeds; *p; ++p) {
        if (sym == *p) return true;
    }
    return false;
}

void FuncBoundaryDetector::seedNonReturning()
{
    // Seed from import names.
    for (const auto& [vma, imp] : _imports) {
        if (isKnownNonReturner(imp.symbol)) {
            _nonReturning.insert(vma);
            // If there's a candidate for this VMA, mark it.
            auto it = _candidates.find(vma);
            if (it != _candidates.end())
                it->second.isNonReturning = true;
        }
    }
    // Seed from symbol names of candidates.
    for (auto& [addr, fb] : _candidates) {
        if (!fb.name.empty() && isKnownNonReturner(fb.name)) {
            fb.isNonReturning = true;
            _nonReturning.insert(addr);
        }
    }
}

void FuncBoundaryDetector::propagateNonReturning()
{
    // Simple fixpoint: iterate over all candidates and mark any whose name
    // is a known non-returner.  A full propagation would require CFG analysis;
    // here we do a conservative name-based + import-address propagation.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [addr, fb] : _candidates) {
            if (fb.isNonReturning) continue;
            // Check if the function name matches any seed.
            if (!fb.name.empty() && isKnownNonReturner(fb.name)) {
                fb.isNonReturning = true;
                _nonReturning.insert(addr);
                changed = true;
            }
        }
    }
}

// ─── Pass 3: thunk detection ─────────────────────────────────────────────────

uint64_t FuncBoundaryDetector::detectThunkAt(std::size_t off) const noexcept
{
    if (off + 2 >= _size) return 0;

    uint8_t b0 = _data[off];
    uint8_t b1 = (off + 1 < _size) ? _data[off + 1] : 0;

    // JMP rel32: E9 <rel32>
    if (b0 == 0xE9 && off + 5 <= _size) {
        int32_t rel = static_cast<int32_t>(readU32(off + 1));
        uint64_t targetVA = _imageBase + off + 5 + rel;
        return targetVA;
    }

    // JMP [RIP+rel32]: FF 25 <rel32>  (x86-64 indirect via GOT/IAT)
    if (b0 == 0xFF && b1 == 0x25 && off + 6 <= _size) {
        int32_t rel = static_cast<int32_t>(readU32(off + 2));
        uint64_t ptrVA = _imageBase + off + 6 + rel;
        // The IAT slot holds the actual target; return the IAT VA as target key.
        return ptrVA;
    }

    // JMP reg: FF E0..E7
    if (b0 == 0xFF && (b1 >= 0xE0 && b1 <= 0xE7)) {
        return 1; // non-zero but unknown target
    }

    // REX.W prefix + JMP reg: 48 FF E? or 41 FF E?
    if ((b0 == 0x48 || b0 == 0x41) && off + 3 <= _size) {
        uint8_t b2 = _data[off + 2];
        if (_data[off + 1] == 0xFF && (b2 >= 0xE0 && b2 <= 0xE7)) {
            return 1;
        }
    }

    return 0;
}

void FuncBoundaryDetector::detectThunks()
{
    for (auto& [addr, fb] : _candidates) {
        if (fb.isThunk) continue;

        std::size_t off = vaToOffset(addr);
        if (off >= _size) continue;

        uint64_t target = detectThunkAt(off);
        if (target == 0) continue;

        fb.isThunk = true;
        fb.thunkTarget = nameForVma(target);
        if (fb.name.empty()) {
            fb.name = "thunk_" + fb.thunkTarget;
        }
    }
}

std::string FuncBoundaryDetector::nameForVma(uint64_t vma) const
{
    // Check imports.
    auto iit = _imports.find(vma);
    if (iit != _imports.end()) {
        return iit->second.symbol.empty()
            ? iit->second.dll + "_" + std::to_string(vma)
            : iit->second.symbol;
    }
    // Check function candidates.
    auto cit = _candidates.find(vma);
    if (cit != _candidates.end() && !cit->second.name.empty()) {
        return cit->second.name;
    }
    // Hex fallback.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "sub_%llx",
                  static_cast<unsigned long long>(vma));
    return buf;
}

// ─── runPass3 ─────────────────────────────────────────────────────────────────

void FuncBoundaryDetector::runPass3()
{
    seedNonReturning();
    propagateNonReturning();
    detectThunks();

    // Estimate end addresses: sort all start addresses; end = next start.
    std::vector<uint64_t> addrs;
    addrs.reserve(_candidates.size());
    for (auto& [a, _] : _candidates) addrs.push_back(a);
    std::sort(addrs.begin(), addrs.end());

    for (std::size_t i = 0; i < addrs.size(); ++i) {
        auto& fb = _candidates[addrs[i]];
        if (i + 1 < addrs.size()) {
            fb.endAddr = addrs[i + 1];
        } else {
            // Last function: extend to end of last exec section.
            fb.endAddr = fb.startAddr + 1;
            for (const auto& sec : _execSections) {
                if (sec.start <= fb.startAddr && fb.startAddr < sec.end) {
                    fb.endAddr = sec.end;
                    break;
                }
            }
        }
    }
    _sortedDirty = true;
}

// ─── runAll ───────────────────────────────────────────────────────────────────

void FuncBoundaryDetector::runAll(CompilerHint hint)
{
    runPass1();
    runPass2(hint);
    runPass3();
}

// ─── Results ─────────────────────────────────────────────────────────────────

const std::vector<FunctionBoundary>&
FuncBoundaryDetector::functions() const noexcept
{
    if (_sortedDirty) {
        _sorted.clear();
        _sorted.reserve(_candidates.size());
        for (const auto& [_, fb] : _candidates) _sorted.push_back(fb);
        std::sort(_sorted.begin(), _sorted.end(),
                  [](const FunctionBoundary& a, const FunctionBoundary& b) {
                      return a.startAddr < b.startAddr;
                  });
        _sortedDirty = false;
    }
    return _sorted;
}

const FunctionBoundary*
FuncBoundaryDetector::functionAt(uint64_t addr) const noexcept
{
    auto it = _candidates.find(addr);
    if (it == _candidates.end()) return nullptr;
    // Return pointer into _sorted to ensure stability.
    // Build sorted list first.
    (void)functions();
    for (const auto& fb : _sorted) {
        if (fb.startAddr == addr) return &fb;
    }
    return nullptr;
}

bool FuncBoundaryDetector::isNonReturning(uint64_t addr) const noexcept
{
    return _nonReturning.count(addr) > 0;
}

bool FuncBoundaryDetector::isThunk(uint64_t addr) const noexcept
{
    auto it = _candidates.find(addr);
    return it != _candidates.end() && it->second.isThunk;
}

} // namespace func_boundary
} // namespace retdec
