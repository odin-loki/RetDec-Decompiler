/**
 * @file src/fsharp_emitter/fs_type_emitter.cpp
 */

#include "retdec/fsharp_emitter/fs_type_emitter.h"

#include <algorithm>
#include <sstream>

namespace retdec {
namespace fsharp_emitter {

FsTypeEmitter::FsTypeEmitter(FsWriter& writer, Options opts)
    : writer_(writer), opts_(std::move(opts)) {}

// ─── typeStr ─────────────────────────────────────────────────────────────────

std::string FsTypeEmitter::typeStr(const BcType& t) const {
    // Map CLR types to F# equivalents
    std::string s = t.toString();
    // Well-known CLR → F# mappings
    static const std::pair<std::string,std::string> kMap[] = {
        {"System.Void",    "unit"},
        {"System.Boolean", "bool"},
        {"System.Byte",    "byte"},
        {"System.SByte",   "sbyte"},
        {"System.Int16",   "int16"},
        {"System.UInt16",  "uint16"},
        {"System.Int32",   "int"},
        {"System.UInt32",  "uint32"},
        {"System.Int64",   "int64"},
        {"System.UInt64",  "uint64"},
        {"System.Single",  "float32"},
        {"System.Double",  "float"},
        {"System.Decimal", "decimal"},
        {"System.Char",    "char"},
        {"System.String",  "string"},
        {"System.Object",  "obj"},
        {"void",           "unit"},
        {"boolean",        "bool"},
        {"byte",           "byte"},
        {"short",          "int16"},
        {"int",            "int"},
        {"long",           "int64"},
        {"float",          "float32"},
        {"double",         "float"},
        {"char",           "char"},
        {"String",         "string"},
        {"Object",         "obj"},
    };
    for (const auto& [clr, fs] : kMap) {
        if (s == clr) return fs;
    }
    // Array handling: T[] → T[]
    if (s.size() > 2 && s.substr(s.size()-2) == "[]")
        return typeStr(BcType()) + "[]"; // fallback; ideally recurse
    // Generic: List<T> → System.Collections.Generic.List<'T> simplified
    return FsWriter::safeName(s);
}

// ─── accessStr ───────────────────────────────────────────────────────────────

std::string FsTypeEmitter::accessStr(BcAccess acc) const {
    if (hasFlag(acc, BcAccess::Private))   return "private ";
    if (hasFlag(acc, BcAccess::Internal))  return "internal ";
    if (hasFlag(acc, BcAccess::Protected)) return "protected ";
    if (hasFlag(acc, BcAccess::Public))    return "public ";
    return "";
}

// ─── Detection helpers ────────────────────────────────────────────────────────

bool FsTypeEmitter::isStaticClass(const BcClass& cls) const {
    return hasFlag(cls.access, BcAccess::Static) ||
           (cls.methods.size() > 0 &&
            std::all_of(cls.methods.begin(), cls.methods.end(),
                [](const BcMethod& m){ return hasFlag(m.access, BcAccess::Static); }) &&
            cls.fields.empty());
}

bool FsTypeEmitter::isDelegate(const BcClass& cls) const {
    return cls.methods.size() == 1 && cls.methods[0].name == "Invoke" &&
           cls.superClass.has_value() &&
           cls.superClass->toString().find("Delegate") != std::string::npos;
}

bool FsTypeEmitter::isSimpleRecord(const BcClass& cls) const {
    if (!opts_.preferRecords) return false;
    if (cls.isInterface || cls.isEnum || cls.isAbstract) return false;
    // A simple record: no methods except constructor, all fields are public
    size_t nonCtorMethods = std::count_if(
        cls.methods.begin(), cls.methods.end(),
        [](const BcMethod& m){ return !m.isConstructor && !m.isStaticInit; });
    return nonCtorMethods == 0 && !cls.fields.empty();
}

bool FsTypeEmitter::isCompilerGenerated(const BcClass& cls) const {
    if (!opts_.omitCompilerGenerated) return false;
    const std::string& n = cls.name;
    return !n.empty() && (n[0] == '<' || n.find("__") == 0 ||
                           n.find("DisplayClass") != std::string::npos ||
                           n.find("d__") != std::string::npos);
}

bool FsTypeEmitter::isPropertyGetter(const BcMethod& m) const {
    return m.name.size() > 4 && m.name.substr(0, 4) == "get_";
}

bool FsTypeEmitter::isPropertySetter(const BcMethod& m) const {
    return m.name.size() > 4 && m.name.substr(0, 4) == "set_";
}

bool FsTypeEmitter::isEventAccessor(const BcMethod& m) const {
    return (m.name.size() > 4 &&
            (m.name.substr(0, 4) == "add_" || m.name.substr(0, 7) == "remove_"));
}

// ─── collectProperties ───────────────────────────────────────────────────────

std::vector<FsTypeEmitter::PropGroup> FsTypeEmitter::collectProperties(
        const BcClass& cls) const {
    std::vector<PropGroup> props;
    for (const auto& m : cls.methods) {
        if (!isPropertyGetter(m)) continue;
        std::string propName = m.name.substr(4);
        PropGroup pg;
        pg.name     = propName;
        pg.type     = m.descriptor.returnType ? *m.descriptor.returnType : BcType{};
        pg.getter   = &m;
        pg.isStatic = hasFlag(m.access, BcAccess::Static);
        // find setter
        for (const auto& m2 : cls.methods) {
            if (m2.name == "set_" + propName) {
                pg.setter = const_cast<BcMethod*>(&m2);
                break;
            }
        }
        props.push_back(std::move(pg));
    }
    return props;
}

// ─── methodParams ────────────────────────────────────────────────────────────

std::string FsTypeEmitter::methodParams(const BcMethod& m) const {
    const auto& pts = m.descriptor.params;
    if (pts.empty()) return "()";
    std::ostringstream ss;
    ss << "(";
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i) ss << ", ";
        std::string pname = (i < m.paramNames.size()) ? m.paramNames[i]
                                                       : "arg" + std::to_string(i);
        ss << FsWriter::safeName(pname) << ": " << typeStr(pts[i] ? *pts[i] : BcType{});
    }
    ss << ")";
    return ss.str();
}

std::string FsTypeEmitter::methodReturnType(const BcMethod& m) const {
    return typeStr(m.descriptor.returnType ? *m.descriptor.returnType : BcType{});
}

std::string FsTypeEmitter::memberModifiers(const BcMethod& m,
                                             bool inInterface) const {
    std::string s;
    if (!inInterface) {
        if (hasFlag(m.access, BcAccess::Static))   s += "static ";
        if (m.isAbstract)                           s += "abstract ";
        if (hasFlag(m.access, BcAccess::Override))  s += "override ";
        if (hasFlag(m.access, BcAccess::Virtual) && !m.isAbstract) s += "virtual ";
    }
    return s;
}

std::string FsTypeEmitter::fieldModifiers(const BcField& f) const {
    std::string s;
    if (hasFlag(f.access, BcAccess::Static)) s += "static ";
    if (!hasFlag(f.access, BcAccess::Readonly) &&
        !hasFlag(f.access, BcAccess::Final)) {
        // mutable is default in classes; we add it explicitly here
    }
    return s;
}

// ─── emitRecord ──────────────────────────────────────────────────────────────

void FsTypeEmitter::emitRecord(const BcClass& cls) {
    writer_.write("type " + FsWriter::safeName(cls.name) + " = { ");
    bool first = true;
    for (const auto& f : cls.fields) {
        if (!first) writer_.write("; ");
        writer_.write(FsWriter::safeName(f.name) + ": " + typeStr(f.type));
        first = false;
    }
    writer_.write(" }");
    writer_.nl();
}

// ─── emitDU (discriminated union for enums) ──────────────────────────────────

void FsTypeEmitter::emitDU(const BcClass& cls) {
    writer_.line("type " + FsWriter::safeName(cls.name) + " =");
    writer_.indent();
    if (cls.enumConstants.empty()) {
        // Fallback: emit fields as cases
        for (const auto& f : cls.fields) {
            if (f.constantIntValue.has_value())
                writer_.line("| " + FsWriter::safeName(f.name) +
                             " = " + std::to_string(*f.constantIntValue));
            else
                writer_.line("| " + FsWriter::safeName(f.name));
        }
    } else {
        int64_t val = 0;
        for (const auto& c : cls.enumConstants) {
            writer_.line("| " + FsWriter::safeName(c) + " = " + std::to_string(val++));
        }
    }
    writer_.dedent();
}

// ─── emitDelegate ────────────────────────────────────────────────────────────

void FsTypeEmitter::emitDelegate(const BcClass& cls) {
    const BcMethod& invoke = cls.methods[0];
    std::string paramStr;
    const auto& pts = invoke.descriptor.params;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i) paramStr += " * ";
        paramStr += typeStr(pts[i] ? *pts[i] : BcType{});
    }
    if (paramStr.empty()) paramStr = "unit";
    writer_.line("type " + FsWriter::safeName(cls.name) + " =");
    writer_.indent();
    writer_.line("delegate of " + paramStr + " -> " +
                 typeStr(invoke.descriptor.returnType ? *invoke.descriptor.returnType : BcType{}));
    writer_.dedent();
}

// ─── emitModule (static class → F# module) ──────────────────────────────────

void FsTypeEmitter::emitModule(const BcClass& cls, const BcModule& /*module*/) {
    writer_.line("module " + FsWriter::safeName(cls.name) + " =");
    writer_.indent();
    // Emit fields as let bindings
    for (const auto& f : cls.fields) {
        if (hasFlag(f.access, BcAccess::Static)) {
            std::string prefix = "let ";
            if (f.constantIntValue.has_value())
                writer_.line(prefix + FsWriter::safeName(f.name) +
                             ": " + typeStr(f.type) +
                             " = " + std::to_string(*f.constantIntValue));
            else if (f.constantStrValue.has_value())
                writer_.line(prefix + FsWriter::safeName(f.name) +
                             ": " + typeStr(f.type) +
                             " = " + writer_.strLiteral(*f.constantStrValue));
            else
                writer_.line(prefix + "mutable " + FsWriter::safeName(f.name) +
                             ": " + typeStr(f.type) + " = Unchecked.defaultof<_>");
        }
    }
    // Emit static methods as let bindings
    for (const auto& m : cls.methods) {
        if (m.isStaticInit) continue;
        if (isPropertyGetter(m) || isPropertySetter(m) || isEventAccessor(m)) continue;
        emitLetBinding(m);
        writer_.blank();
    }
    writer_.dedent();
}

// ─── emitLetBinding ──────────────────────────────────────────────────────────

void FsTypeEmitter::emitLetBinding(const BcMethod& m) {
    std::string name = FsWriter::safeName(m.name);
    std::string params = methodParams(m);
    std::string retType = methodReturnType(m);
    bool isVoid = (retType == "unit");

    writer_.line("let " + name + " " + params + ": " + retType + " =");
    writer_.indent();
    if (m.cfg.blockCount() == 0) {
        writer_.line("failwith \"" + name + " not implemented\"");
    } else {
        writer_.line("failwith \"" + name + " (decompiled)\"");
    }
    writer_.dedent();
    (void)isVoid;
}

// ─── emitInterfaceType ───────────────────────────────────────────────────────

void FsTypeEmitter::emitInterfaceType(const BcClass& cls) {
    // Emit attribute if needed
    writer_.line("[<Interface>]");
    std::string hdr = "type " + FsWriter::safeName(cls.name);
    if (!cls.typeParams.empty()) {
        hdr += "<";
        for (size_t i = 0; i < cls.typeParams.size(); ++i) {
            if (i) hdr += ", ";
            hdr += "'" + FsWriter::safeName(cls.typeParams[i]);
        }
        hdr += ">";
    }
    writer_.line(hdr + " =");
    writer_.indent();

    // Interfaces that inherit other interfaces
    for (const auto& iface : cls.interfaces) {
        writer_.line("inherit " + typeStr(iface));
    }

    // Abstract members
    for (const auto& m : cls.methods) {
        if (m.isStaticInit) continue;
        emitAbstractMember(m);
    }
    if (cls.methods.empty() && cls.interfaces.empty())
        writer_.line("interface end");

    writer_.dedent();
}

// ─── emitAbstractMember ──────────────────────────────────────────────────────

void FsTypeEmitter::emitAbstractMember(const BcMethod& m) {
    if (isPropertyGetter(m)) {
        std::string propName = m.name.substr(4);
        writer_.line("abstract member " + FsWriter::safeName(propName) +
                     ": " + typeStr(m.descriptor.returnType ? *m.descriptor.returnType : BcType{}) + " with get");
        return;
    }
    if (isPropertySetter(m)) return; // merged with getter

    std::string params;
    const auto& pts = m.descriptor.params;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i) params += " * ";
        params += typeStr(pts[i] ? *pts[i] : BcType{});
    }
    if (params.empty()) params = "unit";
    writer_.line("abstract member " + FsWriter::safeName(m.name) +
                 ": " + params + " -> " + typeStr(m.descriptor.returnType ? *m.descriptor.returnType : BcType{}));
}

// ─── emitClassType ───────────────────────────────────────────────────────────

void FsTypeEmitter::emitClassType(const BcClass& cls, const BcModule& /*module*/) {
    // Attributes
    if (cls.isAbstract && !cls.isInterface)
        writer_.line("[<AbstractClass>]");

    // Type header
    std::string hdr = "type " + FsWriter::safeName(cls.name);
    if (!cls.typeParams.empty()) {
        hdr += "<";
        for (size_t i = 0; i < cls.typeParams.size(); ++i) {
            if (i) hdr += ", ";
            hdr += "'" + FsWriter::safeName(cls.typeParams[i]);
        }
        hdr += ">";
    }

    // Default constructor (if no explicit ctor or simple ctor)
    const BcMethod* defaultCtor = nullptr;
    for (const auto& m : cls.methods) {
        if (m.isConstructor && m.descriptor.params.empty()) {
            defaultCtor = &m;
            break;
        }
    }

    if (defaultCtor || cls.methods.empty()) {
        hdr += "()";
    }

    // Inheritance
    if (cls.superClass.has_value()) {
        std::string base = typeStr(*cls.superClass);
        if (base != "obj" && base != "System.Object" && base != "Object")
            hdr += " =\n    inherit " + base + "()";
        else
            hdr += " =";
    } else {
        hdr += " =";
    }
    writer_.line(hdr);
    writer_.indent();

    // Fields as mutable val bindings
    for (const auto& f : cls.fields) {
        emitField(f, false);
    }

    // Properties
    auto props = collectProperties(cls);
    for (const auto& pg : props) {
        std::string pfx = pg.isStatic ? "static member " : "member ";
        std::string self = pg.isStatic ? "" : "this.";
        if (pg.getter && pg.setter) {
            writer_.line(pfx + self + FsWriter::safeName(pg.name) +
                         ": " + typeStr(pg.type));
            writer_.indent();
            writer_.line("with get() = failwith \"" + pg.name + " getter\"");
            writer_.line("and set(value: " + typeStr(pg.type) + ") = failwith \"" +
                         pg.name + " setter\"");
            writer_.dedent();
        } else if (pg.getter) {
            writer_.line(pfx + self + FsWriter::safeName(pg.name) +
                         ": " + typeStr(pg.type) +
                         " = failwith \"" + pg.name + " getter\"");
        }
    }

    // Methods
    for (const auto& m : cls.methods) {
        if (m.isStaticInit) continue;
        if (isPropertyGetter(m) || isPropertySetter(m) || isEventAccessor(m)) continue;
        if (m.isConstructor) {
            emitConstructor(cls, m);
        } else {
            emitMember(cls, m);
        }
        writer_.blank();
    }

    if (cls.fields.empty() && cls.methods.empty())
        writer_.line("class end");

    writer_.dedent();
}

// ─── emitField ───────────────────────────────────────────────────────────────

void FsTypeEmitter::emitField(const BcField& f, bool isLet) {
    std::string acc = accessStr(f.access);
    if (isLet) {
        // Module-level
        writer_.line("let mutable " + FsWriter::safeName(f.name) +
                     ": " + typeStr(f.type) + " = Unchecked.defaultof<_>");
    } else {
        // Class member: val [mutable] name: type
        std::string line = acc;
        if (!hasFlag(f.access, BcAccess::Readonly) &&
            !hasFlag(f.access, BcAccess::Final))
            line += "[<DefaultValue>] val mutable ";
        else
            line += "[<DefaultValue>] val ";
        if (hasFlag(f.access, BcAccess::Static)) line += "static ";
        line += FsWriter::safeName(f.name) + ": " + typeStr(f.type);
        writer_.line(line);
    }
}

// ─── emitConstructor ─────────────────────────────────────────────────────────

void FsTypeEmitter::emitConstructor(const BcClass& /*cls*/, const BcMethod& m) {
    std::string params = methodParams(m);
    writer_.line("new" + params + " =");
    writer_.indent();
    writer_.line("{ }  // constructor body");
    writer_.dedent();
}

// ─── emitMember ──────────────────────────────────────────────────────────────

void FsTypeEmitter::emitMember(const BcClass& /*cls*/, const BcMethod& m) {
    bool inInterface = false;
    std::string mods = memberModifiers(m, inInterface);
    std::string acc  = accessStr(m.access);
    std::string prefix = acc + mods;

    std::string self = hasFlag(m.access, BcAccess::Static) ? "" : "this.";
    std::string keyword = hasFlag(m.access, BcAccess::Static) ? "static member " : "member ";

    std::string name   = FsWriter::safeName(m.name);
    std::string params = methodParams(m);
    std::string retT   = methodReturnType(m);

    if (m.isAbstract) {
        // already handled in interface; for abstract class emit default
        writer_.line(prefix + keyword + self + name + params + ": " + retT + " =");
        writer_.indent();
        writer_.line("raise (System.NotImplementedException())");
        writer_.dedent();
    } else {
        writer_.line(prefix + keyword + self + name + params + ": " + retT + " =");
        writer_.indent();
        if (m.cfg.blockCount() == 0) {
            if (retT == "unit")
                writer_.line("()");
            else
                writer_.line("failwith \"" + name + " (decompiled)\"");
        } else {
            writer_.line("failwith \"" + name + " (decompiled)\"");
        }
        writer_.dedent();
    }
}

// ─── emitClass (top-level dispatch) ──────────────────────────────────────────

void FsTypeEmitter::emitClass(const BcClass& cls, const BcModule& module) {
    if (isCompilerGenerated(cls)) return;

    if (cls.isEnum) {
        emitDU(cls);
        return;
    }
    if (isDelegate(cls)) {
        emitDelegate(cls);
        return;
    }
    if (isSimpleRecord(cls)) {
        emitRecord(cls);
        return;
    }
    if (opts_.preferModules && isStaticClass(cls)) {
        emitModule(cls, module);
        return;
    }
    if (cls.isInterface) {
        emitInterfaceType(cls);
        return;
    }
    emitClassType(cls, module);
}

} // namespace fsharp_emitter
} // namespace retdec
