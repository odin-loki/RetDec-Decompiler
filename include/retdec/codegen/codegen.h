/**
 * @file include/retdec/codegen/codegen.h
 * @brief Readability-Optimised C Code Generation (Stage 24).
 *
 * ## Overview
 *
 * This module is the final stage of the decompilation pipeline.  It consumes:
 *   - The structured CFG (`StructNode` tree from Stage 20)
 *   - SSA values with inferred types (Stage 19)
 *   - Calling conventions (Stage 21)
 *   - Dead code results (Stage 22)
 *   - IPA summaries (Stage 23)
 *
 * and emits compilable, idiomatic C source code.
 *
 * ## C AST
 *
 * The code generator first builds a C abstract syntax tree (AST) composed of
 * `CExpr` (expression nodes) and `CStmt` (statement nodes), then formats
 * the AST as a string.
 *
 * ### Expression nodes (`CExpr`)
 *
 *   Literal      — integer/float/string constant
 *   Var          — a named C variable
 *   BinOp        — a + b, a & b, a == b, ...
 *   UnOp         — -a, !a, ~a, *a, &a
 *   Cast         — (type)expr
 *   Call         — f(a, b, ...)
 *   Index        — p[i]   (array subscript)
 *   Member       — p->field  or  s.field
 *   Ternary      — cond ? then : else
 *   Comma        — a, b  (sequence)
 *
 * ### Statement nodes (`CStmt`)
 *
 *   Assign       — lhs = rhs;
 *   ExprStmt     — expr;
 *   If           — if (cond) { ... } [else { ... }]
 *   While        — while (cond) { ... }
 *   DoWhile      — do { ... } while (cond);
 *   For          — for (init; cond; incr) { ... }
 *   Switch       — switch (expr) { case N: ... }
 *   Return       — return expr;
 *   Break        — break;
 *   Continue     — continue;
 *   Goto         — goto label;
 *   Label        — label:
 *   Block        — { stmt; stmt; ... }
 *   Decl         — type name [= init];
 *
 * ## Code generation passes
 *
 * ### 1. Expression Coalescing
 *
 * SSA values with exactly one definition and exactly one use within the same
 * basic block (with no intervening side effects) are "coalesced" — their
 * defining expression is substituted directly at the use site, eliminating
 * an intermediate temporary variable.
 *
 * Example:
 *   Before: t1 = a + b; t2 = t1 * c;
 *   After:  t2 = (a + b) * c;
 *
 * ### 2. Condition Normalisation
 *
 * Canonicalise boolean expressions for readability:
 *   !(a >= b)   →  a < b
 *   !(a == b)   →  a != b
 *   (a != 0)    →  a          (in boolean context)
 *   (a == 0)    →  !a
 *   (a & (1<<k)) != 0  →  (a >> k) & 1   or keep as bit-test
 *   !(a)        →  !a          (already normal)
 *
 * ### 3. Loop Form Selection
 *
 * Given a `StructNode` loop:
 *   - `For`:     emit `for (init; cond; incr)` when init/cond/incr are
 *                syntactically separable expressions.
 *   - `While`:   emit `while (cond)`.
 *   - `DoWhile`: emit `do { } while (cond)`.
 *   - `Infinite`:emit `while (1)` — never add an inner `break` if the
 *                condition was not recoverable.
 *   - Never emit `while (1) { if (!cond) break; }` when `while (cond)` suffices.
 *
 * ### 4. Pointer Syntax Recovery
 *
 *   *(p + i)  with varying i  →  p[i]
 *   *(p + K)  with K == struct_field_offset  →  p->field_name
 *   p + loop_i * sizeof(T)  →  array decay p[]
 *
 * Cast minimisation:
 *   (int)(int)x  →  (int)x        (redundant cast chain)
 *   (int)x  where x is already int  →  x  (remove entirely)
 *
 * ### 5. Goto Elimination (Erosa-Hendren)
 *
 * For each remaining `goto` in the structured output:
 *   - Attempt to replace with a boolean flag variable approach
 *     (Erosa & Hendren, CC 1994):
 *       Before: goto L; ... L: stmt;
 *       After:  flag = 1; if (flag) { stmt; }
 *   - Only emit the actual `goto` if the flag variable would appear
 *     in more than 2 distinct statement positions (structurally unresolvable).
 *
 * ### 6. Output Formatting
 *
 * - 4-space indentation per level.
 * - K&R brace style: `{` on same line as control, `}` on own line.
 * - Types on separate declaration lines at function top.
 * - `#include` directives inferred from used standard functions.
 * - Empty lines between top-level declarations and between major statement groups.
 */

#ifndef RETDEC_CODEGEN_H
#define RETDEC_CODEGEN_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace ssa      { class SSAFunction; using ValueId = uint32_t; }
namespace cfg_structure { struct StructNode; }
namespace call_conv { struct CallingConvention; }
namespace dce       { struct DeadCodeResult; }
namespace ipa       { struct IpaResult; }
} // namespace retdec

namespace retdec {
namespace codegen {

// ─── C type representation ────────────────────────────────────────────────────

struct CType {
    enum class Kind : uint8_t {
        Void, Bool,
        Int8, Int16, Int32, Int64,
        UInt8, UInt16, UInt32, UInt64,
        Float, Double,
        Pointer,   ///< pointee stored in children[0]
        Array,     ///< element in children[0], size = arraySize
        Struct,    ///< named struct
        Union,     ///< named union (ambiguous global)
        FuncPtr,   ///< function pointer
        Unknown,
    };

    Kind        kind      = Kind::Unknown;
    bool        isConst   = false;
    bool        isVolatile= false;
    std::string name;              ///< struct/union name
    std::size_t arraySize = 0;
    std::vector<std::shared_ptr<CType>> children;

    static std::shared_ptr<CType> make(Kind k) {
        auto t = std::make_shared<CType>();
        t->kind = k;
        return t;
    }
    static std::shared_ptr<CType> ptr(std::shared_ptr<CType> of) {
        auto t = make(Kind::Pointer);
        t->children.push_back(std::move(of));
        return t;
    }
    static std::shared_ptr<CType> arr(std::shared_ptr<CType> elem, std::size_t n) {
        auto t = make(Kind::Array);
        t->children.push_back(std::move(elem));
        t->arraySize = n;
        return t;
    }

    std::string toString() const;
    bool isIntegral() const;
    bool isFloat()    const { return kind == Kind::Float || kind == Kind::Double; }
    bool isPointer()  const { return kind == Kind::Pointer; }
    bool isVoid()     const { return kind == Kind::Void; }
    uint8_t bitWidth() const;

    CType() = default;
    /// Internal: copy constructor that strips `isConst` (used in toString()).
    CType(const CType& o, bool stripConst);
};

// ─── C expression tree ────────────────────────────────────────────────────────

struct CExpr {
    enum class Kind : uint8_t {
        Literal, Var, BinOp, UnOp, Cast, Call,
        Index, Member, Ternary, Comma,
    };

    enum class BinOpKind : uint8_t {
        Add, Sub, Mul, Div, Mod,
        And, Or, Xor, Shl, Shr,
        LAnd, LOr,
        Eq, Ne, Lt, Le, Gt, Ge,
        Assign,
    };

    enum class UnOpKind : uint8_t {
        Neg, Not, BitNot, Deref, AddrOf, PreInc, PreDec, PostInc, PostDec,
    };

    Kind    kind = Kind::Literal;

    // Literal
    std::string literal;   ///< the printed constant (e.g. "42", "3.14f", "\"hello\"")

    // Var
    std::string varName;

    // BinOp
    BinOpKind binOp = BinOpKind::Add;

    // UnOp
    UnOpKind  unOp  = UnOpKind::Neg;

    // Call
    std::string callee;

    // Member: field name and whether it's -> (ptr) or . (value)
    std::string fieldName;
    bool        arrowAccess = false;

    // Type (for Cast)
    std::shared_ptr<CType> castType;

    // Children: sub-expressions
    std::vector<std::shared_ptr<CExpr>> children;

    // SSA value ID this expression came from (for coalescing bookkeeping)
    uint32_t ssaValueId = UINT32_MAX;

    // Inferred C type of this expression
    std::shared_ptr<CType> exprType;

    // Factory helpers
    static std::shared_ptr<CExpr> lit(std::string s, uint32_t vid = UINT32_MAX) {
        auto e = std::make_shared<CExpr>();
        e->kind = Kind::Literal; e->literal = std::move(s); e->ssaValueId = vid;
        return e;
    }
    static std::shared_ptr<CExpr> var(std::string n, uint32_t vid = UINT32_MAX) {
        auto e = std::make_shared<CExpr>();
        e->kind = Kind::Var; e->varName = std::move(n); e->ssaValueId = vid;
        return e;
    }
    static std::shared_ptr<CExpr> binop(BinOpKind op,
                                         std::shared_ptr<CExpr> lhs,
                                         std::shared_ptr<CExpr> rhs) {
        auto e = std::make_shared<CExpr>();
        e->kind = Kind::BinOp; e->binOp = op;
        e->children.push_back(std::move(lhs));
        e->children.push_back(std::move(rhs));
        return e;
    }
    static std::shared_ptr<CExpr> unop(UnOpKind op, std::shared_ptr<CExpr> operand) {
        auto e = std::make_shared<CExpr>();
        e->kind = Kind::UnOp; e->unOp = op;
        e->children.push_back(std::move(operand));
        return e;
    }
    static std::shared_ptr<CExpr> cast(std::shared_ptr<CType> t, std::shared_ptr<CExpr> sub) {
        auto e = std::make_shared<CExpr>();
        e->kind = Kind::Cast; e->castType = std::move(t);
        e->children.push_back(std::move(sub));
        return e;
    }
    static std::shared_ptr<CExpr> index(std::shared_ptr<CExpr> base,
                                         std::shared_ptr<CExpr> idx) {
        auto e = std::make_shared<CExpr>();
        e->kind = Kind::Index;
        e->children.push_back(std::move(base));
        e->children.push_back(std::move(idx));
        return e;
    }
    static std::shared_ptr<CExpr> member(std::shared_ptr<CExpr> base,
                                          std::string field, bool arrow) {
        auto e = std::make_shared<CExpr>();
        e->kind = Kind::Member; e->fieldName = std::move(field);
        e->arrowAccess = arrow;
        e->children.push_back(std::move(base));
        return e;
    }
    static std::shared_ptr<CExpr> call(std::string fn,
                                        std::vector<std::shared_ptr<CExpr>> args) {
        auto e = std::make_shared<CExpr>();
        e->kind = Kind::Call; e->callee = std::move(fn);
        e->children = std::move(args);
        return e;
    }
    static std::shared_ptr<CExpr> ternary(std::shared_ptr<CExpr> cond,
                                            std::shared_ptr<CExpr> then,
                                            std::shared_ptr<CExpr> els) {
        auto e = std::make_shared<CExpr>();
        e->kind = Kind::Ternary;
        e->children.push_back(std::move(cond));
        e->children.push_back(std::move(then));
        e->children.push_back(std::move(els));
        return e;
    }

    std::string toString(int prec = 0) const;
};

const char* binOpStr(CExpr::BinOpKind op) noexcept;
const char* unOpStr(CExpr::UnOpKind op)   noexcept;
int         binOpPrec(CExpr::BinOpKind op) noexcept;

// ─── C statement tree ─────────────────────────────────────────────────────────

struct CStmt {
    enum class Kind : uint8_t {
        Block, Decl, Assign, ExprStmt,
        If, While, DoWhile, For, Switch, Case, Default,
        Return, Break, Continue, Goto, Label,
    };

    Kind kind = Kind::ExprStmt;

    // Expressions used by this statement.
    std::shared_ptr<CExpr> expr;   ///< primary expression (condition / value)
    std::shared_ptr<CExpr> lhs;    ///< for Assign
    std::shared_ptr<CExpr> init;   ///< for For init
    std::shared_ptr<CExpr> incr;   ///< for For increment

    // Child statements (Block body, If then/else, etc.)
    std::vector<std::shared_ptr<CStmt>> children;

    // Decl fields
    std::string             declName;
    std::shared_ptr<CType>  declType;
    std::shared_ptr<CExpr>  declInit;

    // Label / Goto name
    std::string label;

    // Switch case value
    std::optional<int64_t> caseValue;  ///< nullopt = default

    // Factory helpers
    static std::shared_ptr<CStmt> block() {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::Block; return s;
    }
    static std::shared_ptr<CStmt> assign(std::shared_ptr<CExpr> l,
                                          std::shared_ptr<CExpr> r) {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::Assign;
        s->lhs = std::move(l); s->expr = std::move(r); return s;
    }
    static std::shared_ptr<CStmt> exprStmt(std::shared_ptr<CExpr> e) {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::ExprStmt;
        s->expr = std::move(e); return s;
    }
    static std::shared_ptr<CStmt> ifStmt(std::shared_ptr<CExpr> cond) {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::If;
        s->expr = std::move(cond); return s;
    }
    static std::shared_ptr<CStmt> whileStmt(std::shared_ptr<CExpr> cond) {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::While;
        s->expr = std::move(cond); return s;
    }
    static std::shared_ptr<CStmt> doWhileStmt(std::shared_ptr<CExpr> cond) {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::DoWhile;
        s->expr = std::move(cond); return s;
    }
    static std::shared_ptr<CStmt> forStmt(std::shared_ptr<CExpr> init,
                                            std::shared_ptr<CExpr> cond,
                                            std::shared_ptr<CExpr> incr) {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::For;
        s->init = std::move(init); s->expr = std::move(cond);
        s->incr = std::move(incr); return s;
    }
    static std::shared_ptr<CStmt> retStmt(std::shared_ptr<CExpr> val = nullptr) {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::Return;
        s->expr = std::move(val); return s;
    }
    static std::shared_ptr<CStmt> gotoStmt(std::string lbl) {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::Goto;
        s->label = std::move(lbl); return s;
    }
    static std::shared_ptr<CStmt> labelStmt(std::string lbl) {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::Label;
        s->label = std::move(lbl); return s;
    }
    static std::shared_ptr<CStmt> declStmt(std::string name,
                                             std::shared_ptr<CType> type,
                                             std::shared_ptr<CExpr> init = nullptr) {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::Decl;
        s->declName = std::move(name); s->declType = std::move(type);
        s->declInit = std::move(init); return s;
    }
    static std::shared_ptr<CStmt> breakStmt() {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::Break; return s;
    }
    static std::shared_ptr<CStmt> continueStmt() {
        auto s = std::make_shared<CStmt>(); s->kind = Kind::Continue; return s;
    }
};

// ─── C function representation ────────────────────────────────────────────────

struct CParam {
    std::string            name;
    std::shared_ptr<CType> type;
};

struct CFunction {
    std::string              name;
    std::shared_ptr<CType>   returnType;
    std::vector<CParam>      params;
    bool                     isVariadic  = false;
    bool                     isStatic    = false;
    bool                     isInline    = false;
    std::shared_ptr<CStmt>   body;        ///< Kind::Block containing all stmts
};

// ─── C translation unit ───────────────────────────────────────────────────────

struct CUnit {
    std::string                  filename;
    std::vector<std::string>     includes;
    std::vector<CFunction>       functions;
    std::vector<std::string>     typeDecls;  ///< struct/typedef declarations
    std::string                  globalDecls;
};

// ─── Expression coalescer ─────────────────────────────────────────────────────

/**
 * Builds an expression tree for each SSA value and coalesces single-def/
 * single-use temporaries into their use sites.
 *
 * Input:  SSAFunction with inferred types and DCE result (liveInstrs).
 * Output: A map from SSA ValueId → CExpr (the expression for that value).
 *
 * Algorithm:
 *   1. Build a use-count map: for each live instruction, count how many
 *      live instructions use its def value.
 *   2. For each live instruction, build a CExpr from its operands
 *      (recursively coalescing single-use sub-expressions).
 *   3. An instruction is "inlined" if:
 *      a. Its def value has exactly one use.
 *      b. That use is in the same basic block.
 *      c. No side-effectful instruction (Call, Store) appears between
 *         the definition and the use.
 */
class ExprCoalescer {
public:
    struct Result {
        std::unordered_map<uint32_t, std::shared_ptr<CExpr>> valueExprs;
        std::unordered_set<uint32_t> inlinedValues;  ///< suppressed temporaries
        std::unordered_map<std::string, std::string> varNames; ///< valueId → C name
    };

    Result run(const ssa::SSAFunction& fn,
               const dce::DeadCodeResult& dce) const;

private:
    std::shared_ptr<CExpr> buildExpr(uint32_t valueId,
                                      const ssa::SSAFunction& fn,
                                      Result& res,
                                      std::unordered_set<uint32_t>& inProgress) const;

    std::string nameForValue(uint32_t vid, const ssa::SSAFunction& fn) const;
};

// ─── Condition normaliser ─────────────────────────────────────────────────────

/**
 * Applies algebraic rewrites to boolean expressions for readability.
 *
 * Rules (applied bottom-up):
 *   NOT(a >= b)  →  a < b
 *   NOT(a > b)   →  a <= b
 *   NOT(a == b)  →  a != b
 *   NOT(a != b)  →  a == b
 *   NOT(NOT(a))  →  a
 *   (a != 0)     →  a          [in boolean context]
 *   (a == 0)     →  !a         [in boolean context]
 *   (a & 1) != 0 →  a & 1      [bit-test]
 *   (a & pow2) != 0 → a & pow2  [bit-test]
 */
class CondNormaliser {
public:
    std::shared_ptr<CExpr> normalise(std::shared_ptr<CExpr> expr,
                                      bool boolContext = false) const;

private:
    std::shared_ptr<CExpr> flipCmp(std::shared_ptr<CExpr> e) const;
    bool isPowerOfTwo(std::shared_ptr<CExpr> e) const;
};

// ─── Loop form selector ───────────────────────────────────────────────────────

/**
 * Converts a `StructNode` loop into the most specific C loop form:
 *   For   → `for (init; cond; incr) { }`
 *   While → `while (cond) { }`
 *   DoWhile → `do { } while (cond);`
 *   Infinite → `while (1) { }`
 *
 * Never emits `while (1) { if (!cond) break; }` when the condition is
 * syntactically available at the loop entry.
 */
class LoopFormSelector {
public:
    std::shared_ptr<CStmt> select(
        const cfg_structure::StructNode& loop,
        std::shared_ptr<CExpr> cond,
        std::shared_ptr<CExpr> init,
        std::shared_ptr<CExpr> incr,
        std::shared_ptr<CStmt> body) const;
};

// ─── Pointer syntax recovery ─────────────────────────────────────────────────

/**
 * Converts low-level pointer arithmetic to idiomatic C syntax:
 *
 *   Deref(Add(p, i))         →  p[i]          (array subscript)
 *   Deref(Add(p, K))         →  p->field       (struct member)
 *   Add(p, Mul(i, stride))   →  p[i]           (scaled subscript)
 *
 * Cast minimisation:
 *   Cast(T, Cast(T, x))      →  Cast(T, x)     (duplicate cast)
 *   Cast(T, x) where x:T     →  x              (no-op cast)
 */
class PointerSyntax {
public:
    struct StructInfo {
        std::string typeName;
        std::unordered_map<int64_t, std::string> fields; ///< offset → field name
    };

    std::shared_ptr<CExpr> recover(
        std::shared_ptr<CExpr> expr,
        const std::unordered_map<std::string, StructInfo>& structs = {}) const;

private:
    std::shared_ptr<CExpr> trySubscript(std::shared_ptr<CExpr> expr) const;
    std::shared_ptr<CExpr> tryMember(
        std::shared_ptr<CExpr> expr,
        const std::unordered_map<std::string, StructInfo>& structs) const;
    std::shared_ptr<CExpr> minimiseCasts(std::shared_ptr<CExpr> expr) const;
    bool isCastRedundant(const CType& outer, const CExpr& inner) const;
};

// ─── Goto eliminator (Erosa-Hendren) ─────────────────────────────────────────

/**
 * Attempts to eliminate `goto` statements by introducing boolean flag
 * variables.  Only emits an actual `goto` if the flag would appear in
 * more than 2 positions (structurally unresolvable irreducible region).
 *
 * Algorithm (simplified Erosa-Hendren):
 *   For each goto G targeting label L:
 *     1. Identify all statements between G and L.
 *     2. Wrap them in `if (!flag) { ... }` where flag is initially 0
 *        and set to 1 when G executes.
 *     3. If fewer than 3 flag uses result → apply transformation.
 *     4. Otherwise: keep the goto (irreducible case).
 */
class GotoEliminator {
public:
    static constexpr int kMaxFlagUses = 2;

    /// Returns the (potentially modified) statement tree.
    std::shared_ptr<CStmt> eliminate(std::shared_ptr<CStmt> body) const;

private:
    struct GotoInfo {
        std::string label;
        int         flagUseCount = 0;
    };
    void countGotos(const CStmt* s,
                    std::unordered_map<std::string, GotoInfo>& info) const;
    std::shared_ptr<CStmt> rewrite(std::shared_ptr<CStmt> s,
                                    const std::unordered_set<std::string>& eliminate,
                                    std::unordered_map<std::string, std::string>& flags) const;
};

// ─── Output formatter / emitter ──────────────────────────────────────────────

/**
 * Formats the C AST as a string with consistent style:
 *   - 4-space indentation per nesting level.
 *   - K&R brace style: `{` on the same line as the control keyword.
 *   - `}` on its own line.
 *   - One blank line between top-level function definitions.
 *   - Variable declarations grouped at the top of each function body.
 */
class Emitter {
public:
    struct Config {
        int   indentWidth   = 4;
        bool  krBraces      = true;   ///< K&R vs Allman
        bool  addLineNums   = false;  ///< add /* vma: 0xABCD */ comments
        bool  declsAtTop    = true;   ///< hoist all decls to function top
    };
    static Config defaultConfig() noexcept { return {}; }

    std::string emitUnit(const CUnit& unit, const Config& cfg = defaultConfig()) const;
    std::string emitFunction(const CFunction& fn, const Config& cfg = defaultConfig()) const;
    std::string emitStmt(const CStmt& stmt, int indent, const Config& cfg) const;
    std::string emitExpr(const CExpr& expr, int outerPrec = 0) const;
    std::string emitType(const CType& type, const std::string& name = "") const;

private:
    std::string ind(int level, int width) const;
    std::string emitBlock(const CStmt& block, int indent, const Config& cfg) const;
};

// ─── Main code generation pass ────────────────────────────────────────────────

/**
 * Orchestrates the full code generation pipeline for one function:
 *   1. ExprCoalescer  — build expression trees, coalesce temporaries
 *   2. CondNormaliser — readability rewrites for conditions
 *   3. LoopFormSelector — choose best loop form
 *   4. PointerSyntax  — recover array/struct syntax, minimise casts
 *   5. GotoEliminator — eliminate gotos where possible
 *   6. Emitter        — format as C string
 */
class CodeGenPass {
public:
    struct Config {
        Emitter::Config     emitter;
        bool                enableGotoElim    = true;
        bool                enableCoalescing  = true;
        bool                enableCondNorm    = true;
        bool                enablePtrSyntax   = true;
        PointerSyntax::StructInfo* structInfo = nullptr;
    };
    static Config defaultConfig() noexcept { return {}; }

    struct Stats {
        std::size_t coalescedTemps    = 0;
        std::size_t condRewrites      = 0;
        std::size_t gotosEliminated   = 0;
        std::size_t gotosRemaining    = 0;
        std::size_t castsRemoved      = 0;
        std::size_t totalFunctions    = 0;
    };

    CFunction generateFunction(
        const ssa::SSAFunction& fn,
        const cfg_structure::StructNode& structTree,
        const call_conv::CallingConvention& cc,
        const dce::DeadCodeResult& dce,
        const Config& cfg = defaultConfig()) const;

    CUnit generateUnit(
        const std::vector<const ssa::SSAFunction*>& fns,
        const std::vector<const cfg_structure::StructNode*>& trees,
        const std::unordered_map<std::string, call_conv::CallingConvention>& ccMap,
        const std::unordered_map<std::string, dce::DeadCodeResult>& dceMap,
        const Config& cfg = defaultConfig()) const;

    std::string emit(const CFunction& fn, const Config& cfg = defaultConfig()) const;

    const Stats& stats() const { return stats_; }

private:
    mutable Stats stats_;

    std::shared_ptr<CStmt> structNodeToStmt(
        const cfg_structure::StructNode& node,
        const ExprCoalescer::Result& exprs,
        const call_conv::CallingConvention& cc,
        const dce::DeadCodeResult& dce,
        const ssa::SSAFunction& fn,
        const Config& cfg) const;

    std::shared_ptr<CExpr> valueToExpr(
        uint32_t valueId,
        const ExprCoalescer::Result& exprs,
        const ssa::SSAFunction& fn) const;
};

} // namespace codegen
} // namespace retdec

#endif // RETDEC_CODEGEN_H
