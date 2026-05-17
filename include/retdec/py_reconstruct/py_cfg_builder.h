/**
 * @file include/retdec/py_reconstruct/py_cfg_builder.h
 * @brief Control-flow structuring: flat basic blocks → structured Python AST.
 *
 * Python bytecode already encodes rich control flow. This pass converts
 * the flat sequence of basic blocks (with conditional/unconditional jumps
 * and exception regions) into structured Python statements:
 *
 *   if/elif/else   ← conditional jumps (POP_JUMP_IF_*)
 *   while          ← backward jump to a condition block
 *   for            ← FOR_ITER with JUMP_BACKWARD
 *   try/except     ← exception table regions (3.11+) or SETUP_EXCEPT (3.10-)
 *   try/finally    ← exception table with isFinally
 *   with           ← BEFORE_WITH / SETUP_WITH
 *   match          ← MATCH_* opcodes (3.10+)
 *
 * Output: one PyModule::body filled with structured PyStmt nodes.
 */

#ifndef RETDEC_PY_RECONSTRUCT_PY_CFG_BUILDER_H
#define RETDEC_PY_RECONSTRUCT_PY_CFG_BUILDER_H

#include "retdec/py_reconstruct/py_ast_nodes.h"
#include "retdec/py_reconstruct/py_stack_sim.h"
#include "retdec/pyc_parser/py_code_object.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace py_reconstruct {

// ─── PyCfgBuilder ─────────────────────────────────────────────────────────────

/**
 * @brief Converts a Python code object into a structured PyModule.
 *
 * Wraps PyStackSimulator for expression reconstruction, then applies
 * control-flow structuring heuristics to produce readable output.
 */
class PyCfgBuilder {
public:
    struct Options {
        bool emitComments   = true;   ///< Emit # comments for unrecoverable regions
        bool structureLoops = true;   ///< Detect while/for loops
        bool structureTry   = true;   ///< Detect try/except/finally
        bool structureWith  = true;   ///< Detect with statements
        bool structureComp  = true;   ///< Detect comprehensions (list/set/dict/gen)
        bool detectFStr     = true;   ///< Detect f-string patterns
        bool detectAugAssign= true;   ///< Detect augmented assignment
        bool detectWalrus   = true;   ///< Detect := (3.8+)
        bool detectMatch    = true;   ///< Detect match/case (3.10+)
    };
    static Options defaultOptions() noexcept { return {}; }

    explicit PyCfgBuilder(Options opts = defaultOptions());

    /**
     * @brief Build a flat list of Python statements from one code object.
     *
     * These are then wrapped in a FunctionDef or placed at module level
     * by the top-level reconstructor.
     *
     * @param code  The PyCodeObject to reconstruct.
     * @return Structured statement list.
     */
    StmtList build(const PyCodeObject& code);

    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    Options opts_;
    std::vector<std::string> warnings_;

    // ── High-level structure detection ────────────────────────────────────────

    /// Detect and replace while/for loop patterns.
    StmtList detectLoops(StmtList stmts, const PyCodeObject& code);

    /// Detect and replace if/elif/else patterns.
    StmtList detectConditionals(StmtList stmts);

    /// Detect try/except/finally regions from exception table.
    StmtList detectTry(StmtList stmts, const PyCodeObject& code);

    /// Detect with statement patterns.
    StmtList detectWith(StmtList stmts);

    /// Detect comprehension patterns (MAKE_FUNCTION + call → [x for x in ...]).
    StmtList detectComps(StmtList stmts, const PyCodeObject& code);

    // ── Helpers ───────────────────────────────────────────────────────────────

    void warn(const std::string& msg) { warnings_.push_back(msg); }

    /// Wrap a function-level code object into a FunctionDef statement.
    PyStmtPtr wrapFunctionDef(const PyCodeObject& code,
                               StmtList body,
                               ExprList decorators = ExprList{}) const;

    /// Wrap a class-level code object into a ClassDef statement.
    PyStmtPtr wrapClassDef(const PyCodeObject& code,
                            StmtList body) const;
};

// ─── PyReconstructor ─────────────────────────────────────────────────────────

/**
 * @brief Top-level Python reconstruction pipeline.
 *
 * Orchestrates PyCfgBuilder recursively for nested code objects (lambdas,
 * nested functions, classes, comprehensions) and produces a complete PyModule.
 */
class PyReconstructor {
public:
    struct Options {
        PyCfgBuilder::Options cfgOpts;
        bool skipCompGenerated = true;    ///< Skip <genexpr>/<listcomp>/etc.
        bool addDocstrings     = true;    ///< First string const → docstring

        static Options defaultOptions() noexcept { return {}; }
    };

    explicit PyReconstructor(Options opts = Options::defaultOptions());

    /**
     * @brief Reconstruct a complete Python module from its root code object.
     */
    PyModule reconstruct(const PyCodeObject& root, int pyMajor, int pyMinor,
                          const std::string& filename = "");

    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    Options opts_;
    std::vector<std::string> warnings_;

    void warn(const std::string& msg) { warnings_.push_back(msg); }

    /// Recursively build statement body for a code object.
    StmtList buildBody(const PyCodeObject& code, PyCfgBuilder& builder);

    /// Make FunctionDef/AsyncFunctionDef from a code object's metadata.
    PyStmtPtr makeFuncDef(const PyCodeObject& code, StmtList body,
                           ExprList decorators = ExprList{}) const;

    /// Derive PyArguments from code object fields.
    PyArguments makeArguments(const PyCodeObject& code) const;
};

} // namespace py_reconstruct
} // namespace retdec

#endif // RETDEC_PY_RECONSTRUCT_PY_CFG_BUILDER_H
