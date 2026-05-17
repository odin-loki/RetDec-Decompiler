/**
 * @file include/retdec/cxx_backend/cxx_lifter.h
 * @brief Lifts plain codegen::CUnit to CxxUnit by detecting C++ patterns.
 *
 * ## Detection passes (run in order)
 *
 * ### 1. Vtable Detection
 *
 * Identifies function-pointer tables that follow the Itanium ABI vtable
 * layout (virtual function pointers, RTTI pointer, offset-to-top):
 *
 *   - Look for global arrays of function-pointer types.
 *   - Match against the `type_info`-style RTTI structures.
 *   - Assign each vtable to a class name derived from the mangled symbol or
 *     RTTI string.
 *
 * ### 2. Constructor / Destructor Identification
 *
 * Heuristics:
 *   - A function whose first parameter is a pointer to a struct and whose
 *     first action is a vtable store (`*(this+0) = &vtable`) is a constructor.
 *   - A function that zeroes vtable pointer and calls a destructor is
 *     a destructor.
 *   - Mangled name prefix `_ZN…C[12]` (Itanium) confirms constructor.
 *   - Mangled name prefix `_ZN…D[012]` confirms destructor.
 *
 * ### 3. new / delete Recovery
 *
 * Patterns:
 *   - `t = (T*)malloc(sizeof(T)); constructor(t, …)` → `t = new T(…)`
 *   - `destructor(t); free(t)` → `delete t`
 *   - `t = (T*)malloc(n * sizeof(T))` → `t = new T[n]`
 *   - `free(t)` preceded by array destructor loop → `delete[] t`
 *
 * ### 4. Exception Handling Recovery
 *
 * Reads the exception-handling metadata produced by eh_reconstruct and
 * promotes `__cxa_throw` / `__cxa_begin_catch` / `__cxa_end_catch` calls
 * to `throw` / `try` / `catch` AST nodes.
 *
 * ### 5. Class Member Inference
 *
 * Uses pointer-arithmetic-to-struct-field mapping (from PointerSyntax) to
 * determine which fields belong to which class, producing `CxxClass::Field`
 * entries.
 *
 * ### 6. Namespace Recovery
 *
 * Groups classes and functions that share a mangled-name namespace prefix
 * into `CxxNamespace` blocks.
 *
 * ### 7. Template Skeleton Detection
 *
 * Identifies multiple functions or classes with identical structure but
 * differing only in scalar-type parameters, and promotes them to a single
 * `CxxTemplate` skeleton with a comment noting the instantiations.
 */

#ifndef RETDEC_CXX_BACKEND_CXX_LIFTER_H
#define RETDEC_CXX_BACKEND_CXX_LIFTER_H

#include "retdec/cxx_backend/cxx_ast.h"
#include "retdec/codegen/codegen.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace cxx_backend {

// ─── Evidence structures ──────────────────────────────────────────────────────

/**
 * @brief Vtable evidence collected from binary analysis.
 */
struct VtableEntry {
    std::string className;     ///< demangled class name (or "_ZTS"-derived)
    std::string rttiSymbol;    ///< RTTI symbol name
    uint64_t    address = 0;   ///< vtable address in image
    std::vector<std::string> virtualFunctions;  ///< ordered virtual func names
};

/**
 * @brief Exception handling try-region metadata (from eh_reconstruct).
 */
struct EhRegion {
    uint64_t     tryStart  = 0;
    uint64_t     tryEnd    = 0;
    std::string  catchType;      ///< type name, "" = catch(...)
    std::string  catchVar;
    uint64_t     handlerAddr = 0;
};

/**
 * @brief Input context for the lifter.
 */
struct CxxLiftContext {
    /// Vtable entries found by the binary analyser.
    std::vector<VtableEntry> vtables;

    /// EH regions per function (keyed by function name).
    std::unordered_map<std::string, std::vector<EhRegion>> ehRegions;

    /// Mangled symbol → demangled name mapping.
    std::unordered_map<std::string, std::string> demangledNames;

    /// Struct field offsets (from var_recovery / pointer-syntax).
    std::unordered_map<std::string,
        std::unordered_map<int64_t, std::string>> structFields;

    /// Whether RTTI analysis identified any C++ class hierarchies.
    bool hasCxxEvidence = false;
};

// ─── Lifter passes ────────────────────────────────────────────────────────────

/**
 * @brief Detects vtables and populates class skeletons.
 */
class VtableDetector {
public:
    /// Fill in class skeletons from vtable evidence.
    std::vector<CxxClass> detect(const codegen::CUnit& unit,
                                  const CxxLiftContext& ctx) const;

private:
    std::string className(const VtableEntry& vt) const;
    bool isVtableStore(const codegen::CFunction& fn,
                       const VtableEntry& vt) const;
};

/**
 * @brief Identifies constructor and destructor methods.
 */
class CtorDtorDetector {
public:
    struct Result {
        std::unordered_set<std::string> constructors;
        std::unordered_set<std::string> destructors;
    };

    Result detect(const codegen::CUnit& unit,
                  const CxxLiftContext& ctx) const;

private:
    bool looksLikeConstructor(const codegen::CFunction& fn,
                               const CxxLiftContext& ctx) const;
    bool looksLikeDestructor(const codegen::CFunction& fn,
                              const CxxLiftContext& ctx) const;
};

/**
 * @brief Promotes malloc/free pairs to new/delete expressions.
 */
class NewDeleteRecovery {
public:
    struct Replacement {
        std::string functionName;
        /// Index of the statement to replace.
        size_t      stmtIndex = 0;
        bool        isNew     = true;
        bool        isArray   = false;
    };

    std::vector<Replacement> analyse(const codegen::CUnit& unit) const;
    codegen::CFunction applyToFunction(codegen::CFunction fn,
                                        const std::vector<Replacement>& reps) const;
private:
    bool isMallocCall(const codegen::CExpr& e, size_t* sizeOut = nullptr) const;
    bool isFreeCall(const codegen::CExpr& e) const;
};

/**
 * @brief Promotes __cxa_throw / __cxa_begin_catch to CxxTryStmt.
 */
class EhRecovery {
public:
    void applyToFunction(codegen::CFunction& fn,
                         const std::vector<EhRegion>& regions,
                         std::vector<CxxTryStmt>& triesOut) const;
};

/**
 * @brief Groups functions/classes by namespace prefix.
 */
class NamespaceGrouper {
public:
    std::vector<CxxNamespace> group(const std::vector<CxxClass>& classes,
                                     const std::vector<codegen::CFunction>& fns,
                                     const CxxLiftContext& ctx) const;
private:
    std::string extractNamespace(const std::string& demangledName) const;
};

// ─── Main lifter ──────────────────────────────────────────────────────────────

struct CxxLiftOptions {
    bool recoverNewDelete    = true;
    bool recoverExceptions   = true;
    bool detectVtables       = true;
    bool groupNamespaces     = true;
    bool detectCtorDtor      = true;
};

struct CxxLiftResult {
    CxxUnit                  unit;
    std::vector<std::string> warnings;
    bool                     isCxx = false;  ///< was any C++ evidence found?
};

/**
 * @brief Top-level orchestrator: takes a plain-C CUnit + lift context,
 *        runs all detection passes, and produces a CxxUnit.
 */
class CxxLifter {
public:
    explicit CxxLifter(CxxLiftOptions opts = CxxLiftOptions{});

    CxxLiftResult lift(const codegen::CUnit& cUnit,
                       const CxxLiftContext& ctx) const;

private:
    CxxLiftOptions opts_;

    void populateIncludes(CxxUnit& unit, bool hasCxx) const;
};

} // namespace cxx_backend
} // namespace retdec

#endif // RETDEC_CXX_BACKEND_CXX_LIFTER_H
