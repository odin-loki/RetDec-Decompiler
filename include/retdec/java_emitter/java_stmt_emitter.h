/**
 * @file include/retdec/java_emitter/java_stmt_emitter.h
 * @brief Java statement emitter — walks BcCFG and emits Java statements.
 *
 * ## CFG → Statement mapping
 *
 * After JVM reconstruction, the BcCFG contains variable-based instructions.
 * The statement emitter performs a second-pass structured code generation:
 *
 * 1. **Block linearisation** — walk the CFG in dominance order, emitting one
 *    structured statement per group of logically related blocks.
 *
 * 2. **Control-flow structure recognition** — identify:
 *    - if / else  (two-successor block with single join)
 *    - while loop (back-edge to a test block)
 *    - for loop   (init block, test block, increment block)
 *    - do-while   (body block with back-edge, test at end)
 *    - switch / switch-expression (Java 14+)
 *    - try/catch/finally  (from BcExceptionHandler table)
 *    - try-with-resources (AutoCloseable + .close() in finally)
 *    - enhanced for-each  (ForEachPattern from PatternLifter)
 *    - synchronized block (monitorenter/monitorexit pair)
 *
 * 3. **Statement emission** — each recognised structure maps to idiomatic Java.
 *
 * ## Indentation
 *
 * The emitter uses a `CodeWriter` for indented output.  Indentation is 4
 * spaces by default (configurable).
 */

#ifndef RETDEC_JAVA_EMITTER_JAVA_STMT_EMITTER_H
#define RETDEC_JAVA_EMITTER_JAVA_STMT_EMITTER_H

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_module.h"
#include "retdec/java_emitter/java_expr_emitter.h"
#include "retdec/jvm_reconstruct/jvm_reconstruct.h"
#include "retdec/jvm_reconstruct/pattern_lift.h"

#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace java_emitter {

// ─── Code writer ─────────────────────────────────────────────────────────────

/**
 * @brief Indented code output buffer.
 *
 * Wraps a std::ostringstream with indent tracking.
 */
class CodeWriter {
public:
    explicit CodeWriter(int indentWidth = 4);

    void indent();
    void dedent();
    int  currentIndent() const { return level_; }

    void writeLine(const std::string& line);
    void writeLine();   ///< blank line
    void write(const std::string& s);   ///< no newline

    std::string str() const;

private:
    std::ostringstream buf_;
    int level_      = 0;
    int indentWidth_;
    std::string indentStr_;

    void updateIndentStr();
};

// ─── Statement emitter options ────────────────────────────────────────────────

struct StmtEmitOptions {
    bool emitSwitchExpressions = true;  ///< Java 14+ switch expressions
    bool emitPatternInstanceof  = true; ///< Java 16+ instanceof pattern vars
    bool emitTextBlocks         = false;///< Java 13+ text blocks (conservative)
    bool emitRecords            = true; ///< Java 16+ record classes
    bool emitEnhancedFor        = true; ///< emit for-each where detected
    bool emitTryWithResources   = true; ///< detect AutoCloseable pattern
    bool emitSynchronized       = true; ///< detect monitor enter/exit
    int  javaVersion            = 17;   ///< Target Java version for feature gates
};

// ─── Statement emitter ───────────────────────────────────────────────────────

/**
 * @brief Emits Java statements from a BcCFG (after reconstruction).
 *
 * The emitter walks the CFG using a structural analysis approach:
 * it identifies SESE (single-entry single-exit) regions and maps them
 * to the highest-level Java construct applicable.
 */
class JavaStmtEmitter {
public:
    JavaStmtEmitter(const BcMethod& method,
                    const ReconstructResult& recon,
                    const JavaTypePrinter& tyPrinter,
                    const StmtEmitOptions& opts = StmtEmitOptions{});

    /**
     * @brief Emit the full method body.
     *
     * @return Complete method body including surrounding braces.
     */
    std::string emitBody();

private:
    const BcMethod&          method_;
    const ReconstructResult& recon_;
    const JavaTypePrinter&   tyPrinter_;
    StmtEmitOptions          opts_;
    ExprContext              exprCtx_;
    JavaExprEmitter          exprEmit_;
    CodeWriter               out_;

    // Visited tracking (block ids already emitted).
    std::set<uint32_t>                        visited_;
    // Pattern lookup: block id → for-each pattern
    std::unordered_map<uint32_t, size_t>      forEachByHeader_;
    // Pattern lookup: block id → string concat pattern
    std::unordered_map<uint32_t, size_t>      stringConcatByBlock_;
    // Pattern lookup: block id → lambda pattern
    std::unordered_map<uint32_t, size_t>      lambdaByBlock_;

    // Build pattern lookup maps from recon_.patterns.
    void buildPatternMaps();

    // ── Control flow structure emission ──────────────────────────────────────

    // Main dispatch: emit starting from block `id`, up to stop block.
    void emitFrom(uint32_t id, uint32_t stopBlock = UINT32_MAX);

    // Emit a single basic block's instructions as statements.
    void emitBlock(uint32_t blockId);

    // Emit one instruction as a Java statement.
    // Returns false if the instruction is purely an expression (handled inline).
    bool emitInstrAsStmt(const BcInstruction& insn,
                          std::vector<ExprNode>& exprStack);

    // ── Structural recognition ────────────────────────────────────────────────

    // Detect and emit if/else starting at a two-successor block.
    bool tryEmitIfElse(uint32_t blockId);

    // Detect and emit a while loop (test-at-top).
    bool tryEmitWhile(uint32_t blockId);

    // Detect and emit a do-while loop (test-at-bottom).
    bool tryEmitDoWhile(uint32_t blockId);

    // Detect and emit a for loop (init + test + incr + body).
    bool tryEmitFor(uint32_t blockId);

    // Detect and emit a for-each from a ForEachPattern.
    bool tryEmitForEach(uint32_t blockId);

    // Detect and emit a switch statement / switch expression.
    bool tryEmitSwitch(uint32_t blockId);

    // Detect and emit a try/catch/finally.
    bool tryEmitTryCatch(uint32_t blockId);

    // Detect and emit a synchronized block.
    bool tryEmitSynchronized(uint32_t blockId);

    // ── Expression stack helpers ──────────────────────────────────────────────

    // Emit the expression stack flushing any residual statements.
    void flushStack(std::vector<ExprNode>& stack);

    // Build a condition expression string from a conditional branch instruction.
    std::string buildCondition(const BcInstruction& branchInsn,
                                std::vector<ExprNode>& stack);

    // ── Helpers ───────────────────────────────────────────────────────────────

    // Find the immediate post-dominator of a block (join point).
    uint32_t findJoin(uint32_t blockId) const;

    // Return true if `blockId` has a back-edge (is a loop header).
    bool isLoopHeader(uint32_t blockId) const;

    // Return the loop exit block for a loop with header `blockId`.
    uint32_t loopExit(uint32_t blockId) const;

    // Return the back-edge source block for a loop header.
    uint32_t backEdgeSource(uint32_t blockId) const;

    const BcCFG& cfg() const { return method_.cfg; }
};

} // namespace java_emitter
} // namespace retdec

#endif // RETDEC_JAVA_EMITTER_JAVA_STMT_EMITTER_H
