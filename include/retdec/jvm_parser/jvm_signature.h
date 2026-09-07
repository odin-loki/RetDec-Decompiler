/**
 * @file include/retdec/jvm_parser/jvm_signature.h
 * @brief JVM generic-signature parser per JVMS §4.7.9.1.
 *
 * Parses the JVM Signature attribute grammar into BcType values:
 *
 *   ClassSignature    → TypeParameters? SuperclassSignature SuperinterfaceSignature*
 *   MethodSignature   → TypeParameters? ( JavaTypeSignature* ) Result ThrowsSignature*
 *   FieldSignature    → ReferenceTypeSignature
 *
 *   TypeParameters    → '<' TypeParameter+ '>'
 *   TypeParameter     → Identifier ':' FieldTypeSignature? (':' InterfaceTypeSignature)*
 *
 *   ReferenceTypeSignature → ClassTypeSignature | TypeVariableSignature | ArrayTypeSignature
 *   ClassTypeSignature     → 'L' PackageSpecifier? SimpleClassTypeSignature
 *                                ClassTypeSignatureSuffix* ';'
 *   TypeVariableSignature  → 'T' Identifier ';'
 *   ArrayTypeSignature     → '[' JavaTypeSignature
 *   JavaTypeSignature      → ReferenceTypeSignature | BaseType
 *   BaseType               → 'B'|'C'|'D'|'F'|'I'|'J'|'S'|'Z'
 *   TypeArgument           → [WildcardIndicator] ReferenceTypeSignature | '*'
 *   WildcardIndicator      → '+' | '-'
 */

#ifndef RETDEC_JVM_PARSER_JVM_SIGNATURE_H
#define RETDEC_JVM_PARSER_JVM_SIGNATURE_H

#include "retdec/bc_module/bc_type.h"

#include <string>
#include <vector>

namespace retdec {
namespace jvm_parser {

using BcType = bc_module::BcType;

// ─── Parsed class-level type parameters ──────────────────────────────────────

struct JvmTypeParam
{
	std::string name;  ///< "T", "E", "K", "V"
	BcType classBound; ///< extends bound (may be Object if omitted)
	std::vector<BcType> interfaceBounds;
};

// ─── ParsedClassSignature ─────────────────────────────────────────────────────

struct ParsedClassSignature
{
	std::vector<JvmTypeParam> typeParams;
	BcType superclass;
	std::vector<BcType> interfaces;
};

// ─── ParsedMethodSignature ────────────────────────────────────────────────────

struct ParsedMethodSignature
{
	std::vector<JvmTypeParam> typeParams;
	std::vector<BcType> params;
	BcType returnType;
	std::vector<BcType> throwsTypes;
};

// ─── Parser ───────────────────────────────────────────────────────────────────

/**
 * @brief JVM generic-signature parser.
 *
 * All methods throw `JvmParseError` on malformed input.
 */
class JvmSignatureParser {
public:
	/// Deepest nesting the recursive-descent parser will accept.
	///
	/// A signature is a Utf8 constant, so the file bounds its *length* (65535
	/// bytes) but nothing in it bounds its *nesting*: "L<L<L<…" costs two
	/// bytes per level, so one well-formed-looking constant used to drive tens
	/// of thousands of recursive descents and exhaust the stack long before
	/// the grammar rejected it. There is no count in the file to bound this
	/// against, which is why it is a fixed limit and not a bounds:: check.
	/// 64 is far past any signature a compiler emits -- javac's own generics
	/// nest single digits -- and far short of what the stack can take.
	static constexpr unsigned MAX_SIGNATURE_DEPTH = 64;

	/// Parse a field/type generic signature → BcType.
	static BcType parseFieldSig(const std::string& sig);

	/// Parse a class generic signature (ClassSignature grammar).
	static ParsedClassSignature parseClassSig(const std::string& sig);

	/// Parse a method generic signature (MethodSignature grammar).
	static ParsedMethodSignature parseMethodSig(const std::string& sig);

	/// Parse a plain (non-generic) JVM descriptor into a BcType.
	/// e.g. "Ljava/lang/String;" → Class("java/lang/String")
	///      "I" → Int(),  "[I" → Array(Int())
	///      "(ILjava/lang/String;)V" → FuncType
	static BcType parseDescriptor(const std::string& desc);

	/// Parse a plain JVM method descriptor into param types + return type.
	static bc_module::BcFuncType parseMethodDescriptor(const std::string& desc);

private:
	struct Cursor
	{
		const std::string& s;
		size_t pos = 0;
		unsigned depth = 0; ///< current recursion depth, see MAX_SIGNATURE_DEPTH

		char peek() const
		{
			return pos < s.size() ? s[pos] : '\0';
		}
		char get();
		bool expect(char c);
		std::string identifier();
		bool atEnd() const
		{
			return pos >= s.size();
		}
	};

	static BcType parseJavaTypeSignature(Cursor& c);
	static BcType parseReferenceTypeSignature(Cursor& c);
	static BcType parseClassTypeSignature(Cursor& c);
	static BcType parseArrayTypeSignature(Cursor& c);
	static BcType parseTypeVariableSignature(Cursor& c);
	static BcType parseBaseType(char tag);
	static BcType parseTypeArgument(Cursor& c);
	static std::vector<JvmTypeParam> parseTypeParameters(Cursor& c);
	static JvmTypeParam parseTypeParameter(Cursor& c);
};

} // namespace jvm_parser
} // namespace retdec

#endif // RETDEC_JVM_PARSER_JVM_SIGNATURE_H
