/**
 * @file src/vbnet_emitter/vb_type_emitter.cpp
 */

#include "retdec/vbnet_emitter/vb_type_emitter.h"

#include <algorithm>
#include <sstream>

namespace retdec {
namespace vbnet_emitter {

VbTypeEmitter::VbTypeEmitter(VbWriter& writer, Options opts)
    : writer_(writer), opts_(std::move(opts)) {}

// ─── typeStr ─────────────────────────────────────────────────────────────────

std::string VbTypeEmitter::typeStr(const BcType& t) const {
    std::string s = t.toString();
    static const std::pair<std::string,std::string> kMap[] = {
        {"System.Void",    "Void"},
        {"System.Boolean", "Boolean"},
        {"System.Byte",    "Byte"},
        {"System.SByte",   "SByte"},
        {"System.Int16",   "Short"},
        {"System.UInt16",  "UShort"},
        {"System.Int32",   "Integer"},
        {"System.UInt32",  "UInteger"},
        {"System.Int64",   "Long"},
        {"System.UInt64",  "ULong"},
        {"System.Single",  "Single"},
        {"System.Double",  "Double"},
        {"System.Decimal", "Decimal"},
        {"System.Char",    "Char"},
        {"System.String",  "String"},
        {"System.Object",  "Object"},
        {"void",           "Void"},
        {"boolean",        "Boolean"},
        {"byte",           "Byte"},
        {"short",          "Short"},
        {"int",            "Integer"},
        {"long",           "Long"},
        {"float",          "Single"},
        {"double",         "Double"},
        {"char",           "Char"},
        {"String",         "String"},
        {"Object",         "Object"},
    };
    for (const auto& [clr, vb] : kMap)
        if (s == clr) return vb;
    return VbWriter::safeName(s);
}

// ─── accessStr ───────────────────────────────────────────────────────────────

std::string VbTypeEmitter::accessStr(BcAccess acc) const {
    if (hasFlag(acc, BcAccess::Private))   return "Private ";
    if (hasFlag(acc, BcAccess::Internal))  return "Friend ";
    if (hasFlag(acc, BcAccess::Protected)) return "Protected ";
    if (hasFlag(acc, BcAccess::Public))    return "Public ";
    return "Public ";  // default VB.NET is Public for class members
}

// ─── methodModifiers ─────────────────────────────────────────────────────────

std::string VbTypeEmitter::methodModifiers(const BcMethod& m) const {
    std::string s;
    if (hasFlag(m.access, BcAccess::Static))   s += "Shared ";
    if (m.isAbstract)                           s += "MustOverride ";
    if (hasFlag(m.access, BcAccess::Override))  s += "Overrides ";
    if (hasFlag(m.access, BcAccess::Virtual) &&
        !m.isAbstract &&
        !hasFlag(m.access, BcAccess::Override)) s += "Overridable ";
    if (hasFlag(m.access, BcAccess::Final) &&
        !hasFlag(m.access, BcAccess::Static))   s += "NotOverridable ";
    return s;
}

// ─── fieldModifiers ──────────────────────────────────────────────────────────

std::string VbTypeEmitter::fieldModifiers(const BcField& f) const {
    std::string s;
    if (hasFlag(f.access, BcAccess::Static))   s += "Shared ";
    if (hasFlag(f.access, BcAccess::Readonly) ||
        hasFlag(f.access, BcAccess::Final))     s += "ReadOnly ";
    return s;
}

// ─── isVoid ──────────────────────────────────────────────────────────────────

bool VbTypeEmitter::isVoid(const BcMethod& m) const {
    std::string rt = typeStr(m.descriptor.returnType ? *m.descriptor.returnType : BcType{});
    return rt == "Void" || rt == "void" || rt.empty();
}

// ─── paramList ───────────────────────────────────────────────────────────────

std::string VbTypeEmitter::paramList(const BcMethod& m) const {
    const auto& pts = m.descriptor.params;
    if (pts.empty()) return "()";
    std::ostringstream ss;
    ss << "(";
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i) ss << ", ";
        std::string pname = (i < m.paramNames.size()) ? m.paramNames[i]
                                                       : "arg" + std::to_string(i);
        ss << VbWriter::safeName(pname) << " As " << typeStr(pts[i] ? *pts[i] : BcType{});
    }
    ss << ")";
    return ss.str();
}

// ─── Detection ───────────────────────────────────────────────────────────────

bool VbTypeEmitter::isStaticClass(const BcClass& cls) const {
    return hasFlag(cls.access, BcAccess::Static) ||
           (cls.methods.size() > 0 &&
            std::all_of(cls.methods.begin(), cls.methods.end(),
                [](const BcMethod& m){ return hasFlag(m.access, BcAccess::Static); }) &&
            cls.fields.empty());
}

bool VbTypeEmitter::isDelegate(const BcClass& cls) const {
    return cls.methods.size() == 1 && cls.methods[0].name == "Invoke" &&
           cls.superClass.has_value() &&
           cls.superClass->toString().find("Delegate") != std::string::npos;
}

bool VbTypeEmitter::isCompilerGenerated(const BcClass& cls) const {
    if (!opts_.omitCompilerGenerated) return false;
    const std::string& n = cls.name;
    return !n.empty() && (n[0] == '<' || n.find("__") == 0 ||
                           n.find("d__") != std::string::npos);
}

bool VbTypeEmitter::isPropertyGetter(const BcMethod& m) const {
    return m.name.size() > 4 && m.name.substr(0, 4) == "get_";
}

bool VbTypeEmitter::isPropertySetter(const BcMethod& m) const {
    return m.name.size() > 4 && m.name.substr(0, 4) == "set_";
}

bool VbTypeEmitter::isEventAccessor(const BcMethod& m) const {
    return m.name.size() > 4 &&
           (m.name.substr(0, 4) == "add_" || m.name.substr(0, 7) == "remove_");
}

// ─── collectProperties ───────────────────────────────────────────────────────

std::vector<VbTypeEmitter::PropGroup> VbTypeEmitter::collectProperties(
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

// ─── emitField ───────────────────────────────────────────────────────────────

void VbTypeEmitter::emitField(const BcField& f) {
    std::string acc  = accessStr(f.access);
    std::string mods = fieldModifiers(f);
    std::string name = VbWriter::safeName(f.name);
    std::string type = typeStr(f.type);

    std::string line = acc + mods + "Dim " + name + " As " + type;

    if (f.constantIntValue.has_value())
        line += " = " + std::to_string(*f.constantIntValue);
    else if (f.constantFltValue.has_value())
        line += " = " + std::to_string(*f.constantFltValue);
    else if (f.constantStrValue.has_value())
        line += " = " + VbWriter::strLiteral(*f.constantStrValue);

    writer_.line(line);
}

// ─── emitProperty ────────────────────────────────────────────────────────────

void VbTypeEmitter::emitProperty(const std::string& propName, const BcType& propType,
                                   const BcMethod* getter, const BcMethod* setter,
                                   bool isStatic) {
    std::string shared = isStatic ? "Shared " : "";
    std::string ro = setter ? "" : "ReadOnly ";
    std::string type = typeStr(propType);

    writer_.line("Public " + shared + ro + "Property " +
                 VbWriter::safeName(propName) + " As " + type);
    writer_.indent();
    if (getter) {
        writer_.line("Get");
        writer_.indent();
        writer_.line("Throw New NotImplementedException()");
        writer_.dedent();
        writer_.line("End Get");
    }
    if (setter) {
        writer_.line("Set(value As " + type + ")");
        writer_.indent();
        writer_.line("Throw New NotImplementedException()");
        writer_.dedent();
        writer_.line("End Set");
    }
    writer_.dedent();
    writer_.line("End Property");
}

// ─── emitAbstractMethod ──────────────────────────────────────────────────────

void VbTypeEmitter::emitAbstractMethod(const BcMethod& m) {
    std::string acc  = accessStr(m.access);
    std::string name = VbWriter::safeName(m.name);
    std::string params = paramList(m);
    if (isVoid(m)) {
        writer_.line(acc + "MustOverride Sub " + name + params);
    } else {
        writer_.line(acc + "MustOverride Function " + name + params +
                     " As " + typeStr(m.descriptor.returnType ? *m.descriptor.returnType : BcType{}));
    }
}

// ─── emitMethod ──────────────────────────────────────────────────────────────

void VbTypeEmitter::emitMethod(const BcClass& /*cls*/, const BcMethod& m) {
    if (m.isAbstract) {
        emitAbstractMethod(m);
        return;
    }
    std::string acc  = accessStr(m.access);
    std::string mods = methodModifiers(m);
    std::string name = VbWriter::safeName(m.name);
    std::string params = paramList(m);
    bool isVoid_ = isVoid(m);

    if (isVoid_) {
        writer_.line(acc + mods + "Sub " + name + params);
    } else {
        writer_.line(acc + mods + "Function " + name + params +
                     " As " + typeStr(m.descriptor.returnType ? *m.descriptor.returnType : BcType{}));
    }
    writer_.indent();
    if (m.cfg.blockCount() == 0) {
        if (!isVoid_)
            writer_.line("Throw New NotImplementedException()");
    } else {
        if (!isVoid_)
            writer_.line("Throw New NotImplementedException()");
    }
    writer_.dedent();
    if (isVoid_)
        writer_.line("End Sub");
    else
        writer_.line("End Function");
}

// ─── emitConstructor ─────────────────────────────────────────────────────────

void VbTypeEmitter::emitConstructor(const BcClass& /*cls*/, const BcMethod& m) {
    std::string params = paramList(m);
    writer_.line("Public Sub New" + params);
    writer_.indent();
    writer_.line("MyBase.New()");
    writer_.dedent();
    writer_.line("End Sub");
}

// ─── emitModuleMethod ────────────────────────────────────────────────────────

void VbTypeEmitter::emitModuleMethod(const BcMethod& m) {
    std::string acc  = accessStr(m.access);
    std::string name = VbWriter::safeName(m.name);
    std::string params = paramList(m);
    bool isVoid_ = isVoid(m);

    if (isVoid_) {
        writer_.line(acc + "Sub " + name + params);
        writer_.indent();
        // body
        writer_.dedent();
        writer_.line("End Sub");
    } else {
        writer_.line(acc + "Function " + name + params +
                     " As " + typeStr(m.descriptor.returnType ? *m.descriptor.returnType : BcType{}));
        writer_.indent();
        writer_.line("Throw New NotImplementedException()");
        writer_.dedent();
        writer_.line("End Function");
    }
}

// ─── emitEnum ────────────────────────────────────────────────────────────────

void VbTypeEmitter::emitEnum(const BcClass& cls) {
    // Determine underlying type
    writer_.line("Public Enum " + VbWriter::safeName(cls.name));
    writer_.indent();
    if (!cls.enumConstants.empty()) {
        int64_t val = 0;
        for (const auto& c : cls.enumConstants) {
            writer_.line(VbWriter::safeName(c) + " = " + std::to_string(val++));
        }
    } else {
        for (const auto& f : cls.fields) {
            if (f.constantIntValue.has_value())
                writer_.line(VbWriter::safeName(f.name) + " = " +
                             std::to_string(*f.constantIntValue));
            else
                writer_.line(VbWriter::safeName(f.name));
        }
    }
    writer_.dedent();
    writer_.line("End Enum");
}

// ─── emitStructure ───────────────────────────────────────────────────────────

void VbTypeEmitter::emitStructure(const BcClass& cls) {
    writer_.line("Public Structure " + VbWriter::safeName(cls.name));
    writer_.indent();
    for (const auto& f : cls.fields) emitField(f);
    for (const auto& m : cls.methods) {
        if (m.isStaticInit) continue;
        if (m.isConstructor) { emitConstructor(cls, m); continue; }
        if (isPropertyGetter(m) || isPropertySetter(m) || isEventAccessor(m)) continue;
        emitMethod(cls, m);
        writer_.blank();
    }
    writer_.dedent();
    writer_.line("End Structure");
}

// ─── emitDelegate ────────────────────────────────────────────────────────────

void VbTypeEmitter::emitDelegate(const BcClass& cls) {
    const BcMethod& invoke = cls.methods[0];
    std::string params = paramList(invoke);
    if (isVoid(invoke)) {
        writer_.line("Public Delegate Sub " + VbWriter::safeName(cls.name) + params);
    } else {
        writer_.line("Public Delegate Function " + VbWriter::safeName(cls.name) +
                     params + " As " + typeStr(invoke.descriptor.returnType ? *invoke.descriptor.returnType : BcType{}));
    }
}

// ─── emitModule (static class → VB Module) ───────────────────────────────────

void VbTypeEmitter::emitModule(const BcClass& cls) {
    writer_.line("Module " + VbWriter::safeName(cls.name));
    writer_.indent();
    for (const auto& f : cls.fields) emitField(f);
    for (const auto& m : cls.methods) {
        if (m.isStaticInit) continue;
        if (isPropertyGetter(m) || isPropertySetter(m) || isEventAccessor(m)) continue;
        emitModuleMethod(m);
        writer_.blank();
    }
    writer_.dedent();
    writer_.line("End Module");
}

// ─── emitInterface ───────────────────────────────────────────────────────────

void VbTypeEmitter::emitInterface(const BcClass& cls) {
    std::string hdr = "Public Interface " + VbWriter::safeName(cls.name);
    if (!cls.typeParams.empty()) {
        hdr += "(Of ";
        for (size_t i = 0; i < cls.typeParams.size(); ++i) {
            if (i) hdr += ", ";
            hdr += VbWriter::safeName(cls.typeParams[i]);
        }
        hdr += ")";
    }
    writer_.line(hdr);
    writer_.indent();

    for (const auto& iface : cls.interfaces)
        writer_.line("Inherits " + typeStr(iface));

    for (const auto& m : cls.methods) {
        if (m.isStaticInit) continue;
        if (isPropertyGetter(m)) {
            std::string propName = m.name.substr(4);
            bool hasSet = false;
            for (const auto& m2 : cls.methods)
                if (m2.name == "set_" + propName) hasSet = true;
            std::string ro = hasSet ? "" : "ReadOnly ";
            writer_.line(ro + "Property " + VbWriter::safeName(propName) +
                         " As " + typeStr(m.descriptor.returnType ? *m.descriptor.returnType : BcType{}));
            continue;
        }
        if (isPropertySetter(m) || isEventAccessor(m)) continue;
        if (isVoid(m))
            writer_.line("Sub " + VbWriter::safeName(m.name) + paramList(m));
        else
            writer_.line("Function " + VbWriter::safeName(m.name) + paramList(m) +
                         " As " + typeStr(m.descriptor.returnType ? *m.descriptor.returnType : BcType{}));
    }
    writer_.dedent();
    writer_.line("End Interface");
}

// ─── emitRegularClass ────────────────────────────────────────────────────────

void VbTypeEmitter::emitRegularClass(const BcClass& cls) {
    std::string hdr;
    // Access
    if (hasFlag(cls.access, BcAccess::Public)) hdr += "Public ";
    else if (hasFlag(cls.access, BcAccess::Internal)) hdr += "Friend ";

    // Modifiers
    if (cls.isAbstract) hdr += "MustInherit ";
    if (hasFlag(cls.access, BcAccess::Sealed)) hdr += "NotInheritable ";

    hdr += "Class " + VbWriter::safeName(cls.name);

    // Generics
    if (!cls.typeParams.empty()) {
        hdr += "(Of ";
        for (size_t i = 0; i < cls.typeParams.size(); ++i) {
            if (i) hdr += ", ";
            hdr += VbWriter::safeName(cls.typeParams[i]);
        }
        hdr += ")";
    }
    writer_.line(hdr);
    writer_.indent();

    // Inheritance
    if (cls.superClass.has_value()) {
        std::string base = typeStr(*cls.superClass);
        if (base != "Object" && base != "System.Object")
            writer_.line("Inherits " + base);
    }
    for (const auto& iface : cls.interfaces)
        writer_.line("Implements " + typeStr(iface));

    if (cls.superClass.has_value() || !cls.interfaces.empty())
        writer_.blank();

    // Fields
    for (const auto& f : cls.fields) {
        emitField(f);
    }
    if (!cls.fields.empty()) writer_.blank();

    // Properties
    auto props = collectProperties(cls);
    for (const auto& pg : props) {
        emitProperty(pg.name, pg.type, pg.getter, pg.setter, pg.isStatic);
        writer_.blank();
    }

    // Methods
    for (const auto& m : cls.methods) {
        if (m.isStaticInit) continue;
        if (isPropertyGetter(m) || isPropertySetter(m) || isEventAccessor(m)) continue;
        if (m.isConstructor) {
            emitConstructor(cls, m);
        } else {
            emitMethod(cls, m);
        }
        writer_.blank();
    }

    writer_.dedent();
    writer_.line("End Class");
}

// ─── emitClass (dispatch) ────────────────────────────────────────────────────

void VbTypeEmitter::emitClass(const BcClass& cls, const BcModule& /*module*/) {
    if (isCompilerGenerated(cls)) return;

    if (cls.isEnum)         { emitEnum(cls);          return; }
    if (isDelegate(cls))    { emitDelegate(cls);       return; }
    if (isStaticClass(cls)) { emitModule(cls);         return; }
    if (cls.isInterface)    { emitInterface(cls);      return; }

    // Check for struct (value type)
    bool isStruct = hasFlag(cls.access, BcAccess::Readonly) ||
                    cls.name.find("Struct") != std::string::npos;
    if (isStruct) { emitStructure(cls); return; }

    emitRegularClass(cls);
}

} // namespace vbnet_emitter
} // namespace retdec
