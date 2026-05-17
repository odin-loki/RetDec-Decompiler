/**
 * @file include/retdec/fsharp_emitter/fs_type_emitter.h
 * @brief BcClass → F# type declaration emitter.
 *
 * Emits idiomatic F# for common CLR type patterns:
 *
 *   - **class**          → `type Foo() = ...` with `member` bindings
 *   - **interface**      → `type IFoo = abstract member ...`
 *   - **abstract class** → `[<AbstractClass>] type Foo() = ...`
 *   - **enum**           → discriminated union with `| Case = value`
 *   - **struct**         → `[<Struct>] type Foo = struct ... end`
 *   - **record**         → `type Foo = { field: T; ... }`
 *   - **static class**   → `module Foo = ...` (F# module idiom)
 *   - **delegate**       → `type Del = delegate of T -> R`
 *
 * Method bodies are lifted from BcCFG instructions as best-effort
 * imperative F# code or replaced with `failwith "TODO"` when the CFG
 * is empty / opaque.
 */

#ifndef RETDEC_FSHARP_EMITTER_FS_TYPE_EMITTER_H
#define RETDEC_FSHARP_EMITTER_FS_TYPE_EMITTER_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/fsharp_emitter/fs_writer.h"

#include <string>
#include <vector>

namespace retdec {
namespace fsharp_emitter {

using namespace bc_module;

class FsTypeEmitter {
public:
    struct Options {
        bool omitCompilerGenerated = true;
        bool emitXmlDoc            = true;
        bool preferRecords         = true;  ///< Emit records for simple data classes
        bool preferModules         = true;  ///< Static classes → F# modules
    };
    static Options defaultOptions() noexcept { return {}; }

    FsTypeEmitter(FsWriter& writer, Options opts = defaultOptions());

    void emitClass(const BcClass& cls, const BcModule& module);

private:
    FsWriter& writer_;
    Options   opts_;

    // ── Type forms ──────────────────────────────────────────────────────────

    void emitRecord(const BcClass& cls);
    void emitDU(const BcClass& cls);           ///< Discriminated union (enum)
    void emitDelegate(const BcClass& cls);
    void emitModule(const BcClass& cls, const BcModule& module);
    void emitClassType(const BcClass& cls, const BcModule& module);
    void emitInterfaceType(const BcClass& cls);

    // ── Members ─────────────────────────────────────────────────────────────

    void emitField(const BcField& f, bool isLet);
    void emitConstructor(const BcClass& cls, const BcMethod& m);
    void emitMember(const BcClass& cls, const BcMethod& m);
    void emitAbstractMember(const BcMethod& m);
    void emitLetBinding(const BcMethod& m);     ///< For module-level functions

    // ── Signature helpers ───────────────────────────────────────────────────

    std::string typeStr(const BcType& t) const;
    std::string accessStr(BcAccess acc) const;
    std::string methodParams(const BcMethod& m) const;
    std::string methodReturnType(const BcMethod& m) const;
    std::string memberModifiers(const BcMethod& m, bool inInterface) const;
    std::string fieldModifiers(const BcField& f) const;

    // ── Detection helpers ────────────────────────────────────────────────────

    bool isStaticClass(const BcClass& cls) const;
    bool isDelegate(const BcClass& cls) const;
    bool isSimpleRecord(const BcClass& cls) const;
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

} // namespace fsharp_emitter
} // namespace retdec

#endif // RETDEC_FSHARP_EMITTER_FS_TYPE_EMITTER_H
