/**
 * @file include/retdec/py_reconstruct/py_stack_sim.h
 * @brief Stack simulation for CPython bytecode → expression reconstruction.
 *
 * CPython uses an evaluation stack; this simulator tracks PyExpr trees on
 * the virtual stack as instructions execute, reconstructing compound
 * expressions (calls, attribute accesses, subscripts, binary ops, etc.)
 * in-place rather than introducing temporary variables.
 */

#ifndef RETDEC_PY_RECONSTRUCT_PY_STACK_SIM_H
#define RETDEC_PY_RECONSTRUCT_PY_STACK_SIM_H

#include "retdec/py_reconstruct/py_ast_nodes.h"
#include "retdec/pyc_parser/py_code_object.h"
#include "retdec/pyc_parser/pyc_magic.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace py_reconstruct {

using namespace pyc_parser;

// ─── StackEntry ──────────────────────────────────────────────────────────────

/**
 * @brief A single slot on the simulated evaluation stack.
 */
struct StackEntry {
    PyExprPtr expr;              ///< Expression producing this value
    bool      isName  = false;  ///< True → was a LOAD_NAME/LOAD_FAST
    std::string name;           ///< Original name if isName
};

using Stack = std::vector<StackEntry>;

// ─── BlockResult ─────────────────────────────────────────────────────────────

/**
 * @brief Result of simulating one basic block.
 *
 * Contains the emitted statements plus the stack state at the end of
 * the block (for use by successor blocks).
 */
struct BlockResult {
    StmtList stmts;
    Stack    exitStack;
    bool     terminates = false;  ///< Returns / raises
};

// ─── PyStackSimulator ────────────────────────────────────────────────────────

/**
 * @brief Simulates the CPython evaluation stack for one code object.
 *
 * Processes each basic block in topological order and emits Python
 * statement AST nodes from the bytecode instruction stream.
 */
class PyStackSimulator {
public:
    struct Options {
        bool inlineSimpleNames  = true;   ///< Inline single-use names
        bool reconstructAugAssign = true; ///< x = x+y  →  x += y
        bool reconstructFStr    = true;   ///< FORMAT_VALUE+BUILD_STRING → f""
    };
    static Options defaultOptions() noexcept { return {}; }

    PyStackSimulator(const PyCodeObject& code, Options opts = defaultOptions());

	/// The code object is held by reference for the simulator's whole life, so
	/// a temporary would be gone before simulate() runs.  Rejecting it here
	/// turns a use-after-free into a compile error.
	PyStackSimulator(PyCodeObject&& code, Options opts = defaultOptions()) = delete;

	/**
     * @brief Run the simulation and return all statements.
     * @return flat statement list for the entire function body.
     */
    StmtList simulate();

    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    const PyCodeObject& code_;
    Options opts_;
    mutable std::vector<std::string> warnings_;

    // ── Block processing ─────────────────────────────────────────────────────

    /// Decode all instructions into (opcode_name, arg) pairs with offsets.
    struct RawInstr {
        std::string name;
        int32_t     arg;
        uint32_t    offset;
    };
    std::vector<RawInstr> decode() const;

    /// Find basic block leaders (sorted unique offsets).
    std::vector<uint32_t> findLeaders(const std::vector<RawInstr>& instrs) const;

    /// Simulate one block from entryStack; return stmts + exit stack.
    BlockResult simulateBlock(
        const std::vector<RawInstr>& instrs,
        size_t blockStart, size_t blockEnd,
        Stack entryStack);

    // ── Instruction handlers ──────────────────────────────────────────────────

    /// Apply one instruction to stack+stmts; return false on error.
    bool applyInstr(const RawInstr& instr, Stack& stack, StmtList& stmts);

    // LOAD instructions → push expression
    PyExprPtr buildLoad(const RawInstr& instr) const;

    // BINARY_OP / inplace → BinOp expr
    PyExprPtr buildBinOp(const std::string& opName,
                          PyExprPtr lhs, PyExprPtr rhs) const;

    // BUILD_* → collection literals
    PyExprPtr buildCollection(const std::string& opName,
                               int32_t count, Stack& stack) const;

    // COMPARE_OP
    PyExprPtr buildCompare(int32_t cmpIdx,
                            PyExprPtr lhs, PyExprPtr rhs) const;

    // ── Helpers ───────────────────────────────────────────────────────────────

    PyExprPtr popExpr(Stack& stack) const;
    void      pushExpr(Stack& stack, PyExprPtr e) const;

	/// A .pyc carries its operand counts in the instruction stream and nothing
	/// in the file has to agree with them, so a four-byte count can name two
	/// billion operands a three-instruction body does not have.  Every loop
	/// that consumes or synthesises one entry per count goes through one of
	/// these, so a declared count costs at most what could plausibly exist.

	/// Once the stack is empty every further pop yields a placeholder that
	/// carries no information; a few of those are enough to show the block was
	/// truncated.  @a perItem is how many entries one count pops.
	int32_t boundedPopCount(int32_t declared, const Stack& stack, int32_t perItem = 1) const;

	/// A count that only pushes has no stack to run out of, so it gets a flat
	/// cap instead.
	int32_t boundedPushCount(int32_t declared) const;

	PyExprPtr constFromIdx(int32_t idx) const;
    std::string nameFromIdx(int32_t idx, const std::vector<std::string>& table) const;

    void warn(const std::string& msg) const { warnings_.push_back(msg); }
};

} // namespace py_reconstruct
} // namespace retdec

#endif // RETDEC_PY_RECONSTRUCT_PY_STACK_SIM_H
