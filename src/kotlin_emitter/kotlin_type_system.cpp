/**
 * @file src/kotlin_emitter/kotlin_type_system.cpp
 * @brief Kotlin type reconstruction from BcClass + KotlinClassMetadata.
 */

#include <memory>
#include "retdec/kotlin_emitter/kotlin_type_system.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace retdec {
namespace kotlin_emitter {

// ─── JVM-to-Kotlin name mapping ───────────────────────────────────────────────

static const std::unordered_map<std::string, std::string> kJvmToKotlin = {
    // Primitives via boxed/kotlin names
    {"kotlin/Any",           "Any"},
    {"kotlin/Nothing",       "Nothing"},
    {"kotlin/Unit",          "Unit"},
    {"kotlin/Boolean",       "Boolean"},
    {"kotlin/Char",          "Char"},
    {"kotlin/Byte",          "Byte"},
    {"kotlin/Short",         "Short"},
    {"kotlin/Int",           "Int"},
    {"kotlin/Long",          "Long"},
    {"kotlin/Float",         "Float"},
    {"kotlin/Double",        "Double"},
    {"kotlin/String",        "String"},
    {"kotlin/Number",        "Number"},
    {"kotlin/CharSequence",  "CharSequence"},
    {"kotlin/Comparable",    "Comparable"},
    {"kotlin/Enum",          "Enum"},
    {"kotlin/Array",         "Array"},
    {"kotlin/BooleanArray",  "BooleanArray"},
    {"kotlin/ByteArray",     "ByteArray"},
    {"kotlin/CharArray",     "CharArray"},
    {"kotlin/ShortArray",    "ShortArray"},
    {"kotlin/IntArray",      "IntArray"},
    {"kotlin/LongArray",     "LongArray"},
    {"kotlin/FloatArray",    "FloatArray"},
    {"kotlin/DoubleArray",   "DoubleArray"},
    {"kotlin/Throwable",     "Throwable"},
    {"kotlin/Exception",     "Exception"},
    {"kotlin/Annotation",    "Annotation"},
    {"kotlin/Cloneable",     "Cloneable"},
    {"kotlin/Iterable",      "Iterable"},
    {"kotlin/Iterator",      "Iterator"},
    {"kotlin/Collection",    "Collection"},
    {"kotlin/List",          "List"},
    {"kotlin/Set",           "Set"},
    {"kotlin/Map",           "Map"},
    {"kotlin/Map/Entry",     "Map.Entry"},
    {"kotlin/MutableIterable",   "MutableIterable"},
    {"kotlin/MutableIterator",   "MutableIterator"},
    {"kotlin/MutableCollection", "MutableCollection"},
    {"kotlin/MutableList",       "MutableList"},
    {"kotlin/MutableSet",        "MutableSet"},
    {"kotlin/MutableMap",        "MutableMap"},
    {"kotlin/MutableMap/Entry",  "MutableMap.Entry"},
    // JVM types (mapped to Kotlin equivalents)
    {"java/lang/Object",        "Any"},
    {"java/lang/String",        "String"},
    {"java/lang/Integer",       "Int"},
    {"java/lang/Long",          "Long"},
    {"java/lang/Short",         "Short"},
    {"java/lang/Byte",          "Byte"},
    {"java/lang/Double",        "Double"},
    {"java/lang/Float",         "Float"},
    {"java/lang/Boolean",       "Boolean"},
    {"java/lang/Character",     "Char"},
    {"java/lang/Number",        "Number"},
    {"java/lang/Comparable",    "Comparable"},
    {"java/lang/Iterable",      "Iterable"},
    {"java/lang/Runnable",      "Runnable"},
    {"java/lang/Throwable",     "Throwable"},
    {"java/lang/Exception",     "Exception"},
    {"java/lang/RuntimeException", "RuntimeException"},
    {"java/lang/CharSequence",  "CharSequence"},
    {"java/lang/Cloneable",     "Cloneable"},
    {"java/util/List",          "List"},
    {"java/util/ArrayList",     "ArrayList"},
    {"java/util/Set",           "Set"},
    {"java/util/HashSet",       "HashSet"},
    {"java/util/Map",           "Map"},
    {"java/util/HashMap",       "HashMap"},
    {"java/util/Collection",    "Collection"},
    {"java/util/Iterator",      "Iterator"},
    {"kotlin/coroutines/Continuation", "Continuation"},
};

// ─── KtTypeRenderer ───────────────────────────────────────────────────────────

KtTypeRenderer::KtTypeRenderer(const std::vector<std::string>& stringTable)
    : strings_(stringTable) {}

std::string KtTypeRenderer::kotlinName(const std::string& jvmName) const {
    if (jvmName.empty()) return "Any";
    // Slash → dot normalisation
    std::string dotName = jvmName;
    std::replace(dotName.begin(), dotName.end(), '/', '.');
    // Try Kotlin mapping first
    auto it = kJvmToKotlin.find(jvmName);
    if (it != kJvmToKotlin.end()) return it->second;
    // Return the simple name (last component after '.' or '/')
    auto pos = dotName.rfind('.');
    if (pos != std::string::npos) return dotName.substr(pos + 1);
    return dotName;
}

std::string KtTypeRenderer::render(const KotlinType& type) const {
    if (type.isFunctionType || type.isSuspendFunctionType) {
        // Render as (Params) -> Return or suspend (Params) -> Return
        std::string prefix = type.isSuspendFunctionType ? "suspend " : "";
        std::string params;
        for (size_t i = 0; i < type.funParams.size(); ++i) {
            if (i) params += ", ";
            params += (type.funParams[i] ? render(*type.funParams[i]) : "Any");
        }
        std::string ret = type.funReturn ? render(*type.funReturn) : "Unit";
        std::string base = prefix + "(" + params + ") -> " + ret;
        return type.nullable ? base + "?" : base;
    }

    if (type.typeParamIdx >= 0) {
        // Type parameter reference (T, E, K, etc.)
        // We don't have the param name here — use the class name if set
        std::string name = type.className.empty() ? "T" : type.className;
        return type.nullable ? name + "?" : name;
    }

    std::string base = kotlinName(type.className);

    if (!type.typeArgs.empty()) {
        base += "<";
        for (size_t i = 0; i < type.typeArgs.size(); ++i) {
            if (i) base += ", ";
            base += renderArg(type.typeArgs[i]);
        }
        base += ">";
    }

    return type.nullable ? base + "?" : base;
}

std::string KtTypeRenderer::render(const std::shared_ptr<KotlinType>& type) const {
    if (!type) return "Any";
    return render(*type);
}

std::string KtTypeRenderer::renderArg(const KotlinTypeArg& arg) const {
    if (arg.isStarProj) return "*";
    std::string prefix;
    if (arg.variance == KotlinTypeArg::Variance::In)  prefix = "in ";
    if (arg.variance == KotlinTypeArg::Variance::Out) prefix = "out ";
    return prefix + (arg.type ? render(*arg.type) : "*");
}

// ─── KtClassReconstructor ─────────────────────────────────────────────────────

KtClassKind KtClassReconstructor::classifyKind(const KotlinClassFlags& flags) {
    if (flags.isObject)    return KtClassKind::ObjectDecl;
    if (flags.isCompanion) return KtClassKind::CompanionObject;
    if (flags.isData)      return KtClassKind::DataClass;
    if (flags.isInline)    return KtClassKind::ValueClass;
    if (flags.isFunInterface) return KtClassKind::FunInterface;
    if (flags.isSealed || flags.modality == 3) {
        return KtClassKind::SealedClass;
    }
    if (flags.isInterface) return KtClassKind::Interface;
    if (flags.isEnum)      return KtClassKind::Enum;
    if (flags.isAnnotation)return KtClassKind::Annotation;
    return KtClassKind::Class;
}

KtVisibility KtClassReconstructor::toVisibility(int v) {
    switch (v) {
        case 0: case 1: return KtVisibility::Private;
        case 2:         return KtVisibility::Private;
        case 3:         return KtVisibility::Protected;
        case 4:         return KtVisibility::Internal;
        default:        return KtVisibility::Public;
    }
}

std::vector<std::string>
KtClassReconstructor::renderTypeParams(
        const std::vector<KotlinTypeParam>& params,
        const KtTypeRenderer& renderer) {
    std::vector<std::string> result;
    result.reserve(params.size());
    for (const auto& tp : params) {
        std::string s;
        if (tp.variance == 1) s = "in ";
        else if (tp.variance == 2) s = "out ";
        if (tp.isReified) s += "reified ";
        s += tp.name.empty() ? "T" : tp.name;
        if (!tp.upperBounds.empty()) {
            // Only show the first meaningful upper bound (not "Any")
            for (const auto& ub : tp.upperBounds) {
                if (!ub) continue;
                std::string bound = renderer.render(*ub);
                if (bound != "Any" && bound != "Any?") {
                    s += " : " + bound;
                    break;
                }
            }
        }
        result.push_back(std::move(s));
    }
    return result;
}

const BcMethod* KtClassReconstructor::findBcMethod(
        const BcClass& cls, const KotlinFunction& kfun) {
    // Try to match by JVM descriptor if available.
    if (!kfun.jvmDescriptor.empty()) {
        for (const auto& m : cls.methods) {
            if (m.name == kfun.jvmName && !kfun.jvmName.empty())
                return &m;
            if (m.descriptor.jvmDescriptor() == kfun.jvmDescriptor)
                return &m;
        }
    }
    // Fall back to name matching.
    const BcMethod* best = nullptr;
    for (const auto& m : cls.methods) {
        if (m.name == kfun.name) {
            best = &m;
            // Prefer methods whose parameter count matches (account for
            // suspend adding a Continuation param).
            size_t ktParams = kfun.valueParams.size();
            size_t jvmParams = m.descriptor.params.size();
            if (jvmParams == ktParams || jvmParams == ktParams + 1)
                return &m;
        }
    }
    return best;
}

KtFunction KtClassReconstructor::buildFunction(
        const KotlinFunction& kfun,
        const BcClass& cls,
        const KtTypeRenderer& renderer) {
    KtFunction fn;
    fn.name        = kfun.name;
    fn.returnType  = kfun.returnType ? renderer.render(*kfun.returnType) : "Unit";
    fn.receiverType= kfun.receiverType ? renderer.render(*kfun.receiverType) : "";
    fn.isOperator  = kfun.isOperator;
    fn.isInfix     = kfun.isInfix;
    fn.isInline    = kfun.isInline;
    fn.isTailrec   = kfun.isTailrec;
    fn.isSuspend   = kfun.isSuspend;
    fn.isExternal  = kfun.isExternal;
    fn.isExpect    = kfun.isExpect;
    fn.visibility  = toVisibility(kfun.visibility);
    fn.typeParams  = kfun.typeParams;
    fn.bcMethod    = findBcMethod(cls, kfun);

    // Build parameter list, skipping the Continuation<T> of suspend fns.
    for (const auto& vp : kfun.valueParams) {
        KtParam p;
        p.name       = vp.name;
        p.type       = vp.type ? renderer.render(*vp.type) : "Any";
        p.hasDefault = vp.hasDefault;
        p.isVarArg   = vp.isVarArg;
        p.isCrossInline = vp.isCrossInline;
        p.isNoInline    = vp.isNoInline;
        fn.params.push_back(std::move(p));
    }

    if (fn.isSuspend) removeContinuationParam(fn);

    return fn;
}

void KtClassReconstructor::removeContinuationParam(KtFunction& fn) {
    // The JVM adds a Continuation<T> as the last parameter of suspend fns.
    // Strip it from the Kotlin view.
    if (!fn.params.empty()) {
        const std::string& lastType = fn.params.back().type;
        if (lastType.find("Continuation") != std::string::npos)
            fn.params.pop_back();
    }
}

KtProperty KtClassReconstructor::buildProperty(
        const KotlinProperty& kprop,
        const KtTypeRenderer& renderer) {
    KtProperty prop;
    prop.name         = kprop.name;
    prop.type         = kprop.returnType ? renderer.render(*kprop.returnType) : "Any";
    prop.receiverType = kprop.receiverType ? renderer.render(*kprop.receiverType) : "";
    prop.isVar        = kprop.isVar;
    prop.isLateinit   = kprop.isLateinit;
    prop.isConst      = kprop.isConst;
    prop.isDelegated  = kprop.isDelegated;
    prop.visibility   = toVisibility(kprop.visibility);
    return prop;
}

std::vector<KtProperty>
KtClassReconstructor::extractPrimaryCtorParams(
        const std::vector<KotlinProperty>& props,
        bool isDataClass, bool isValueClass,
        const KtTypeRenderer& renderer) {
    std::vector<KtProperty> result;
    if (!isDataClass && !isValueClass) return result;

    for (const auto& p : props) {
        // Primary constructor params are properties without a receiver type.
        // For data/value classes all component properties go into the ctor.
        if (!p.receiverType) {
            KtProperty kp = buildProperty(p, renderer);
            kp.isPrimaryCtorParam = true;
            result.push_back(std::move(kp));
        }
    }
    return result;
}

KtClass KtClassReconstructor::reconstruct(
        const BcClass& cls,
        const KotlinClassMetadata& meta) {
    KtTypeRenderer renderer(meta.stringTable);
    KtClass kt;

    // Basic identification
    kt.fqName     = meta.fqName.empty() ? cls.fqName : meta.fqName;
    kt.name       = meta.name.empty()   ? cls.name   : meta.name;
    kt.sourceFile = meta.sourceFile.empty() ? cls.sourceFile : meta.sourceFile;
    kt.bcClass    = &cls;

    // Derive package from fqName (kotlin/ style)
    {
        std::string fq = kt.fqName;
        std::replace(fq.begin(), fq.end(), '/', '.');
        auto pos = fq.rfind('.');
        kt.packageName = (pos != std::string::npos) ? fq.substr(0, pos) : "";
        if (kt.name.empty())
            kt.name = (pos != std::string::npos) ? fq.substr(pos + 1) : fq;
    }

    // Class kind
    const auto& flags = meta.flags;
    kt.kind = classifyKind(flags);
    kt.visibility = toVisibility(flags.visibility);
    kt.isAbstract  = flags.isAbstract;
    kt.isOpen      = flags.isOpen;
    kt.isSealed    = flags.isSealed;
    kt.isExpect    = flags.isExpect;
    kt.isExternal  = flags.isExternal;
    kt.isInner     = flags.isInner;

    // Also detect interface/enum from BcClass if not in flags
    if (cls.isInterface && kt.kind == KtClassKind::Class)
        kt.kind = KtClassKind::Interface;
    if (cls.isEnum && kt.kind == KtClassKind::Class)
        kt.kind = KtClassKind::Enum;
    if (cls.isAnnotation && kt.kind == KtClassKind::Class)
        kt.kind = KtClassKind::Annotation;

    // Type parameters
    kt.typeParams = renderTypeParams(meta.typeParams, renderer);

    // Supertypes → superClass + interfaces
    for (const auto& st : meta.supertypes) {
        if (!st) continue;
        std::string rendered = renderer.render(*st);
        // First non-Any, non-Annotation, non-Enum is the superclass
        if (!kt.superClass.has_value() &&
            rendered != "Any" && rendered != "Enum" &&
            rendered != "Annotation") {
            // Heuristic: if the name ends with common interface names, it's
            // probably an interface.  Otherwise treat as superclass.
            bool isInterface = (rendered.find("able") != std::string::npos ||
                                rendered.find("Interface") != std::string::npos ||
                                rendered.find("Listener") != std::string::npos ||
                                rendered.find("Handler") != std::string::npos);
            if (isInterface)
                kt.interfaces.push_back(rendered);
            else
                kt.superClass = rendered;
        } else if (rendered != "Any") {
            kt.interfaces.push_back(rendered);
        }
    }

    // Properties
    bool isData  = (kt.kind == KtClassKind::DataClass);
    bool isValue = (kt.kind == KtClassKind::ValueClass);
    kt.primaryCtorParams = extractPrimaryCtorParams(
        meta.properties, isData, isValue, renderer);

    for (const auto& kprop : meta.properties) {
        KtProperty prop = buildProperty(kprop, renderer);
        kt.properties.push_back(std::move(prop));
    }

    // Functions
    for (const auto& kfun : meta.functions) {
        KtFunction fn = buildFunction(kfun, cls, renderer);
        kt.functions.push_back(std::move(fn));
    }

    // Enum entries
    for (const auto& entry : meta.enumEntries)
        kt.enumEntries.push_back(entry);
    // Fall back to BcClass enumConstants if metadata entries are empty
    if (kt.enumEntries.empty())
        kt.enumEntries = cls.enumConstants;

    // Sealed subclasses
    for (const auto& sub : meta.sealedSubclasses) {
        std::string subName = sub;
        auto pos = subName.rfind('/');
        if (pos != std::string::npos) subName = subName.substr(pos + 1);
        kt.sealedSubclasses.push_back(subName);
    }

    // Nested classes
    kt.nestedClasses = meta.nestedClasses;

    // Companion object placeholder (body filled by the file emitter
    // when processing the companion class itself).
    if (!meta.companionName.empty()) {
        KtClass companion;
        companion.name = meta.companionName;
        companion.kind = KtClassKind::CompanionObject;
        kt.companion   = std::make_unique<KtClass>(std::move(companion));
    }

    return kt;
}

} // namespace kotlin_emitter
} // namespace retdec
