/**
 * @file include/retdec/vbnet_emitter/vb_type_emitter.h
 * @brief BcClass → VB.NET type declaration emitter.
 *
 * Emits idiomatic VB.NET for common CLR type patterns:
 *
 *   - **class**          → Class ... End Class
 *   - **interface**      → Interface ... End Interface
 *   - **abstract class** → MustInherit Class ... End Class
 *   - **sealed class**   → NotInheritable Class ... End Class
 *   - **enum**           → Enum ... End Enum
 *   - **struct**         → Structure ... End Structure
 *   - **record**         → Class with ReadOnly properties (VB.NET 16.9+)
 *   - **module**         → Module ... End Module (static classes)
 *   - **delegate**       → Delegate Sub/Function
 *
 * Method bodies emit Throw New NotImplementedException() or best-effort
 * imperative code from the BcCFG.
 */

#ifndef RETDEC_VBNET_EMITTER_VB_TYPE_EMITTER_H
#define RETDEC_VBNET_EMITTER_VB_TYPE_EMITTER_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/vbnet_emitter/vb_writer.h"

#include <string>
#include <vector>

namespace retdec {
namespace vbnet_emitter {

using namespace bc_module;

class VbTypeEmitter {
public:
    struct Options {
        bool omitCompilerGenerated = true;
        bool emitXmlDoc            = true;
        bool emitLineNumbers       = false;
    };
    static Options defaultOptions() noexcept { return {}; }

    VbTypeEmitter(VbWriter& writer, Options opts = defaultOptions());

    void emitClass(const BcClass& cls, const BcModule& module);

private:
    VbWriter& writer_;
    Options   opts_;

    // ── Type forms ──────────────────────────────────────────────────────────

    void emitRegularClass(const BcClass& cls);
    void emitInterface(const BcClass& cls);
    void emitEnum(const BcClass& cls);
    void emitStructure(const BcClass& cls);
    void emitModule(const BcClass& cls);
    void emitDelegate(const BcClass& cls);

    // ── Members ─────────────────────────────────────────────────────────────

    void emitField(const BcField& f);
    void emitMethod(const BcClass& cls, const BcMethod& m);
    void emitConstructor(const BcClass& cls, const BcMethod& m);
    void emitProperty(const std::string& propName, const BcType& propType,
                      const BcMethod* getter, const BcMethod* setter,
                      bool isStatic);
    void emitAbstractMethod(const BcMethod& m);
    void emitModuleMethod(const BcMethod& m);  ///< For Module-level Sub/Function

    // ── Signature helpers ────────────────────────────────────────────────────

    std::string typeStr(const BcType& t) const;
    std::string accessStr(BcAccess acc) const;
    std::string methodModifiers(const BcMethod& m) const;
    std::string fieldModifiers(const BcField& f) const;
    std::string paramList(const BcMethod& m) const;
    bool isVoid(const BcMethod& m) const;

    // ── Detection ────────────────────────────────────────────────────────────

    bool isStaticClass(const BcClass& cls) const;
    bool isDelegate(const BcClass& cls) const;
    bool isCompilerGenerated(const BcClass& cls) const;
    bool isPropertyGetter(const BcMethod& m) const;
    bool isPropertySetter(const BcMethod& m) const;
    bool isEventAccessor(const BcMethod& m) const;

    struct PropGroup {
        std::string name;
        BcType      type;
        const BcMethod* getter = nullptr;
        const BcMethod* setter = nullptr;
        bool isStatic = false;
    };
    std::vector<PropGroup> collectProperties(const BcClass& cls) const;
};

} // namespace vbnet_emitter
} // namespace retdec

#endif // RETDEC_VBNET_EMITTER_VB_TYPE_EMITTER_H
