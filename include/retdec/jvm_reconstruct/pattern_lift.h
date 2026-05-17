/**
 * @file include/retdec/jvm_reconstruct/pattern_lift.h
 * @brief High-level pattern reconstruction for Java idioms.
 *
 * Detects and rewrites three key patterns in JVM bytecode:
 *
 * ## 1. String Concatenation (Java 9+ invokedynamic)
 *
 * Java 9+ uses invokedynamic + StringConcatFactory.makeConcatWithConstants
 * instead of StringBuilder chains.  We detect:
 *
 *   invokedynamic #n "makeConcatWithConstants" "(...)Ljava/lang/String;"
 *
 * and replace it with a synthetic `BcOpcode::Add` tree on string operands.
 * We also handle the classic Java 8 StringBuilder pattern:
 *
 *   new StringBuilder → append* → toString → assign
 *
 * ## 2. Lambda / Method Reference (invokedynamic + LambdaMetafactory)
 *
 * LambdaMetafactory.metafactory produces a functional interface instance.
 * We detect invokedynamic calls to the metafactory and emit a synthetic
 * `BcOpcode::InvokeDynamic` instruction that carries:
 *   - The functional interface type.
 *   - The implementation method reference.
 *   - The capture variables.
 *
 * The Java emitter then renders this as a lambda expression or method
 * reference depending on the implementation method's structure.
 *
 * ## 3. Enhanced For-Loop (Iterator pattern)
 *
 * Classic pattern (for (T x : collection)):
 *
 *   invokeinterface Collection.iterator()
 *   while (iterator.hasNext()) {
 *       T x = (T) iterator.next();
 *       … body …
 *   }
 *
 * Array for-loop pattern (for (T x : array)):
 *
 *   int len = array.length;
 *   for (int i = 0; i < len; i++) {
 *       T x = array[i];
 *       … body …
 *   }
 *
 * Both are annotated with a `ForEachPattern` marker attached to the loop
 * header block.  The Java emitter uses the marker to emit `for (T x : …)`
 * instead of the verbose iterator/index form.
 */

#ifndef RETDEC_JVM_RECONSTRUCT_PATTERN_LIFT_H
#define RETDEC_JVM_RECONSTRUCT_PATTERN_LIFT_H

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_module.h"
#include "retdec/jvm_reconstruct/local_rebuild.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace jvm_reconstruct {

// ─── String concatenation ────────────────────────────────────────────────────

/**
 * @brief Marks a group of instructions as a string concatenation expression.
 *
 * After detection, the group is collapsed to a single synthetic instruction
 * with opcode `BcOpcode::Add` carrying all string parts as operands.
 */
struct StringConcatPattern {
    uint32_t blockId;
    uint32_t firstInstrId;  ///< First instruction of the pattern
    uint32_t lastInstrId;   ///< Last instruction (toString or invdyn)
    std::vector<uint32_t> partSlotIds;  ///< Slot IDs of operand parts
    std::string recipe;     ///< invokedynamic recipe string (may be empty)
};

// ─── Lambda / method reference ────────────────────────────────────────────────

enum class LambdaKind {
    Lambda,          ///< () -> body (no captures of existing method)
    MethodReference, ///< SomeClass::method or obj::method
};

struct LambdaPattern {
    uint32_t    blockId;
    uint32_t    instrId;     ///< The invokedynamic instruction id
    LambdaKind  kind;
    std::string functionalInterface; ///< "java/util/function/Predicate"
    BcMethodRef implMethod;          ///< The implementation method
    std::vector<uint32_t> captureSlots; ///< Captured variable slots
};

// ─── Enhanced for-loop ────────────────────────────────────────────────────────

enum class ForEachKind {
    Iterator,  ///< for (T x : collection) — uses Iterator.hasNext / next
    Array,     ///< for (T x : array)      — uses array length + index
};

struct ForEachPattern {
    ForEachKind kind;
    uint32_t    loopHeaderBlock;    ///< Block containing hasNext / length check
    uint32_t    bodyBlock;          ///< First block of loop body
    uint32_t    exitBlock;          ///< First block after loop
    uint32_t    elementSlot;        ///< Slot ID of the loop variable (x)
    BcType      elementType;        ///< Type of x
    uint32_t    collectionSlot;     ///< Slot ID of the collection / array
    uint32_t    iteratorSlot;       ///< For Iterator kind: iterator local slot
    uint32_t    indexSlot;          ///< For Array kind: index local slot
    uint32_t    lengthSlot;         ///< For Array kind: length local slot
};

// ─── Pattern lift result ─────────────────────────────────────────────────────

struct PatternLiftResult {
    std::vector<StringConcatPattern> stringConcats;
    std::vector<LambdaPattern>       lambdas;
    std::vector<ForEachPattern>      forEachLoops;
};

// ─── Pattern lifter ──────────────────────────────────────────────────────────

/**
 * @brief Detects high-level Java patterns in a BcCFG.
 *
 * Called after local variable reconstruction.  The BcCFG is not modified;
 * the patterns are returned as annotations for the Java emitter.
 */
class PatternLifter {
public:
    PatternLiftResult lift(const BcCFG& cfg,
                           const BcMethod& method,
                           const StackSimResult& simResult,
                           const LocalRebuildResult& locals);

private:
    // String concatenation detection.
    void detectStringConcat(const BcCFG& cfg,
                             const StackSimResult& simResult,
                             PatternLiftResult& result) const;

    // Lambda / method reference detection.
    void detectLambda(const BcCFG& cfg,
                      const BcMethod& method,
                      const StackSimResult& simResult,
                      PatternLiftResult& result) const;

    // Enhanced for-loop detection.
    void detectForEach(const BcCFG& cfg,
                       const StackSimResult& simResult,
                       const LocalRebuildResult& locals,
                       PatternLiftResult& result) const;

    // Check if an invokedynamic instruction is a string concat.
    static bool isStringConcatInvokeDynamic(const BcInstruction& insn);

    // Check if an invokedynamic instruction is a lambda.
    static bool isLambdaInvokeDynamic(const BcInstruction& insn,
                                       LambdaPattern& out);

    // Detect the Iterator.hasNext() pattern in a loop header block.
    static std::optional<ForEachPattern>
        detectIteratorLoop(const BcCFG& cfg,
                           uint32_t loopHeaderBlock,
                           const StackSimResult& simResult);

    // Detect the array-index loop pattern.
    static std::optional<ForEachPattern>
        detectArrayLoop(const BcCFG& cfg,
                        uint32_t loopHeaderBlock,
                        const StackSimResult& simResult);
};

} // namespace jvm_reconstruct
} // namespace retdec

#endif // RETDEC_JVM_RECONSTRUCT_PATTERN_LIFT_H
