/**
 * @file include/retdec/jvm_parser/jvm_attr.h
 * @brief JVM class-file attribute structs per JVMS §4.7.
 *
 * Covers all attributes relevant to decompilation:
 *   Code, ConstantValue, Deprecated, Exceptions, InnerClasses,
 *   EnclosingMethod, Signature, SourceFile, LineNumberTable,
 *   LocalVariableTable, LocalVariableTypeTable, BootstrapMethods,
 *   RuntimeVisibleAnnotations, RuntimeVisibleParameterAnnotations,
 *   RuntimeVisibleTypeAnnotations, Record (Java 16+),
 *   PermittedSubclasses (Java 17+), NestHost, NestMembers,
 *   MethodParameters, StackMapTable.
 */

#ifndef RETDEC_JVM_PARSER_JVM_ATTR_H
#define RETDEC_JVM_PARSER_JVM_ATTR_H

#include "retdec/jvm_parser/jvm_const_pool.h"
#include "retdec/bc_module/bc_module.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace retdec {
namespace jvm_parser {

using BcAnnotation = bc_module::BcAnnotation;

// ─── Exception table entry (Code attribute) ───────────────────────────────────

struct ExceptionEntry {
    uint16_t startPc   = 0;
    uint16_t endPc     = 0;
    uint16_t handlerPc = 0;
    uint16_t catchType = 0;  ///< cp index of Class, 0 = catch-all (finally)
};

// ─── Line number table entry ──────────────────────────────────────────────────

struct LineNumber {
    uint16_t startPc    = 0;
    int32_t  lineNumber = 0;
};

// ─── Local variable table entry ───────────────────────────────────────────────

struct LocalVarEntry {
    uint16_t startPc   = 0;
    uint16_t length    = 0;
    uint16_t nameIndex = 0;
    uint16_t descOrSigIndex = 0; ///< descriptor or generic signature
    uint16_t index     = 0;      ///< local variable slot
};

// ─── Annotation element values ────────────────────────────────────────────────

struct AnnotationElem;

struct AnnotationElemArray { std::vector<AnnotationElem> values; };

struct EnumConst {
    std::string typeName;
    std::string constName;
};

/// Thin wrapper for a class-info descriptor string in AnnotationValue.
struct ClassDescStr { std::string value; };

struct AnnotationValue {
    std::variant<
        int32_t,          // 'B','C','I','S','Z'
        int64_t,          // 'J'
        float,            // 'F'
        double,           // 'D'
        std::string,      // 's' (String constant value)
        EnumConst,        // 'e'
        ClassDescStr,     // 'c' (class info descriptor)
        BcAnnotation,     // '@'
        AnnotationElemArray // '['
    > value;
    char tag = 'I';
};

struct AnnotationElem {
    std::string  name;
    AnnotationValue elementValue;
};

struct RawAnnotation {
    std::string            typeName;  ///< from cp descriptor
    std::vector<AnnotationElem> elements;
};

// ─── Code attribute ───────────────────────────────────────────────────────────

struct CodeAttr {
    uint16_t maxStack     = 0;
    uint16_t maxLocals    = 0;
    std::vector<uint8_t>  bytecode;
    std::vector<ExceptionEntry> exceptionTable;
    std::vector<LineNumber>     lineNumbers;
    std::vector<LocalVarEntry>  localVarTable;
    std::vector<LocalVarEntry>  localVarTypeTable; ///< LVTT (generic signatures)
    // StackMapTable is parsed but not stored verbatim — used by verifier only.
};

// ─── BootstrapMethods attribute ───────────────────────────────────────────────

struct BootstrapMethod {
    uint16_t             methodRef = 0;   ///< cp index of MethodHandle
    std::vector<uint16_t> arguments;      ///< cp indices of static args
};

struct BootstrapMethodsAttr {
    std::vector<BootstrapMethod> methods;
};

// ─── InnerClasses attribute ───────────────────────────────────────────────────

struct InnerClassEntry {
    uint16_t innerClassInfo   = 0;  ///< cp Class index
    uint16_t outerClassInfo   = 0;  ///< cp Class index (0 = anonymous)
    uint16_t innerName        = 0;  ///< cp Utf8 index (0 = anonymous)
    uint16_t accessFlags      = 0;
};

struct InnerClassesAttr {
    std::vector<InnerClassEntry> classes;
};

// ─── EnclosingMethod attribute ────────────────────────────────────────────────

struct EnclosingMethodAttr {
    uint16_t classIndex  = 0;   ///< cp Class index
    uint16_t methodIndex = 0;   ///< cp NameAndType index (0 = class initialiser)
};

// ─── Record attribute (Java 16+) ─────────────────────────────────────────────

struct RecordComponent {
    uint16_t nameIndex       = 0;
    uint16_t descriptorIndex = 0;
    std::optional<std::string> signature;
    std::vector<RawAnnotation> annotations;
};

struct RecordAttr {
    std::vector<RecordComponent> components;
};

// ─── PermittedSubclasses attribute (Java 17+, sealed classes) ────────────────

struct PermittedSubclassesAttr {
    std::vector<uint16_t> classIndices;  ///< cp Class indices
};

// ─── NestHost / NestMembers (Java 11+) ───────────────────────────────────────

struct NestHostAttr   { uint16_t hostClassIndex = 0; };
struct NestMembersAttr { std::vector<uint16_t> memberIndices; };

// ─── MethodParameters (Java 8+) ──────────────────────────────────────────────

struct MethodParameter {
    std::string name;
    uint16_t    accessFlags = 0;
};

struct MethodParametersAttr {
    std::vector<MethodParameter> params;
};

// ─── Parsed class-file attribute (discriminated union) ───────────────────────

struct RawAttr {
    std::string          name;
    std::vector<uint8_t> data;
};

using ParsedAttr = std::variant<
    RawAttr,
    CodeAttr,
    BootstrapMethodsAttr,
    InnerClassesAttr,
    EnclosingMethodAttr,
    RecordAttr,
    PermittedSubclassesAttr,
    NestHostAttr,
    NestMembersAttr,
    MethodParametersAttr
>;

// ─── Attribute parser ─────────────────────────────────────────────────────────

/**
 * @brief Parses a single JVM attribute from a binary stream.
 *
 * @param r     Reader positioned at the attribute_name_index.
 * @param pool  Constant pool for name resolution.
 * @param context "class", "field", "method", or "code" for diagnostics.
 */
ParsedAttr parseAttribute(BinaryReader& r, const ConstPool& pool,
                          const std::string& context = "unknown");

/**
 * @brief Parse `count` attributes.
 */
std::vector<ParsedAttr> parseAttributes(BinaryReader& r, const ConstPool& pool,
                                        uint16_t count,
                                        const std::string& context = "unknown");

// ─── Attribute accessors ──────────────────────────────────────────────────────

const CodeAttr*                 getCode(const std::vector<ParsedAttr>& attrs);
const BootstrapMethodsAttr*     getBootstrap(const std::vector<ParsedAttr>& attrs);
const InnerClassesAttr*         getInnerClasses(const std::vector<ParsedAttr>& attrs);
const EnclosingMethodAttr*      getEnclosing(const std::vector<ParsedAttr>& attrs);
const RecordAttr*               getRecord(const std::vector<ParsedAttr>& attrs);
const PermittedSubclassesAttr*  getPermittedSubclasses(const std::vector<ParsedAttr>& attrs);
const NestHostAttr*             getNestHost(const std::vector<ParsedAttr>& attrs);
const MethodParametersAttr*     getMethodParameters(const std::vector<ParsedAttr>& attrs);

/// Return the raw Signature string, or "" if absent.
std::string getSignature(const std::vector<ParsedAttr>& attrs,
                         const ConstPool& pool);

/// Return the SourceFile string, or "" if absent.
std::string getSourceFile(const std::vector<ParsedAttr>& attrs,
                          const ConstPool& pool);

/// Collect all RuntimeVisible* annotations.
std::vector<RawAnnotation> getAnnotations(const std::vector<ParsedAttr>& attrs,
                                          const ConstPool& pool);

/// Collect per-parameter annotations from RuntimeVisibleParameterAnnotations.
std::vector<std::vector<RawAnnotation>>
getParamAnnotations(const std::vector<ParsedAttr>& attrs,
                    const ConstPool& pool);

} // namespace jvm_parser
} // namespace retdec

#endif // RETDEC_JVM_PARSER_JVM_ATTR_H
