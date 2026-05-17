/**
 * @file include/retdec/cil_reconstruct/cil_patterns.h
 * @brief CIL high-level pattern detection (Phases 4–10).
 *
 * ## Patterns detected
 *
 * ### Phase 4 — Property access
 *
 * CIL compilers lower property accesses to `get_Prop` / `set_Prop` callvirt
 * instructions.  We detect these and rewrite them as property access syntax:
 *
 *   ldloc.0 / callvirt get_Name  →  local.Name
 *   ldloc.0 / ldarg.1 / callvirt set_Name(value)  →  local.Name = arg1
 *
 * Similarly for static properties (ldsfld / call get_Prop).
 *
 * ### Phase 5 — Async/await state machine
 *
 * The C# compiler lowers `async`/`await` into a state machine class that
 * implements `IAsyncStateMachine`.  The state machine has:
 *   - An `int <>1__state` field (current await state, -1 = running)
 *   - A `MoveNext()` method with a big switch on the state field
 *   - `AsyncTaskMethodBuilder` / `AsyncValueTaskMethodBuilder` fields
 *   - `<>u__1`, `<>u__2`, ... fields for awaiter temporaries
 *
 * Detection:
 *   1. Identify classes named `<MethodName>d__N` with IAsyncStateMachine.
 *   2. Identify the state field and awaiter fields.
 *   3. Reconstruct the linear async method body with `await` expressions.
 *
 * ### Phase 6 — LINQ
 *
 * LINQ chains appear as chained callvirt instructions on IEnumerable<T>:
 *   `source.Where(x => ...).Select(x => ...).OrderBy(x => ...)`
 *
 * We detect:
 *   - `System.Linq.Enumerable.*` method calls
 *   - `System.Linq.Queryable.*` for LINQ-to-SQL
 *   - Lambda expressions (compiler-generated delegates via `ldftn` + `newobj Action<>`)
 *
 * ### Phase 7 — Iterator (yield)
 *
 * The C# compiler lowers `yield return` / `yield break` into a state machine
 * implementing `IEnumerator<T>`.  Detection is similar to async:
 *   1. Class named `<MethodName>d__N` with IEnumerator<T>.
 *   2. State field + `Current` property.
 *   3. `MoveNext()` switch reconstruction.
 *
 * ### Phase 8 — Unsafe / fixed
 *
 * - `fixed (T* p = array)` appears as a pinned local + array address ldloc/stloc.
 * - `stackalloc T[n]` → `localloc` instruction.
 * - Pointer arithmetic: add/sub on `NATIVE_INT` typed expressions.
 *
 * ### Phase 9 — Pattern matching
 *
 * C# 7+ pattern matching (`is T x`) compiles to:
 *   `isinst T` + brfalse + unbox.any T + stloc
 *
 * C# 9+ `when` guards appear as additional conditionals.
 *
 * ### Phase 10 — Switch expressions (C# 8+)
 *
 * C# 8 switch expressions compile to a pattern of gotos into a set of case
 * blocks.  We detect the canonical pattern and reconstruct:
 *   `x switch { pattern1 => expr1, pattern2 => expr2, _ => exprN }`
 */

#ifndef RETDEC_CIL_RECONSTRUCT_CIL_PATTERNS_H
#define RETDEC_CIL_RECONSTRUCT_CIL_PATTERNS_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/cil_reconstruct/cil_var_recovery.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace retdec {
namespace cil_reconstruct {

using namespace bc_module;

// ─── Property access detection ────────────────────────────────────────────────

/**
 * @brief Represents a detected property access or assignment pattern.
 */
struct PropertyPattern {
    enum Kind { Getter, Setter, EventAdd, EventRemove } kind;

    std::string ownerType;    ///< Class that declares the property
    std::string propertyName; ///< Name without get_/set_/add_/remove_ prefix
    BcType      propertyType;

    bool isStatic = false;

    // For setters: the value expression
    CilExprPtr valueExpr;
    // For getters: inserted as the top-of-stack expression
    CilExprPtr resultExpr;

    // Block/instruction coordinates of the callvirt that was detected
    uint32_t blockId = 0;
    uint32_t insnIdx = 0;
};

// ─── Lambda / delegate detection ──────────────────────────────────────────────

/**
 * @brief A detected lambda / anonymous delegate.
 *
 * C# lambdas compile to:
 *   - A compiler-generated method (often `<MethodName>b__N` or `<>c.<>9__N_M`)
 *   - `ldftn` / `ldvirtftn` + `newobj Func<>` / `Action<>`
 *
 * We record the target method name so the C# emitter can inline the lambda.
 */
struct LambdaInfo {
    std::string captureClass;      ///< Class capturing the closure variables
    std::string targetMethod;      ///< Compiler-generated target method
    std::string delegateType;      ///< Func<>, Action<>, Predicate<>, etc.
    std::vector<std::string> capturedVars; ///< Names of captured variables
    bool        isAnonymousMethod = false; ///< delegate { ... } vs arrow lambda
};

// ─── Async state machine descriptor ──────────────────────────────────────────

/**
 * @brief Describes a detected async state machine class.
 */
struct AsyncStateMachine {
    std::string smClassName;        ///< e.g. "<DoWorkAsync>d__5"
    std::string ownerMethodName;    ///< The original async method name
    std::string ownerClassName;
    BcType      returnType;         ///< Task / Task<T> / ValueTask<T>

    std::string stateField;         ///< "<>1__state"
    std::string builderField;       ///< "<>t__builder"
    std::vector<std::string> awaiterFields;   ///< "<>u__1", "<>u__2", ...
    std::vector<std::string> capturedLocals;  ///< User locals hoisted to fields

    /// Reconstructed async body (after state machine expansion)
    std::vector<CilStmt> asyncBody;

    int stateCount = 0;  ///< Number of `await` suspension points
};

// ─── Iterator state machine descriptor ───────────────────────────────────────

struct IteratorStateMachine {
    std::string smClassName;
    std::string ownerMethodName;
    std::string ownerClassName;
    BcType      yieldType;         ///< The T in IEnumerable<T>

    std::string stateField;
    std::string currentField;      ///< "<>2__current"
    std::vector<std::string> capturedLocals;

    std::vector<CilStmt> iteratorBody;  ///< Reconstructed with yield return/break
};

// ─── LINQ chain descriptor ────────────────────────────────────────────────────

struct LinqCall {
    std::string methodName;  ///< Where, Select, OrderBy, GroupBy, …
    CilExprPtr  source;      ///< The IEnumerable source
    std::vector<CilExprPtr> args; ///< Lambda args
};

struct LinqChain {
    std::vector<LinqCall> calls;
    bool isQueryable = false;  ///< LINQ-to-SQL / IQueryable
};

// ─── Pattern matching descriptor ─────────────────────────────────────────────

struct IsTypePattern {
    CilExprPtr  subject;
    BcType      testedType;
    std::string boundVarName; ///< e.g. `x` in `is Dog x`
    uint32_t    blockId  = 0;
    uint32_t    insnIdx  = 0;
};

// ─── Switch expression descriptor ────────────────────────────────────────────

struct SwitchExprArm {
    std::optional<CilExprPtr> pattern;  ///< null = default arm
    CilExprPtr                body;
};

struct SwitchExpr {
    CilExprPtr            subject;
    std::vector<SwitchExprArm> arms;
    BcType                resultType;
};

// ─── CilPatternDetector ───────────────────────────────────────────────────────

/**
 * @brief Detects high-level .NET patterns in a recovered method.
 *
 * Operates as a set of AST/IR rewrites over the CilStmt list.
 * Each pass is independent and idempotent.
 */
class CilPatternDetector {
public:
    struct Options {
        bool detectProperties    = true;
        bool detectAsync         = true;
        bool detectLinq          = true;
        bool detectIterator      = true;
        bool detectUnsafe        = true;
        bool detectPatternMatch  = true;
        bool detectSwitchExpr    = true;
        bool detectUsing         = true;
        bool detectLock          = true;
        bool detectForEach       = true;
    };
    static Options defaultOptions() noexcept { return {}; }

    explicit CilPatternDetector(const Options& opts = defaultOptions());

    /**
     * @brief Run all enabled pattern passes over a recovered method.
     *
     * Modifies `method.body` in-place, annotating or rewriting statements.
     * Sets the `isAsync`, `isIterator`, `hasLinq`, etc. flags on the method.
     */
    void detect(CilRecoveredMethod& method,
                const BcModule& module) const;

    // ── Individual passes ─────────────────────────────────────────────────

    void detectPropertyAccess(CilRecoveredMethod& method) const;
    void detectAsyncStateMachine(CilRecoveredMethod& method,
                                  const BcModule& module) const;
    void detectIteratorStateMachine(CilRecoveredMethod& method,
                                     const BcModule& module) const;
    void detectLinqChains(CilRecoveredMethod& method) const;
    void detectUnsafePatterns(CilRecoveredMethod& method) const;
    void detectIsPatterns(CilRecoveredMethod& method) const;
    void detectSwitchExpressions(CilRecoveredMethod& method) const;
    void detectUsingStatements(CilRecoveredMethod& method) const;
    void detectLockStatements(CilRecoveredMethod& method) const;
    void detectForEachLoops(CilRecoveredMethod& method) const;

private:
    Options opts_;

    // ── Helpers ───────────────────────────────────────────────────────────

    // Detect get_X / set_X accessor calls
    static std::optional<PropertyPattern> matchPropertyCall(
        const CilStmt& stmt);

    // Detect ldftn + newobj Func<> / Action<> lambda construction
    static std::optional<LambdaInfo> matchLambdaConstruction(
        const CilExprPtr& expr);

    // Detect `isinst T; dup; brtrue; pop; ldnull` pattern
    static std::optional<IsTypePattern> matchIsTypePattern(
        const std::vector<CilStmt>& stmts, size_t pos);

    // Match `using` pattern: newobj/call + try-finally + Dispose()
    static bool matchUsingPattern(
        const std::vector<CilStmt>& stmts, size_t pos,
        std::string& varName, CilExprPtr& initExpr, std::vector<CilStmt>& body);

    // Match `lock` pattern: Monitor.Enter + try-finally + Monitor.Exit
    static bool matchLockPattern(
        const std::vector<CilStmt>& stmts, size_t pos,
        CilExprPtr& lockExpr, std::vector<CilStmt>& body);

    // Match `foreach` pattern: GetEnumerator + MoveNext + Current + finally Dispose
    static bool matchForEachPattern(
        const std::vector<CilStmt>& stmts, size_t pos,
        std::string& varName, BcType& varType,
        CilExprPtr& collection, std::vector<CilStmt>& body);

    // Check if a class is a compiler-generated async state machine
    static bool isAsyncSMClass(const BcClass& cls);
    static bool isIteratorSMClass(const BcClass& cls);

    // Walk statements recursively, applying a rewriter
    using StmtRewriter = std::function<bool(CilStmt&, std::vector<CilStmt>&)>;
    static void walkStmts(std::vector<CilStmt>& stmts,
                           const StmtRewriter& rewriter);

    // Rebuild async body from state machine MoveNext
    static std::vector<CilStmt> reconstructAsyncBody(
        const BcClass& smClass,
        const BcModule& module);

    // Rebuild iterator body from state machine MoveNext
    static std::vector<CilStmt> reconstructIteratorBody(
        const BcClass& smClass,
        const BcModule& module);
};

} // namespace cil_reconstruct
} // namespace retdec

#endif // RETDEC_CIL_RECONSTRUCT_CIL_PATTERNS_H
