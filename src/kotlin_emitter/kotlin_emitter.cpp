/**
 * @file src/kotlin_emitter/kotlin_emitter.cpp
 * @brief Kotlin source emitter — produces idiomatic .kt source.
 */

#include "retdec/kotlin_emitter/kotlin_emitter.h"

#include <algorithm>
#include <cassert>
#include <sstream>

namespace retdec {
namespace kotlin_emitter {

// ─── KotlinEmitter ────────────────────────────────────────────────────────────

KotlinEmitter::KotlinEmitter(const KtEmitOptions& opts) : opts_(opts) {}

// ─── Modifier helpers ─────────────────────────────────────────────────────────

std::string KotlinEmitter::classModifiers(const KtClass& cls) const {
    std::string mods;
    auto addMod = [&](const std::string& m) {
        if (!mods.empty()) mods += " ";
        mods += m;
    };

    std::string vis = visibilityStr(cls.visibility);
    if (!vis.empty()) addMod(vis);

    switch (cls.kind) {
        case KtClassKind::DataClass:       addMod("data"); break;
        case KtClassKind::SealedClass:     addMod("sealed"); break;
        case KtClassKind::SealedInterface: addMod("sealed"); break;
        case KtClassKind::ValueClass:      addMod("value"); break;
        case KtClassKind::FunInterface:    addMod("fun"); break;
        default: break;
    }

    if (cls.isAbstract && cls.kind == KtClassKind::Class)
        addMod("abstract");
    else if (cls.isOpen && cls.kind == KtClassKind::Class)
        addMod("open");
    if (cls.isExpect) addMod("expect");
    if (cls.isExternal) addMod("external");
    if (cls.isInner) addMod("inner");

    return mods;
}

std::string KotlinEmitter::functionModifiers(const KtFunction& fn) const {
    std::string mods;
    auto addMod = [&](const std::string& m) {
        if (!mods.empty()) mods += " ";
        mods += m;
    };

    std::string vis = visibilityStr(fn.visibility);
    if (!vis.empty()) addMod(vis);
    if (fn.isOverride)  addMod("override");
    if (fn.isAbstract)  addMod("abstract");
    if (fn.isOpen)      addMod("open");
    if (fn.isInline)    addMod("inline");
    if (fn.isTailrec)   addMod("tailrec");
    if (fn.isOperator)  addMod("operator");
    if (fn.isInfix)     addMod("infix");
    if (fn.isSuspend)   addMod("suspend");
    if (fn.isExternal)  addMod("external");
    if (fn.isExpect)    addMod("expect");

    return mods;
}

std::string KotlinEmitter::propertyModifiers(const KtProperty& prop) const {
    std::string mods;
    auto addMod = [&](const std::string& m) {
        if (!mods.empty()) mods += " ";
        mods += m;
    };

    std::string vis = visibilityStr(prop.visibility);
    if (!vis.empty()) addMod(vis);
    if (prop.isOverride)  addMod("override");
    if (prop.isConst)     addMod("const");
    if (prop.isLateinit)  addMod("lateinit");
    if (prop.isAbstract)  addMod("abstract");
    if (prop.isOpen)      addMod("open");

    return mods;
}

// ─── Annotation emission ──────────────────────────────────────────────────────

void KotlinEmitter::emitAnnotations(const std::vector<std::string>& anns,
                                     CodeWriter& writer) {
    for (const auto& ann : anns)
        writer.writeLine("@" + ann);
}

// ─── Property emission ────────────────────────────────────────────────────────

void KotlinEmitter::emitProperty(const KtProperty& prop,
                                  bool inPrimaryConstructor,
                                  CodeWriter& writer) {
    emitAnnotations(prop.annotations, writer);

    std::string mods = propertyModifiers(prop);
    std::string keyword = prop.isVar ? "var" : "val";

    std::string line;
    if (!mods.empty()) line = mods + " ";
    if (!prop.receiverType.empty())
        line += prop.receiverType + ".";
    line += keyword + " " + prop.name + ": " + prop.type;
    if (!prop.initializer.empty())
        line += " = " + prop.initializer;
    if (prop.isDelegated)
        line += " by TODO()";

    writer.writeLine(line);
}

// ─── Function emission ────────────────────────────────────────────────────────

std::string KotlinEmitter::buildFunctionSignature(const KtFunction& fn) const {
    std::string sig;
    std::string mods = functionModifiers(fn);
    if (!mods.empty()) sig = mods + " ";
    sig += "fun";

    // Generic type params
    if (!fn.typeParams.empty()) {
        sig += " <";
        for (size_t i = 0; i < fn.typeParams.size(); ++i) {
            if (i) sig += ", ";
            sig += fn.typeParams[i];
        }
        sig += ">";
    }

    sig += " ";

    // Extension receiver
    if (!fn.receiverType.empty())
        sig += fn.receiverType + ".";

    sig += fn.name + "(";

    // Parameters
    for (size_t i = 0; i < fn.params.size(); ++i) {
        const auto& p = fn.params[i];
        if (i) sig += ", ";
        // Annotations inline (simplified)
        for (const auto& ann : p.annotations)
            sig += "@" + ann + " ";
        if (p.isCrossInline) sig += "crossinline ";
        if (p.isNoInline)    sig += "noinline ";
        if (p.isVarArg)      sig += "vararg ";
        sig += p.name + ": " + p.type;
        if (p.hasDefault)    sig += " = TODO()";
    }

    sig += ")";

    // Return type (omit Unit for non-suspend; show for suspend)
    if (fn.returnType != "Unit" || fn.isSuspend)
        sig += ": " + fn.returnType;

    return sig;
}

void KotlinEmitter::emitFunctionBody(
        const KtFunction& fn,
        const ReconstructResult& recon,
        CodeWriter& writer) {
    // For now, emit a stub body.  The full body would require adapting
    // JavaStmtEmitter to Kotlin syntax, which is a follow-on task.
    // We emit TODO() for abstract/native stubs and placeholder for others.
    (void)recon;
    (void)fn;
    writer.writeLine("TODO(\"Decompiled body\")");
}

void KotlinEmitter::emitFunction(
        const KtFunction& fn,
        const std::unordered_map<std::string, ReconstructResult>& recon,
        const KtClass& ownerClass,
        CodeWriter& writer) {
    (void)ownerClass;
    emitAnnotations(fn.annotations, writer);

    std::string sig = buildFunctionSignature(fn);

    if (fn.isAbstract || (fn.bcMethod && fn.bcMethod->isAbstract)) {
        writer.writeLine(sig);
        return;
    }

    if (!fn.bcMethod || fn.bcMethod->cfg.blocks().empty()) {
        writer.writeLine(sig + " { TODO() }");
        return;
    }

    writer.writeLine(sig + " {");
    writer.indent();

    // Look up the reconstruction result for this method.
    std::string key = fn.bcMethod->name + fn.bcMethod->descriptor.jvmDescriptor();
    auto it = recon.find(key);
    if (it != recon.end()) {
        emitFunctionBody(fn, it->second, writer);
    } else {
        writer.writeLine("TODO(\"Decompiled body\")");
    }

    writer.dedent();
    writer.writeLine("}");
}

// ─── Primary constructor ──────────────────────────────────────────────────────

void KotlinEmitter::emitPrimaryConstructor(const KtClass& cls,
                                            CodeWriter& writer) {
    if (cls.primaryCtorParams.empty()) return;

    // Already emitted inline with the class header.
    // This is called when the class header needs a separate constructor block.
    // For data/value classes the params are in the header, so nothing to do here.
}

// ─── Enum body ────────────────────────────────────────────────────────────────

void KotlinEmitter::emitEnum(const KtClass& cls, CodeWriter& writer) {
    if (!cls.enumEntries.empty()) {
        for (size_t i = 0; i < cls.enumEntries.size(); ++i) {
            std::string line = cls.enumEntries[i];
            if (i + 1 < cls.enumEntries.size()) line += ",";
            writer.writeLine(line);
        }
        // Semicolon separator before methods (if any)
        bool hasMethods = !cls.functions.empty() || !cls.properties.empty();
        if (hasMethods) writer.writeLine(";");
        writer.writeLine();
    }
}

// ─── Sealed class body ────────────────────────────────────────────────────────

void KotlinEmitter::emitSealed(
        const KtClass& cls,
        const std::unordered_map<std::string, ReconstructResult>& recon,
        CodeWriter& writer) {
    // Emit sealed subclass stubs if we don't have full definitions.
    for (const auto& sub : cls.sealedSubclasses) {
        writer.writeLine();
        writer.writeLine("// Sealed subclass: " + sub);
        writer.writeLine("class " + sub + " : " + cls.name + "()");
    }
    // Then regular body
    emitBody(cls, recon, writer);
}

// ─── Class body ───────────────────────────────────────────────────────────────

void KotlinEmitter::emitBody(
        const KtClass& cls,
        const std::unordered_map<std::string, ReconstructResult>& recon,
        CodeWriter& writer) {
    bool first = true;

    // Non-primary-ctor properties
    for (const auto& prop : cls.properties) {
        if (prop.isPrimaryCtorParam) continue;
        if (first) { first = false; } else writer.writeLine();
        emitProperty(prop, false, writer);
    }

    // Companion object
    if (cls.companion != nullptr) {
        if (!first) writer.writeLine();
        first = false;
        const auto& comp = *cls.companion;
        std::string header = "companion object";
        if (!comp.name.empty() && comp.name != "Companion")
            header += " " + comp.name;
        writer.writeLine(header + " {");
        writer.indent();
        emitBody(comp, recon, writer);
        writer.dedent();
        writer.writeLine("}");
    }

    // Functions
    for (const auto& fn : cls.functions) {
        if (!first) writer.writeLine();
        first = false;
        emitFunction(fn, recon, cls, writer);
    }
}

// ─── Class header ─────────────────────────────────────────────────────────────

void KotlinEmitter::emitClassHeader(const KtClass& cls, CodeWriter& writer) {
    emitAnnotations(cls.annotations, writer);

    // @JvmInline for value classes
    if (cls.kind == KtClassKind::ValueClass)
        writer.writeLine("@JvmInline");

    std::string mods = classModifiers(cls);
    if (!mods.empty()) mods += " ";

    std::string keyword;
    switch (cls.kind) {
        case KtClassKind::Interface:
        case KtClassKind::FunInterface:
        case KtClassKind::SealedInterface: keyword = "interface"; break;
        case KtClassKind::ObjectDecl:      keyword = "object";    break;
        case KtClassKind::CompanionObject: keyword = "object";    break;
        case KtClassKind::Enum:            keyword = "enum class"; break;
        case KtClassKind::Annotation:      keyword = "annotation class"; break;
        default:                           keyword = "class";     break;
    }

    std::string header = mods + keyword + " " + cls.name;

    // Generic type parameters
    if (!cls.typeParams.empty()) {
        header += "<";
        for (size_t i = 0; i < cls.typeParams.size(); ++i) {
            if (i) header += ", ";
            header += cls.typeParams[i];
        }
        header += ">";
    }

    // Primary constructor parameters (for data / value classes and classes
    // with primary constructors)
    if (!cls.primaryCtorParams.empty()) {
        header += "(";
        for (size_t i = 0; i < cls.primaryCtorParams.size(); ++i) {
            const auto& p = cls.primaryCtorParams[i];
            if (i) header += ", ";
            std::string pmods = propertyModifiers(p);
            if (!pmods.empty()) header += pmods + " ";
            header += (p.isVar ? "var " : "val ") + p.name + ": " + p.type;
        }
        header += ")";
    }

    // Inheritance
    bool hasSuperType = cls.superClass.has_value() || !cls.interfaces.empty();
    if (hasSuperType) {
        header += " : ";
        if (cls.superClass.has_value()) {
            header += cls.superClass.value();
            bool needsCallSite = (cls.kind != KtClassKind::Interface &&
                                  cls.kind != KtClassKind::ObjectDecl);
            if (needsCallSite) header += "()";
            if (!cls.interfaces.empty()) header += ", ";
        }
        for (size_t i = 0; i < cls.interfaces.size(); ++i) {
            if (i) header += ", ";
            header += cls.interfaces[i];
        }
    }

    writer.writeLine(header + " {");
}

// ─── Top-level class emitter ──────────────────────────────────────────────────

void KotlinEmitter::emitClass(
        const KtClass& ktClass,
        const std::unordered_map<std::string, ReconstructResult>& recon,
        CodeWriter& writer) {
    emitClassHeader(ktClass, writer);
    writer.indent();

    switch (ktClass.kind) {
        case KtClassKind::Enum:
            emitEnum(ktClass, writer);
            emitBody(ktClass, recon, writer);
            break;
        case KtClassKind::SealedClass:
        case KtClassKind::SealedInterface:
            emitSealed(ktClass, recon, writer);
            break;
        default:
            emitBody(ktClass, recon, writer);
            break;
    }

    writer.dedent();
    writer.writeLine("}");
}

} // namespace kotlin_emitter
} // namespace retdec
