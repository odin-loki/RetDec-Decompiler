/**
 * @file src/compiler_detect/compiler_fingerprinter.cpp
 * @brief Compiler + version + optimisation-level fingerprinter.
 *
 * ## Feature extraction
 *
 * Each function's prologue (first 64 bytes) is decoded with a small
 * hand-rolled x86-64 pattern matcher — no external decoder dependency.
 * The patterns are kept minimal and conservative to avoid false matches.
 *
 * ## Decision tree
 *
 * Nodes are evaluated in priority order:
 *   1. Hard evidence: MSVC Rich header, EH personality, mangling style.
 *   2. Soft evidence: shadow-space ratio, frame-pointer ratio, tail-call ratio.
 *   3. Optimisation level estimated from tail-call ratio, frame-pointer ratio,
 *      and memset inline threshold.
 *   4. Version range narrowed from EH personality + opt features.
 */

#include "retdec/compiler_detect/compiler_fingerprinter.h"
#include "retdec/compiler_detect/compiler_profile.h"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace retdec {
namespace compiler_detect {

// ─── toString helpers ─────────────────────────────────────────────────────────

std::string toString(CompilerFamily f)
{
    switch (f) {
    case CompilerFamily::GCC:   return "GCC";
    case CompilerFamily::Clang: return "Clang";
    case CompilerFamily::MSVC:  return "MSVC";
    case CompilerFamily::ICC:   return "ICC";
    case CompilerFamily::TCC:   return "TCC";
    default:                    return "Unknown";
    }
}

std::string toString(OptLevel o)
{
    switch (o) {
    case OptLevel::O0: return "O0";
    case OptLevel::O1: return "O1";
    case OptLevel::O2: return "O2";
    case OptLevel::O3: return "O3";
    case OptLevel::Os: return "Os";
    case OptLevel::Oz: return "Oz";
    default:           return "Unknown";
    }
}

std::string toString(CppABI a)
{
    switch (a) {
    case CppABI::Itanium: return "Itanium";
    case CppABI::MSVC:    return "MSVC";
    case CppABI::Rust:    return "Rust";
    default:              return "Unknown";
    }
}

std::string toString(CallingConvention cc)
{
    switch (cc) {
    case CallingConvention::SystemV_AMD64: return "SystemV_AMD64";
    case CallingConvention::Win64:         return "Win64";
    case CallingConvention::Cdecl_x86:    return "cdecl";
    case CallingConvention::Stdcall_x86:  return "stdcall";
    case CallingConvention::Fastcall_x86: return "fastcall";
    case CallingConvention::Thiscall_x86: return "thiscall";
    default:                              return "Unknown";
    }
}

std::string VersionRange::toString() const
{
    if (!isKnown()) return "unknown";
    if (major_lo == major_hi) return std::to_string(major_lo);
    return std::to_string(major_lo) + "-" + std::to_string(major_hi);
}

std::string CompilerProfile::summary() const
{
    std::ostringstream oss;
    oss << retdec::compiler_detect::toString(family);
    if (version.isKnown()) oss << " " << version.toString();
    oss << " -" << retdec::compiler_detect::toString(optLevel);
    oss << " (" << retdec::compiler_detect::toString(callConv) << ")";
    if (confidence > 0.0f) oss << " conf=" << static_cast<int>(confidence * 100) << "%";
    return oss.str();
}

// ─── Feed methods ─────────────────────────────────────────────────────────────

void CompilerFingerprinter::addFunction(const uint8_t* bytes, uint32_t size)
{
    if (_funcs.size() >= kMaxFunctions) return;
    if (!bytes || size == 0) return;
    _funcs.push_back(analysePrologue(bytes, size));
}

void CompilerFingerprinter::setRichHeader(uint32_t vsMajorVersion)
{
    _richHeaderVSMajor = vsMajorVersion;
}

void CompilerFingerprinter::setEHPersonality(char kind)
{
    _ehPersonality = kind;
}

void CompilerFingerprinter::setManglingStyle(bool hasItanium, bool hasMSVC)
{
    _itaniumMangling = hasItanium;
    _msvcMangling    = hasMSVC;
}

void CompilerFingerprinter::setImports(const std::vector<std::string>& importNames)
{
    for (const auto& name : importNames) {
        if (name == "__stack_chk_fail" || name == "__stack_chk_guard") {
            _stackCanary = true;
        }
        if (name == "__gxx_personality_v0" || name == "__gcc_personality_v0") {
            if (_ehPersonality == 0) _ehPersonality = 'g';
        }
        if (name == "__CxxFrameHandler3" || name == "__C_specific_handler") {
            if (_ehPersonality == 0) _ehPersonality = 'm';
        }
    }
}

// ─── Prologue analysis ────────────────────────────────────────────────────────

/**
 * Analyse the first min(size, 64) bytes of a function.
 *
 * Patterns detected:
 *
 *  Frame pointer (RBP used):
 *    PUSH RBP     = 55
 *    MOV  RBP,RSP = 48 89 E5
 *
 *  Frame pointer with SUB RSP (MSVC-style):
 *    PUSH RBP  55
 *    PUSH RBX / R12-R15 (extended saves)
 *    MOV  RBP,RSP  48 89 E5
 *
 *  Stack alignment:
 *    AND  RSP,-16  = 48 83 E4 F0
 *    AND  RSP,-32  = 48 83 E4 E0
 *
 *  Shadow space (MSVC Win64 calling convention):
 *    SUB RSP,32  = 48 83 EC 20
 *    SUB RSP,40  = 48 83 EC 28  (with alignment)
 *    SUB RSP,48  = 48 83 EC 30  (with saved registers)
 *    Any SUB RSP,N with N >= 32 is counted as shadow-space if followed
 *    by the four MSVC characteristic moves.
 *
 *  Tail call: function exits via JMP (E9/EB/FF /4) not RET (C3/CB).
 *
 *  Inline memset: look for REP STOSB / STOSQ pattern with counter load,
 *    or loop of MOV [ptr+offset], imm / MOV [ptr+offset*8], imm sequences.
 */
CompilerFingerprinter::FuncStats
CompilerFingerprinter::analysePrologue(const uint8_t* b, uint32_t sz)
{
    FuncStats st;
    if (sz == 0) return st;

    const uint32_t limit = std::min(sz, 128u);

    // ── Frame pointer check ─────────────────────────────────────────────────
    // Pattern: 55  (PUSH RBP) then 48 89 E5 (MOV RBP,RSP) within first 8 bytes
    // also handles: 55 41 5? ... 48 89 E5 (callee-save pushes in between)
    bool sawPushRBP = false;
    for (uint32_t i = 0; i < std::min(limit, 16u); ++i) {
        if (b[i] == 0x55) { sawPushRBP = true; continue; }
        if (sawPushRBP && i + 2 < limit &&
            b[i] == 0x48 && b[i+1] == 0x89 && b[i+2] == 0xE5) {
            st.hasFramePointer = true;
            break;
        }
    }

    // ── Stack alignment (AND RSP, -16 / -32) ───────────────────────────────
    for (uint32_t i = 0; i + 3 < limit; ++i) {
        if (b[i] == 0x48 && b[i+1] == 0x83 && b[i+2] == 0xE4 &&
            (b[i+3] == 0xF0 || b[i+3] == 0xE0 || b[i+3] == 0xC0)) {
            st.hasStackAlign = true;
            break;
        }
    }

    // ── Shadow space (SUB RSP, N ≥ 32) ─────────────────────────────────────
    // 48 83 EC xx  — SUB RSP, imm8
    // 48 81 EC xx xx xx xx — SUB RSP, imm32
    for (uint32_t i = 0; i + 3 < limit; ++i) {
        if (b[i] == 0x48 && b[i+1] == 0x83 && b[i+2] == 0xEC) {
            if (b[i+3] >= 0x20) { st.hasShadowSpace = true; break; }
        }
        if (b[i] == 0x48 && b[i+1] == 0x81 && b[i+2] == 0xEC && i + 6 < limit) {
            uint32_t imm = (uint32_t)b[i+3] | ((uint32_t)b[i+4] << 8) |
                           ((uint32_t)b[i+5] << 16) | ((uint32_t)b[i+6] << 24);
            if (imm >= 0x20) { st.hasShadowSpace = true; break; }
        }
    }

    // ── Inline memset detection ─────────────────────────────────────────────
    // Look for REP STOSB (F3 AA) or REP STOSQ (F3 48 AB).
    // Also: series of consecutive MOV [rdi+offset], 0 instructions.
    for (uint32_t i = 0; i + 1 < limit; ++i) {
        if (b[i] == 0xF3) {
            if (i + 1 < limit && b[i+1] == 0xAA) {
                // REP STOSB — count depends on RCX; mark as at least 1
                st.memsetBytes = std::max(st.memsetBytes, 1u);
            }
            if (i + 2 < limit && b[i+1] == 0x48 && b[i+2] == 0xAB) {
                // REP STOSQ
                st.memsetBytes = std::max(st.memsetBytes, 8u);
            }
        }
        // XMM movaps zero-pattern: 66 0F 6F C0 (MOVDQA xmm0,xmm0) for 16B
        if (i + 3 < limit &&
            b[i]==0x66 && b[i+1]==0x0F && b[i+2]==0x7F) {
            st.memsetBytes = std::max(st.memsetBytes, 16u);
        }
    }
    // Series of MOV [rdi+N*8], 0 (REX 48, opcode C7, ModRM 47, disp8, imm32=0)
    {
        uint32_t zeroStores = 0;
        for (uint32_t i = 0; i + 6 < limit; ++i) {
            if (b[i] == 0x48 && b[i+1] == 0xC7 && (b[i+2] & 0xC0) == 0x40) {
                uint32_t immOff = i + 4;
                if (immOff + 3 < limit &&
                    b[immOff]==0 && b[immOff+1]==0 && b[immOff+2]==0 && b[immOff+3]==0) {
                    ++zeroStores;
                }
            }
        }
        if (zeroStores > 0)
            st.memsetBytes = std::max(st.memsetBytes, zeroStores * 8u);
    }

    // ── Tail call detection ─────────────────────────────────────────────────
    // Scan the last 16 bytes for a JMP (not a RET).
    const uint32_t tail_start = sz > 16u ? sz - 16u : 0u;
    bool seenRet = false, seenJmp = false;
    for (uint32_t i = tail_start; i < sz; ++i) {
        uint8_t op = b[i];
        if (op == 0xC3 || op == 0xCB) { seenRet = true; }
        if (op == 0xE9 || op == 0xEB) { seenJmp = true; } // JMP rel
        if (op == 0xFF && i + 1 < sz && ((b[i+1] >> 3) & 7) == 4) {
            seenJmp = true; // JMP r/m
        }
    }
    // Only count as tail call if we see JMP with no RET after it
    st.hasTailCall = seenJmp && !seenRet;

    return st;
}

// ─── Feature extraction ───────────────────────────────────────────────────────

FeatureVector CompilerFingerprinter::extractFeatures() const
{
    FeatureVector fv;
    fv.richHeaderVSMajor = _richHeaderVSMajor;
    fv.ehPersonality     = _ehPersonality;
    fv.itaniumMangling   = _itaniumMangling;
    fv.msvcMangling      = _msvcMangling;
    fv.stackCanary       = _stackCanary;
    fv.functionsAnalysed = static_cast<uint32_t>(_funcs.size());

    if (_funcs.empty()) return fv;

    const float n = static_cast<float>(_funcs.size());

    uint32_t fpCount = 0, tcCount = 0, ssCount = 0, saCount = 0;
    uint32_t maxMemset = 0;
    for (const auto& f : _funcs) {
        if (f.hasFramePointer) ++fpCount;
        if (f.hasTailCall)     ++tcCount;
        if (f.hasShadowSpace)  ++ssCount;
        if (f.hasStackAlign)   ++saCount;
        maxMemset = std::max(maxMemset, f.memsetBytes);
    }

    fv.framePointerRatio    = static_cast<float>(fpCount) / n;
    fv.tailCallRatio        = static_cast<float>(tcCount) / n;
    fv.shadowSpaceAlloc     = (static_cast<float>(ssCount) / n) > 0.20f;
    fv.stackAlign16         = (static_cast<float>(saCount) / n) > 0.05f;
    fv.memsetInlineThreshold = maxMemset;

    return fv;
}

// ─── Version range helpers ────────────────────────────────────────────────────

VersionRange CompilerFingerprinter::gccVersionFromFeatures(const FeatureVector& fv)
{
    // GCC 9-10: higher frame-pointer ratio at O0/O1, less aggressive tail calls.
    // GCC 11+:  -fstack-protector-strong default, stack canary more common.
    // GCC 12+:  more aggressive inlining of memset (XMM patterns).
    if (fv.stackCanary && fv.memsetInlineThreshold >= 16) {
        return {11, 13};
    }
    if (fv.memsetInlineThreshold >= 16) {
        return {10, 13};
    }
    if (fv.stackCanary) {
        return {9, 12};
    }
    return {9, 13};
}

VersionRange CompilerFingerprinter::clangVersionFromFeatures(const FeatureVector& fv)
{
    // Clang 12: introduced -fstack-clash-protection by default on some distros.
    // Clang 15: more aggressive tail-call opts.
    // Clang 16: COFF improvements (Windows).
    if (fv.tailCallRatio > 0.15f && fv.stackCanary) {
        return {15, 17};
    }
    if (fv.tailCallRatio > 0.10f) {
        return {14, 17};
    }
    if (fv.stackCanary) {
        return {12, 16};
    }
    return {12, 17};
}

VersionRange CompilerFingerprinter::msvcVersionFromFeatures(const FeatureVector& fv)
{
    // Rich header provides VS major version directly.
    if (fv.richHeaderVSMajor == 19) {
        // MSVC 19.xx = VS 2015-2022 — narrow by shadow + EH.
        // VS 2022 = toolset 1930+.
        return {2019, 2022};
    }
    if (fv.richHeaderVSMajor == 16 || fv.richHeaderVSMajor == 17) {
        return {2019, 2019};
    }
    // Without Rich header, use shadow space + EH.
    if (fv.shadowSpaceAlloc && fv.ehPersonality == 'm') {
        return {2019, 2022};
    }
    return {2015, 2022};
}

OptLevel CompilerFingerprinter::optLevelFromFeatures(const FeatureVector& fv,
                                                      CompilerFamily family)
{
    // Heuristics based on observed codegen behaviour:
    //
    //  O0: high frame-pointer ratio (>0.90), low tail-call ratio (<0.02),
    //      large memset threshold unlikely (compiler just calls memset).
    //  O1: moderate frame-pointer (0.60-0.90), rare tail calls.
    //  O2: frame-pointer ratio drops (~0.30-0.60 for GCC/Clang),
    //      some tail calls, moderate memset inlining.
    //  O3: low frame-pointer (<0.30), higher tail-call ratio,
    //      aggressive memset inlining (XMM/YMM patterns, threshold ≥ 32).
    //  Os: similar to O2 but very small inline memset threshold (≤ 8).

    if (fv.functionsAnalysed == 0) return OptLevel::Unknown;

    // MSVC uses /O0=/Od, /O1, /O2 — shadow space always present, frame ptr
    // less reliable. Use tail-call + memset as primary signals.
    if (family == CompilerFamily::MSVC) {
        if (fv.tailCallRatio < 0.02f && fv.memsetInlineThreshold == 0)
            return OptLevel::O0;
        // Only classify as O1 if there's no inline memset (memset inlining
        // indicates O2 or higher even with zero tail calls).
        if (fv.tailCallRatio < 0.05f && fv.memsetInlineThreshold < 16u)
            return OptLevel::O1;
        if (fv.memsetInlineThreshold >= 32)
            return OptLevel::O2;
        return OptLevel::O2;
    }

    // GCC / Clang
    const float fp = fv.framePointerRatio;
    const float tc = fv.tailCallRatio;
    const uint32_t ms = fv.memsetInlineThreshold;

    if (fp > 0.88f && tc < 0.02f && ms < 16u)
        return OptLevel::O0;

    if (fp > 0.60f && tc < 0.05f)
        return OptLevel::O1;

    if (ms >= 32u || (tc > 0.12f && fp < 0.20f))
        return OptLevel::O3;

    if (ms <= 8u && tc < 0.08f && fp >= 0.40f)
        return OptLevel::Os;

    return OptLevel::O2;
}

// ─── Decision tree ────────────────────────────────────────────────────────────

CompilerProfile CompilerFingerprinter::decisionTree(const FeatureVector& fv)
{
    CompilerProfile p;
    p.features = fv;

    // ── Node 1: MSVC (hard evidence) ────────────────────────────────────────
    // Rich header is MSVC-only, EH personality __CxxFrameHandler3 is MSVC-only.
    bool hardMSVC = (fv.richHeaderVSMajor > 0)
                 || (fv.ehPersonality == 'm')
                 || (fv.msvcMangling && !fv.itaniumMangling);
    if (hardMSVC) {
        p.family   = CompilerFamily::MSVC;
        p.cppABI   = CppABI::MSVC;
        p.callConv = CallingConvention::Win64;
        p.version  = msvcVersionFromFeatures(fv);
        p.optLevel = optLevelFromFeatures(fv, CompilerFamily::MSVC);

        float conf = 0.70f;
        if (fv.richHeaderVSMajor > 0) conf += 0.20f;
        if (fv.ehPersonality == 'm')  conf += 0.05f;
        if (fv.shadowSpaceAlloc)      conf += 0.05f;
        p.confidence = std::min(conf, 1.0f);
        return p;
    }

    // ── Node 2: distinguish GCC vs Clang (soft evidence) ────────────────────
    // Key discriminators:
    //   - Clang generates more tail calls at O2+ than GCC.
    //   - GCC tends to keep frame pointers longer (higher ratio at same opt).
    //   - Stack canary is common on both; not discriminating.
    //   - Clang does AND RSP,-16 more consistently.
    //   - __gxx_personality_v0 is present for BOTH (not discriminating).

    bool likelyGNU = fv.itaniumMangling || fv.ehPersonality == 'g' || fv.stackCanary;

    if (fv.shadowSpaceAlloc && !fv.itaniumMangling) {
        // Shadow space on Windows; could be MSVC without Rich header or
        // GCC/Clang cross-compiled for Windows (MinGW).
        // If itanium mangling is absent, lean toward MSVC.
        p.family   = CompilerFamily::MSVC;
        p.cppABI   = CppABI::MSVC;
        p.callConv = CallingConvention::Win64;
        p.version  = msvcVersionFromFeatures(fv);
        p.optLevel = optLevelFromFeatures(fv, CompilerFamily::MSVC);
        p.confidence = 0.55f;
        return p;
    }

    if (fv.shadowSpaceAlloc && fv.itaniumMangling) {
        // MinGW-w64 or Clang on Windows: Itanium ABI with Win64 calling convention.
        // Discriminate GCC vs Clang by tail-call ratio.
        if (fv.tailCallRatio > 0.08f) {
            p.family   = CompilerFamily::Clang;
            p.version  = clangVersionFromFeatures(fv);
            p.confidence = 0.65f;
        } else {
            p.family   = CompilerFamily::GCC;
            p.version  = gccVersionFromFeatures(fv);
            p.confidence = 0.60f;
        }
        p.cppABI   = CppABI::Itanium;
        p.callConv = CallingConvention::Win64;
        p.optLevel = optLevelFromFeatures(fv, p.family);
        return p;
    }

    // ── Node 3: SystemV AMD64 (Linux/macOS) ──────────────────────────────────
    // No shadow space — either GCC, Clang, or ICC.

    if (!likelyGNU && fv.functionsAnalysed < 5) {
        // Too little evidence.
        p.family     = CompilerFamily::Unknown;
        p.confidence = 0.0f;
        return p;
    }

    // Clang tends to:
    //   - lower frame-pointer ratio at O2 (more aggressive omission)
    //   - higher tail-call ratio (more aggressive tail-call optimisation)
    //   - consistent AND RSP,-16 in every function (stackAlign16)
    float clangScore = 0.0f;
    float gccScore   = 0.0f;

    if (fv.tailCallRatio > 0.12f)         clangScore += 2.0f;
    else if (fv.tailCallRatio > 0.05f)    clangScore += 1.0f;
    else                                   gccScore   += 1.0f;

    if (fv.framePointerRatio < 0.25f)     clangScore += 1.5f;
    else if (fv.framePointerRatio > 0.70f) gccScore  += 1.5f;
    else                                   gccScore   += 0.5f;

    if (fv.stackAlign16)                   clangScore += 0.5f;
    if (fv.memsetInlineThreshold >= 32)    clangScore += 0.5f;

    if (clangScore > gccScore) {
        p.family     = CompilerFamily::Clang;
        p.version    = clangVersionFromFeatures(fv);
        float diff   = clangScore - gccScore;
        p.confidence = std::min(0.50f + diff * 0.08f, 0.92f);
    } else {
        p.family     = CompilerFamily::GCC;
        p.version    = gccVersionFromFeatures(fv);
        float diff   = gccScore - clangScore;
        p.confidence = std::min(0.50f + diff * 0.08f, 0.92f);
    }

    p.cppABI   = CppABI::Itanium;
    p.callConv = CallingConvention::SystemV_AMD64;
    p.optLevel = optLevelFromFeatures(fv, p.family);
    return p;
}

// ─── Public classify ─────────────────────────────────────────────────────────

CompilerProfile CompilerFingerprinter::classify() const
{
    FeatureVector fv = extractFeatures();
    return decisionTree(fv);
}

} // namespace compiler_detect
} // namespace retdec
