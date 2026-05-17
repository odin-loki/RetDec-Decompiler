/**
 * @file src/java_emitter/java_class_emitter.cpp
 * @brief Java class/interface/enum/record/annotation emitter.
 */

#include "retdec/java_emitter/java_class_emitter.h"

#include <algorithm>
#include <sstream>

namespace retdec {
namespace java_emitter {

using namespace bc_module;
using namespace jvm_reconstruct;

// ─── JavaClassEmitter ─────────────────────────────────────────────────────────

JavaClassEmitter::JavaClassEmitter(
        ImportSet& imports,
        JavaTypePrinter& tyPrinter,
        const std::unordered_map<std::string, ReconstructResult>* reconMap,
        const ClassEmitOptions& opts)
    : imports_(imports), tyPrinter_(tyPrinter), reconMap_(reconMap), opts_(opts) {}

// ─── Annotation emission ─────────────────────────────────────────────────────

std::string JavaClassEmitter::emitAnnotationValue(const BcAnnotationValue& val) const {
    switch (val.kind) {
        case BcAnnotationValue::Kind::Int:    return std::to_string(val.intValue);
        case BcAnnotationValue::Kind::Float:  return std::to_string(val.floatValue);
        case BcAnnotationValue::Kind::Bool:   return val.boolValue ? "true" : "false";
        case BcAnnotationValue::Kind::String: return "\"" + val.stringValue + "\"";
        case BcAnnotationValue::Kind::Enum:
            return val.enumTypeName + "." + val.enumConstant;
        case BcAnnotationValue::Kind::Type: {
            // Class literal
            BcRefType ref; ref.kind = BcRefKind::Class;
            ref.className = val.stringValue;
            return tyPrinter_.print(BcType{ref}) + ".class";
        }
        case BcAnnotationValue::Kind::Array: {
            std::string out = "{";
            for (size_t i = 0; i < val.arrayValue.size(); ++i) {
                if (i) out += ", ";
                out += emitAnnotationValue(val.arrayValue[i]);
            }
            out += "}";
            return out;
        }
        case BcAnnotationValue::Kind::Annotation:
            return "@" + val.stringValue;
        default:
            return "/* ? */";
    }
}

std::string JavaClassEmitter::emitAnnotation(const BcAnnotation& ann) const {
    // Register the annotation type for import.
    BcRefType ref; ref.kind = BcRefKind::Class; ref.className = ann.typeName;
    std::string annType = tyPrinter_.print(BcType{ref});

    if (ann.elements.empty()) return "@" + annType;

    std::string out = "@" + annType + "(";
    bool first = true;
    for (const auto& kv : ann.elements) {
        if (!first) out += ", ";
        first = false;
        if (kv.first == "value" && ann.elements.size() == 1) {
            out += emitAnnotationValue(kv.second);
        } else {
            out += kv.first + " = " + emitAnnotationValue(kv.second);
        }
    }
    out += ")";
    return out;
}

void JavaClassEmitter::emitAnnotations(const std::vector<BcAnnotation>& anns,
                                        CodeWriter& writer) {
    for (const auto& ann : anns) {
        if (!ann.isVisible) continue; // Skip compile-time-only annotations.
        writer.writeLine(emitAnnotation(ann));
    }
}

// ─── Modifier emission ────────────────────────────────────────────────────────

std::string JavaClassEmitter::canonicalModifiers(BcAccess access,
                                                   bool isInterface,
                                                   bool isField,
                                                   bool isMethod) {
    std::string out;
    auto append = [&](const char* s) {
        if (!out.empty()) out += " ";
        out += s;
    };

    // javac canonical order.
    if (hasFlag(access, BcAccess::Public))    append("public");
    if (hasFlag(access, BcAccess::Protected)) append("protected");
    if (hasFlag(access, BcAccess::Private))   append("private");
    if (hasFlag(access, BcAccess::Abstract))  append("abstract");
    if (hasFlag(access, BcAccess::Static))    append("static");
    if (hasFlag(access, BcAccess::Final))     append("final");
    if (isField && hasFlag(access, BcAccess::Transient))  append("transient");
    if (isField && hasFlag(access, BcAccess::Volatile))   append("volatile");
    if (isMethod && hasFlag(access, BcAccess::Synchronized)) append("synchronized");
    if (isMethod && hasFlag(access, BcAccess::Native))    append("native");
    if (isMethod && hasFlag(access, BcAccess::Strict))    append("strictfp");
    if (hasFlag(access, BcAccess::Sealed))    append("sealed");

    return out;
}

std::string JavaClassEmitter::modifiersFor(BcAccess access, bool isInterface,
                                            bool isField, bool isMethod) const {
    return canonicalModifiers(access, isInterface, isField, isMethod);
}

// ─── Field emission ───────────────────────────────────────────────────────────

void JavaClassEmitter::emitField(const BcField& field, const BcClass& /*cls*/,
                                  CodeWriter& writer) {
    if (!opts_.emitSyntheticFields &&
        hasFlag(field.access, BcAccess::Synthetic)) return;

    emitAnnotations(field.annotations, writer);

    std::string mods = modifiersFor(field.access, false, true, false);
    std::string type = tyPrinter_.print(field.type);
    std::string decl = (mods.empty() ? "" : mods + " ") + type + " " + field.name;

    // Emit constant initializer for static final fields.
    if (field.constantStrValue)
        decl += " = \"" + *field.constantStrValue + "\"";
    else if (field.constantIntValue)
        decl += " = " + std::to_string(*field.constantIntValue);
    else if (field.constantFltValue) {
        std::ostringstream os;
        os << *field.constantFltValue;
        decl += " = " + os.str();
    }

    writer.writeLine(decl + ";");
}

// ─── Method emission ──────────────────────────────────────────────────────────

const ReconstructResult* JavaClassEmitter::reconFor(const BcMethod& method) const {
    if (!reconMap_) return nullptr;
    auto it = reconMap_->find(method.name);
    return (it != reconMap_->end()) ? &it->second : nullptr;
}

std::string JavaClassEmitter::buildParamList(const BcMethod& method,
                                              const ReconstructResult* recon) const {
    std::string out;
    const BcFuncType& desc = method.descriptor;

    size_t startIdx = hasFlag(method.access, BcAccess::Static) ? 0 : 1;

    for (size_t i = 0; i < desc.params.size(); ++i) {
        if (i) out += ", ";
        std::string ptype = desc.params[i] ? tyPrinter_.print(*desc.params[i])
                                           : "Object";

        // Get parameter name.
        std::string pname;
        if (recon && !recon->locals.locals.empty()) {
            // Look for param local at slot (startIdx + i).
            uint32_t slot = static_cast<uint32_t>(startIdx + i);
            for (const auto& lv : recon->locals.locals) {
                if (lv.isParam && lv.index == slot) {
                    pname = lv.name;
                    break;
                }
            }
        }
        if (pname.empty()) {
            if (i < method.paramNames.size())
                pname = method.paramNames[i];
            else
                pname = "p" + std::to_string(i);
        }

        out += ptype + " " + pname;
    }
    return out;
}

void JavaClassEmitter::emitMethodSignature(const BcMethod& method,
                                            const BcClass& cls,
                                            CodeWriter& writer) {
    emitAnnotations(method.annotations, writer);

    std::string mods = modifiersFor(method.access, false, false, true);

    // Generic type params.
    std::string typeParams;
    if (!method.typeParams.empty()) {
        typeParams = "<";
        for (size_t i = 0; i < method.typeParams.size(); ++i) {
            if (i) typeParams += ", ";
            typeParams += method.typeParams[i];
        }
        typeParams += "> ";
    }

    // Return type.
    std::string retType;
    if (method.isConstructor || method.isStaticInit) {
        retType = "";
    } else {
        retType = (method.descriptor.returnType
                   ? tyPrinter_.print(*method.descriptor.returnType)
                   : "void") + " ";
    }

    // Method name.
    std::string name = method.isConstructor ? cls.name : method.name;
    if (method.isStaticInit) name = "static";

    // Parameter list.
    const ReconstructResult* recon = reconFor(method);
    std::string params = method.isStaticInit ? ""
                                              : buildParamList(method, recon);

    // Throws clause.
    std::string throwsClause;
    if (!method.throwsList.empty()) {
        throwsClause = " throws ";
        for (size_t i = 0; i < method.throwsList.size(); ++i) {
            if (i) throwsClause += ", ";
            BcRefType ref; ref.kind = BcRefKind::Class;
            ref.className = method.throwsList[i];
            throwsClause += tyPrinter_.print(BcType{ref});
        }
    }

    // Build signature line.
    std::string sig;
    if (!mods.empty()) sig = mods + " ";
    sig += typeParams + retType;
    if (method.isStaticInit) {
        sig += "static";
    } else {
        sig += name + "(" + params + ")" + throwsClause;
    }

    // Abstract/native methods have no body.
    if (method.isAbstract || method.isNative) {
        writer.writeLine(sig + ";");
        return;
    }

    writer.writeLine(sig + " {");
}

void JavaClassEmitter::emitMethod(const BcMethod& method, const BcClass& cls,
                                   CodeWriter& writer) {
    // Skip bridge/synthetic if requested.
    if (!opts_.emitBridgeMethods && hasFlag(method.access, BcAccess::Bridge))
        return;
    if (hasFlag(method.access, BcAccess::Synthetic) &&
        !method.isStaticInit && !method.isConstructor)
        return;

    writer.writeLine();

    // Emit signature.
    emitMethodSignature(method, cls, writer);
    if (method.isAbstract || method.isNative) return;

    writer.indent();

    // Emit body.
    if (!method.cfg.blocks().empty()) {
        const ReconstructResult* recon = reconFor(method);
        if (recon) {
            JavaStmtEmitter stmtEmit(method, *recon, tyPrinter_,
                                      opts_.stmtOpts);
            // The body emitter includes the braces; strip them for indent.
            std::string body = stmtEmit.emitBody();
            // Output the body lines (without the outer braces).
            std::istringstream iss(body);
            std::string line;
            bool first = true, last = false;
            std::vector<std::string> lines;
            while (std::getline(iss, line)) lines.push_back(line);
            for (size_t i = 0; i < lines.size(); ++i) {
                // Skip first '{' and last '}'
                if (i == 0 && lines[i].find('{') != std::string::npos) continue;
                if (i == lines.size()-1 && lines[i].find('}') != std::string::npos) continue;
                // Remove one level of indent (4 spaces).
                std::string l = lines[i];
                if (l.size() >= 4 && l.substr(0, 4) == "    ")
                    l = l.substr(4);
                writer.writeLine(l);
            }
        } else {
            writer.writeLine("// Reconstruction data unavailable");
        }
    }

    writer.dedent();
    writer.writeLine("}");
}

// ─── Class header emission ────────────────────────────────────────────────────

void JavaClassEmitter::emitClassHeader(const BcClass& cls, CodeWriter& writer) {
    emitAnnotations(cls.annotations, writer);

    // Determine kind.
    std::string keyword;
    if (cls.isAnnotation) keyword = "@interface";
    else if (cls.isInterface) keyword = "interface";
    else if (cls.isEnum) keyword = "enum";
    else if (cls.isRecord && opts_.javaVersion >= 16) keyword = "record";
    else keyword = "class";

    std::string mods = modifiersFor(cls.access, cls.isInterface, false, false);

    // Generic type params.
    std::string typeParams;
    if (!cls.typeParams.empty()) {
        typeParams = "<";
        for (size_t i = 0; i < cls.typeParams.size(); ++i) {
            if (i) typeParams += ", ";
            typeParams += cls.typeParams[i];
        }
        typeParams += ">";
    }

    // Extends clause.
    std::string extendsClause;
    if (cls.superClass && !cls.isInterface) {
        std::string superName = tyPrinter_.print(*cls.superClass);
        if (superName != "Object" && superName != "java.lang.Object")
            extendsClause = " extends " + superName;
    }

    // Implements clause.
    std::string implementsClause;
    if (!cls.interfaces.empty()) {
        std::string kw = cls.isInterface ? " extends " : " implements ";
        implementsClause = kw;
        for (size_t i = 0; i < cls.interfaces.size(); ++i) {
            if (i) implementsClause += ", ";
            implementsClause += tyPrinter_.print(cls.interfaces[i]);
        }
    }

    std::string header = (mods.empty() ? "" : mods + " ") + keyword + " " +
                          cls.name + typeParams + extendsClause +
                          implementsClause + " {";
    writer.writeLine(header);
}

// ─── Enum constants ────────────────────────────────────────────────────────────

void JavaClassEmitter::emitEnumConstants(const BcClass& cls,
                                          CodeWriter& writer) {
    if (!cls.isEnum) return;
    // Enum constants are represented as static final fields in BcClass.
    bool first = true;
    bool anyConstants = false;
    for (const auto& f : cls.fields) {
        if (hasFlag(f.access, BcAccess::Static) &&
            hasFlag(f.access, BcAccess::Final)) {
            anyConstants = true;
            break;
        }
    }
    if (!anyConstants) return;

    std::string constants;
    for (const auto& f : cls.fields) {
        if (!hasFlag(f.access, BcAccess::Static) ||
            !hasFlag(f.access, BcAccess::Final)) continue;
        if (!first) constants += ",\n    ";
        first = false;
        constants += f.name;
    }
    if (!constants.empty())
        writer.writeLine("    " + constants + ";");
}

// ─── Record components ────────────────────────────────────────────────────────

void JavaClassEmitter::emitRecordComponents(const BcClass& cls,
                                             CodeWriter& writer) {
    if (!cls.isRecord) return;
    // Record components are the constructor parameters.
    for (const auto& m : cls.methods) {
        if (m.isConstructor) {
            // Emit as a record header after the class name.
            // (This requires a more sophisticated approach — placeholder.)
            (void)writer;
            break;
        }
    }
}

// ─── Fields emission ──────────────────────────────────────────────────────────

void JavaClassEmitter::emitFields(const BcClass& cls, CodeWriter& writer) {
    for (const auto& field : cls.fields) {
        if (cls.isEnum && hasFlag(field.access, BcAccess::Static) &&
            hasFlag(field.access, BcAccess::Final))
            continue; // Already emitted as enum constants.
        emitField(field, cls, writer);
    }
}

// ─── Methods emission ─────────────────────────────────────────────────────────

void JavaClassEmitter::emitMethods(const BcClass& cls, CodeWriter& writer) {
    for (const auto& method : cls.methods)
        emitMethod(method, cls, writer);
}

// ─── Inner classes ────────────────────────────────────────────────────────────

void JavaClassEmitter::emitInnerClasses(const BcClass& /*cls*/,
                                         CodeWriter& /*writer*/) {
    // Inner classes are separate BcClass entries in BcModule.
    // The file emitter handles them — this is a placeholder.
}

// ─── Main class emit ──────────────────────────────────────────────────────────

void JavaClassEmitter::emitClass(const BcClass& cls, CodeWriter& writer,
                                  int depth) {
    emitClassHeader(cls, writer);
    writer.indent();

    if (cls.isEnum)
        emitEnumConstants(cls, writer);

    emitFields(cls, writer);
    emitMethods(cls, writer);
    emitInnerClasses(cls, writer);

    writer.dedent();
    writer.writeLine("}");
}

std::string JavaClassEmitter::anonClassName(uint32_t idx) {
    return "Anonymous$" + std::to_string(idx);
}

} // namespace java_emitter
} // namespace retdec
