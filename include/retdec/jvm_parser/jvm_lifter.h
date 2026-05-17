/**
 * @file include/retdec/jvm_parser/jvm_lifter.h
 * @brief JVM bytecode lifter: raw Code attribute → BcCFG.
 *
 * ## Coverage
 *
 * All JVM opcodes from Java 1.0 through Java 21 are handled:
 *   - Constants: nop, aconst_null, iconst_*, lconst_*, fconst_*, dconst_*,
 *     bipush, sipush, ldc, ldc_w, ldc2_w.
 *   - Loads/stores: iload/aload/lload/fload/dload (and _0.._3 variants),
 *     istore/astore/lstore/fstore/dstore, iaload/aastore families, wide prefix.
 *   - Arithmetic: iadd, ladd, fadd, dadd, isub, …, irem, …, ineg, …,
 *     ishl, lshl, ishr, lushr, iand, ior, ixor, …, iinc.
 *   - Conversions: i2l, i2f, i2d, l2i, l2f, l2d, f2i, f2l, f2d, d2i, d2l,
 *     d2f, i2b, i2c, i2s.
 *   - Comparisons: lcmp, fcmpl, fcmpg, dcmpl, dcmpg.
 *   - Control: ifeq/ifne/iflt/ifge/ifgt/ifle, if_icmp*, if_acmp*,
 *     goto, goto_w, jsr/ret (deprecated), tableswitch, lookupswitch,
 *     ireturn/lreturn/freturn/dreturn/areturn/return.
 *   - References: getstatic, putstatic, getfield, putfield, invokevirtual,
 *     invokespecial, invokestatic, invokeinterface, invokedynamic,
 *     new, newarray, anewarray, arraylength, athrow,
 *     checkcast, instanceof, monitorenter, monitorexit.
 *   - Extended: wide, multianewarray, ifnull, ifnonnull, goto_w, jsr_w.
 *
 * ## Algorithm
 *
 * 1. **Leader detection**: Build the set of leader PCs (offset 0, branch
 *    targets, fall-throughs after branches, exception handler entries).
 * 2. **Block construction**: For each leader PC, collect instructions until
 *    the next leader.
 * 3. **Edge wiring**: Add successor edges based on terminators; wire up
 *    all exception table entries as EH edges.
 * 4. **Stack state annotation**: Propagate typed stack state through the CFG.
 *    Stack frames from the StackMapTable (if present) seed the propagation.
 */

#ifndef RETDEC_JVM_PARSER_JVM_LIFTER_H
#define RETDEC_JVM_PARSER_JVM_LIFTER_H

#include "retdec/jvm_parser/jvm_attr.h"
#include "retdec/jvm_parser/jvm_signature.h"
#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_module.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace jvm_parser {

// ─── Lifter options ───────────────────────────────────────────────────────────

struct LiftOptions {
    bool annotateStack   = true;   ///< Compute and store entry/exit stack types
    bool mapLineNumbers  = true;   ///< Annotate instructions with source lines
    bool resolveLdc      = true;   ///< Resolve ldc → PushInt/PushString/etc.
};
static LiftOptions defaultLiftOptions() noexcept { return {}; }

// ─── Lift result ─────────────────────────────────────────────────────────────

struct LiftResult {
    bool        ok    = false;
    std::string error;
    bc_module::BcCFG cfg;
};

// ─── JvmLifter ────────────────────────────────────────────────────────────────

/**
 * @brief Lifts one JVM method's Code attribute to a BcCFG.
 */
class JvmLifter {
public:
    explicit JvmLifter(const ConstPool& pool, LiftOptions opts = defaultLiftOptions());

    /**
     * Lift a parsed CodeAttr for a method with the given descriptor.
     *
     * @param code       Parsed Code attribute.
     * @param methodDesc Method descriptor string "(ILjava/lang/String;)V".
     */
    LiftResult lift(const CodeAttr& code, const std::string& methodDesc);

private:
    const ConstPool& pool_;
    LiftOptions      opts_;

    // Pass 1: find leader PCs.
    std::vector<uint32_t> findLeaders(const std::vector<uint8_t>& bc,
                                      const std::vector<ExceptionEntry>& eh);

    // Pass 2: build blocks + fill instructions.
    void buildBlocks(bc_module::BcCFG& cfg,
                     const std::vector<uint8_t>& bc,
                     const std::vector<uint32_t>& leaders,
                     const ConstPool& pool);

    // Pass 3: wire EH edges.
    void wireExceptions(bc_module::BcCFG& cfg,
                        const std::vector<ExceptionEntry>& eh,
                        const std::vector<uint32_t>& leaders,
                        const ConstPool& pool);

    // Pass 4: annotate stack types (forward propagation).
    void annotateStack(bc_module::BcCFG& cfg,
                       const std::string& methodDesc);

    // Per-opcode instruction builder.
    bc_module::BcInstruction decodeInstr(const std::vector<uint8_t>& bc,
                                          uint32_t& pc, uint32_t nextPc,
                                          uint32_t instrId,
                                          const std::vector<uint32_t>& leaders);

    // Map from bytecode PC → block id.
    std::unordered_map<uint32_t, uint32_t> pcToBlock_;

    // Line-number map (PC → source line).
    std::unordered_map<uint32_t, int32_t> lineMap_;
};

} // namespace jvm_parser
} // namespace retdec

#endif // RETDEC_JVM_PARSER_JVM_LIFTER_H
