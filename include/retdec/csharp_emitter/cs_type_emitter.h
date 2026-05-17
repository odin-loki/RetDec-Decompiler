/**
 * @file include/retdec/csharp_emitter/cs_type_emitter.h
 * @brief BcClass → C# type declaration (class/struct/record/interface/enum/delegate).
 *
 * ## Type forms supported
 *
 *   - **class**: regular reference type
 *   - **struct**: value type (`is(access & Static) && !isInterface`)
 *   - **record class / record struct**: detected by `BcClass::isRecord`
 *   - **interface**: `BcClass::isInterface`
 *   - **enum**: `BcClass::isEnum`
 *   - **delegate**: detected by single `Invoke` method with no body
 *   - **abstract class**: `BcClass::isAbstract && !isInterface`
 *   - **sealed class**: detected from access flags
 *   - **static class**: all members static + no instance members
 *   - **partial**: added when the class is compiler-generated async/iterator SM
 *
 * ## Generic parameter constraints
 *
 *   - `where T : class` (reference type)
 *   - `where T : struct` (value type)
 *   - `where T : new()` (parameterless ctor)
 *   - `where T : SomeBase` (base class constraint)
 *   - `where T : ISomeInterface` (interface constraint)
 *   - `where T : notnull` (C# 8+)
 *   - `where T : unmanaged` (C# 7.3+)
 *
 * ## Members emitted
 *
 *   1. Fields (constants first, then static, then instance)
 *   2. Constructors
 *   3. Destructor / finalizer
 *   4. Properties (auto, computed, expression-bodied)
 *   5. Events
 *   6. Indexers
 *   7. Operators
 *   8. Regular methods
 *   9. Nested types
 */

#ifndef RETDEC_CSHARP_EMITTER_CS_TYPE_EMITTER_H
#define RETDEC_CSHARP_EMITTER_CS_TYPE_EMITTER_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/cil_reconstruct/cil_reconstructor.h"
#include "retdec/csharp_emitter/cs_expr_emitter.h"
#include "retdec/csharp_emitter/cs_stmt_emitter.h"
#include "retdec/csharp_emitter/cs_writer.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace csharp_emitter {

using namespace bc_module;
using namespace cil_reconstruct;

// ─── CsTypeEmitter ───────────────────────────────────────────────────────────

/**
 * @brief Emits a single BcClass declaration (including all its members)
 *        into a CsWriter.
 */
class CsTypeEmitter {
public:
    struct Options {
        bool omitCompilerGenerated  = true;  ///< Skip <...>d__N state machines
        bool emitXmlDoc             = true;
        bool emitRegions            = false; ///< #region grouping
        bool preferAutoProperties   = true;  ///< Detect simple get/set → auto
        bool emitPartialForSMs      = true;  ///< Emit partial for state machines
        int  csharpVersion          = 12;
    };
    static Options defaultOptions() noexcept { return {}; }

    CsTypeEmitter(CsWriter& writer,
                  const CsExprEmitter& expr,
                  const CsStmtEmitter& stmt,
                  Options opts = defaultOptions());

    /**
     * @brief Emit the complete C# declaration for `cls`.
     *
     * @param cls      The class to emit.
     * @param results  Per-method reconstruction results (method body CilStmts).
     * @param module   The owning module (for cross-type references).
     * @param indent   Starting indent level.
     */
    void emitClass(const BcClass& cls,
                   const std::unordered_map<std::string, CilReconstructResult>& results,
                   const BcModule& module);

private:
    CsWriter&            writer_;
    const CsExprEmitter& expr_;
    const CsStmtEmitter& stmt_;
    Options              opts_;

    // ── Type header ──────────────────────────────────────────────────────────

    void emitTypeHeader(const BcClass& cls);
    void emitTypeModifiers(const BcClass& cls);
    void emitTypeKeyword(const BcClass& cls);
    void emitGenericParams(const std::vector<std::string>& typeParams);
    void emitBaseList(const BcClass& cls);
    void emitGenericConstraints(const std::vector<std::string>& typeParams);

    // ── Members ───────────────────────────────────────────────────────────────

    void emitFields(const BcClass& cls);
    void emitField(const BcField& f);

    void emitConstructors(const BcClass& cls,
                          const std::unordered_map<std::string, CilReconstructResult>& results);
    void emitDestructor(const BcClass& cls,
                        const std::unordered_map<std::string, CilReconstructResult>& results);

    void emitProperties(const BcClass& cls,
                        const std::unordered_map<std::string, CilReconstructResult>& results);
    void emitAutoProperty(const std::string& propName, const BcType& propType,
                          bool hasGetter, bool hasSetter, bool isStatic,
                          const std::string& modifiers);
    void emitComputedProperty(const std::string& propName, const BcType& propType,
                              const BcMethod* getter, const BcMethod* setter,
                              const std::unordered_map<std::string, CilReconstructResult>& results,
                              const std::string& modifiers);

    void emitEvents(const BcClass& cls,
                    const std::unordered_map<std::string, CilReconstructResult>& results);

    void emitMethods(const BcClass& cls,
                     const std::unordered_map<std::string, CilReconstructResult>& results);
    void emitMethod(const BcClass& cls, const BcMethod& m,
                    const std::unordered_map<std::string, CilReconstructResult>& results);
    void emitMethodSignature(const BcClass& cls, const BcMethod& m);
    void emitMethodBody(const BcMethod& m,
                        const std::unordered_map<std::string, CilReconstructResult>& results);

    void emitEnum(const BcClass& cls);

    // ── Helpers ───────────────────────────────────────────────────────────────

    std::string accessModifiers(BcAccess access) const;
    std::string methodKey(const BcClass& cls, const BcMethod& m) const;

    bool isDelegate(const BcClass& cls) const;
    bool isCompilerGenerated(const BcClass& cls) const;
    bool isPropertyAccessor(const BcMethod& m) const;
    bool isEventAccessor(const BcMethod& m) const;
    bool isOperator(const BcMethod& m) const;

    /// Group get_X / set_X / add_X / remove_X methods by property/event name.
    struct PropertyGroup {
        std::string name;
        BcType      type;
        BcMethod*   getter = nullptr;
        BcMethod*   setter = nullptr;
        bool        isStatic = false;
        std::string modifiers;
    };
    std::vector<PropertyGroup> collectProperties(const BcClass& cls) const;

    struct EventGroup {
        std::string name;
        BcType      type;
        BcMethod*   adder   = nullptr;
        BcMethod*   remover = nullptr;
        bool        isStatic = false;
    };
    std::vector<EventGroup> collectEvents(const BcClass& cls) const;
};

} // namespace csharp_emitter
} // namespace retdec

#endif // RETDEC_CSHARP_EMITTER_CS_TYPE_EMITTER_H
