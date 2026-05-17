/**
* @file src/llvmir2hll/llvm/llvm_intrinsic_converter_ext.cpp
* @brief Extension for LLVMIntrinsicConverter: 20+ missing intrinsic mappings.
* @copyright (c) 2024, MIT license
*
* Adds mappings not in the original convertIntrinsicFuncName():
*
*  Math:
*    llvm.ceil.*        → ceilf / ceil / ceill
*    llvm.round.*       → roundf / round / roundl
*    llvm.nearbyint.*   → nearbyintf / nearbyint / nearbyintl
*    llvm.trunc.*       → truncf / trunc / truncl
*    llvm.rint.*        → rintf / rint / rintl
*    llvm.minnum.*      → fminf / fmin / fminl
*    llvm.maxnum.*      → fmaxf / fmax / fmaxl
*    llvm.minimum.*     → fminf / fmin  (same mapping, IEEE 754-2008 minimum)
*    llvm.maximum.*     → fmaxf / fmax
*    llvm.exp2.*        → exp2f / exp2 / exp2l
*    llvm.log2.*        → log2f / log2 / log2l
*    llvm.log10.*       → log10f / log10 / log10l
*    llvm.powi.*        → powif / powi (GCC extension __builtin_powi)
*
*  Bit manipulation:
*    llvm.bswap.*       → __builtin_bswap16/32/64 (width-specific)
*    llvm.ctpop.*       → __builtin_popcount / __builtin_popcountl
*    llvm.ctlz.*        → __builtin_clz / __builtin_clzl
*    llvm.cttz.*        → __builtin_ctz / __builtin_ctzl
*    llvm.fshl.*        → __builtin_rotateleft32/64  (or equivalent)
*    llvm.fshr.*        → __builtin_rotateright32/64
*
*  Overflow intrinsics (strip the overflow variant, keep the op):
*    llvm.sadd.with.overflow.*  → emit __retdec_sadd_overflow (intrinsic placeholder)
*    llvm.uadd.with.overflow.*
*    llvm.ssub.with.overflow.*
*    llvm.usub.with.overflow.*
*    llvm.smul.with.overflow.*
*    llvm.umul.with.overflow.*
*    (These are renamed to descriptive names; actual IR handling is done by the
*     existing types propagator and instruction optimizer passes.)
*
*  Lifetime / debug annotations (pure metadata, strip as call-stmts):
*    llvm.lifetime.start / llvm.lifetime.end    → remove
*    llvm.dbg.declare / llvm.dbg.value          → remove
*    llvm.invariant.start / llvm.invariant.end  → remove
*    llvm.assume                                → remove
*    llvm.stacksave / llvm.stackrestore         → remove
*/

#include <string>
#include "retdec/utils/string.h"
#include "retdec/llvmir2hll/llvm/llvm_intrinsic_converter_ext.h"

using retdec::utils::startsWith;
using retdec::utils::endsWith;

namespace retdec {
namespace llvmir2hll {

/**
 * Returns true if @a funcName is an LLVM intrinsic that produces only
 * metadata / side-effect-free annotations and whose call-statement can be
 * removed entirely (it carries no information in decompiled C output).
 */
bool isStrippableLLVMIntrinsic(const std::string& funcName) {
    // Lifetime markers — not relevant in C.
    if (startsWith(funcName, "llvm.lifetime.start")) return true;
    if (startsWith(funcName, "llvm.lifetime.end"))   return true;
    // Debug annotations.
    if (startsWith(funcName, "llvm.dbg."))           return true;
    // Invariant markers.
    if (startsWith(funcName, "llvm.invariant.start")) return true;
    if (startsWith(funcName, "llvm.invariant.end"))   return true;
    // Assume hint.
    if (startsWith(funcName, "llvm.assume"))          return true;
    // Stack save/restore — these are artifacts of the alloca lowering pass;
    // they don't correspond to observable C-level behaviour.
    if (startsWith(funcName, "llvm.stacksave"))       return true;
    if (startsWith(funcName, "llvm.stackrestore"))    return true;
    // Experimental / pseudo-probe annotations.
    if (startsWith(funcName, "llvm.pseudoprobe"))     return true;
    if (startsWith(funcName, "llvm.experimental.noalias.scope")) return true;
    return false;
}

/**
 * Returns the width suffix embedded in an LLVM intrinsic name:
 *   llvm.bswap.i16  → 16
 *   llvm.bswap.i32  → 32
 *   llvm.bswap.i64  → 64
 * Returns 0 if not determinable.
 */
static unsigned extractBitWidth(const std::string& funcName) {
    if (endsWith(funcName, ".i8"))  return 8;
    if (endsWith(funcName, ".i16")) return 16;
    if (endsWith(funcName, ".i32")) return 32;
    if (endsWith(funcName, ".i64")) return 64;
    return 0;
}

/**
 * Returns the C name for an LLVM intrinsic that was not handled by the
 * original LLVMIntrinsicConverter::convertIntrinsicFuncName().
 *
 * Returns an empty string if the intrinsic is not recognised here.
 * Returns "<strip>" if the intrinsic should be stripped as a call-statement.
 */
std::string getExtendedIntrinsicName(const std::string& funcName) {
    // ── Math rounding ──────────────────────────────────────────────────────
    if (startsWith(funcName, "llvm.ceil.")) {
        if (endsWith(funcName, ".f32")) return "ceilf";
        if (endsWith(funcName, ".f64")) return "ceil";
        return "ceill";
    }
    if (startsWith(funcName, "llvm.round.")) {
        if (endsWith(funcName, ".f32")) return "roundf";
        if (endsWith(funcName, ".f64")) return "round";
        return "roundl";
    }
    if (startsWith(funcName, "llvm.nearbyint.")) {
        if (endsWith(funcName, ".f32")) return "nearbyintf";
        if (endsWith(funcName, ".f64")) return "nearbyint";
        return "nearbyintl";
    }
    if (startsWith(funcName, "llvm.trunc.")) {
        if (endsWith(funcName, ".f32")) return "truncf";
        if (endsWith(funcName, ".f64")) return "trunc";
        return "truncl";
    }
    if (startsWith(funcName, "llvm.rint.")) {
        if (endsWith(funcName, ".f32")) return "rintf";
        if (endsWith(funcName, ".f64")) return "rint";
        return "rintl";
    }
    if (startsWith(funcName, "llvm.round.even.")) {
        // C23 roundeven; fall back to nearbyint for older targets.
        if (endsWith(funcName, ".f32")) return "nearbyintf";
        if (endsWith(funcName, ".f64")) return "nearbyint";
        return "nearbyintl";
    }

    // ── FP min/max ─────────────────────────────────────────────────────────
    if (startsWith(funcName, "llvm.minnum.") ||
        startsWith(funcName, "llvm.minimum.")) {
        if (endsWith(funcName, ".f32")) return "fminf";
        if (endsWith(funcName, ".f64")) return "fmin";
        return "fminl";
    }
    if (startsWith(funcName, "llvm.maxnum.") ||
        startsWith(funcName, "llvm.maximum.")) {
        if (endsWith(funcName, ".f32")) return "fmaxf";
        if (endsWith(funcName, ".f64")) return "fmax";
        return "fmaxl";
    }

    // ── Math: exp2 / log2 / log10 / powi ──────────────────────────────────
    if (startsWith(funcName, "llvm.exp2.")) {
        if (endsWith(funcName, ".f32")) return "exp2f";
        if (endsWith(funcName, ".f64")) return "exp2";
        return "exp2l";
    }
    if (startsWith(funcName, "llvm.log2.")) {
        if (endsWith(funcName, ".f32")) return "log2f";
        if (endsWith(funcName, ".f64")) return "log2";
        return "log2l";
    }
    if (startsWith(funcName, "llvm.log10.")) {
        if (endsWith(funcName, ".f32")) return "log10f";
        if (endsWith(funcName, ".f64")) return "log10";
        return "log10l";
    }
    if (startsWith(funcName, "llvm.powi.")) {
        if (endsWith(funcName, ".f32")) return "__builtin_powif";
        if (endsWith(funcName, ".f64")) return "__builtin_powi";
        return "__builtin_powil";
    }

    // ── Bit manipulation ───────────────────────────────────────────────────
    if (startsWith(funcName, "llvm.bswap.")) {
        unsigned w = extractBitWidth(funcName);
        if (w == 16) return "__builtin_bswap16";
        if (w == 32) return "__builtin_bswap32";
        if (w == 64) return "__builtin_bswap64";
        return "__builtin_bswap32";  // default
    }
    if (startsWith(funcName, "llvm.ctpop.")) {
        unsigned w = extractBitWidth(funcName);
        if (w == 64) return "__builtin_popcountll";
        return "__builtin_popcount";
    }
    if (startsWith(funcName, "llvm.ctlz.")) {
        unsigned w = extractBitWidth(funcName);
        if (w == 64) return "__builtin_clzll";
        if (w == 16) return "__builtin_clz";   // no 16-bit variant in GCC
        return "__builtin_clz";
    }
    if (startsWith(funcName, "llvm.cttz.")) {
        unsigned w = extractBitWidth(funcName);
        if (w == 64) return "__builtin_ctzll";
        return "__builtin_ctz";
    }
    // Funnel shifts — rotate left/right.
    if (startsWith(funcName, "llvm.fshl.")) {
        unsigned w = extractBitWidth(funcName);
        if (w == 64) return "__builtin_rotateleft64";
        return "__builtin_rotateleft32";
    }
    if (startsWith(funcName, "llvm.fshr.")) {
        unsigned w = extractBitWidth(funcName);
        if (w == 64) return "__builtin_rotateright64";
        return "__builtin_rotateright32";
    }

    // ── Overflow intrinsics ────────────────────────────────────────────────
    // Rename to descriptive names. The struct result is handled by
    // the types_propagator pass.
    if (startsWith(funcName, "llvm.sadd.with.overflow.")) return "__retdec_sadd_ov";
    if (startsWith(funcName, "llvm.uadd.with.overflow.")) return "__retdec_uadd_ov";
    if (startsWith(funcName, "llvm.ssub.with.overflow.")) return "__retdec_ssub_ov";
    if (startsWith(funcName, "llvm.usub.with.overflow.")) return "__retdec_usub_ov";
    if (startsWith(funcName, "llvm.smul.with.overflow.")) return "__retdec_smul_ov";
    if (startsWith(funcName, "llvm.umul.with.overflow.")) return "__retdec_umul_ov";
    if (startsWith(funcName, "llvm.sadd.sat.")) return "__retdec_sadd_sat";
    if (startsWith(funcName, "llvm.uadd.sat.")) return "__retdec_uadd_sat";
    if (startsWith(funcName, "llvm.ssub.sat.")) return "__retdec_ssub_sat";
    if (startsWith(funcName, "llvm.usub.sat.")) return "__retdec_usub_sat";

    // ── Strippable metadata intrinsics ─────────────────────────────────────
    if (isStrippableLLVMIntrinsic(funcName)) return "<strip>";

    return {};   // Not handled here.
}

} // namespace llvmir2hll
} // namespace retdec
