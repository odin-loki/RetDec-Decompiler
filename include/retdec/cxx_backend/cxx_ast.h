/**
 * @file include/retdec/cxx_backend/cxx_ast.h
 * @brief C++ AST extension over the plain-C codegen AST.
 *
 * Adds C++-only node kinds on top of the existing CExpr / CStmt / CType
 * trees from retdec/codegen/codegen.h.  The new nodes cover:
 *
 *   Expressions:
 *     CxxNew       — `new T(args)` / `new T[n]`
 *     CxxDelete    — `delete p`    / `delete[] p`
 *     CxxCast      — `static_cast<T>(e)`, `reinterpret_cast`, `dynamic_cast`
 *     CxxThis      — the `this` pointer
 *     CxxThrow     — `throw e`
 *     CxxLambda    — `[captures](params) { body }` (best-effort recovery)
 *     CxxScope     — `Foo::bar` qualified name
 *     CxxRef       — reference-typed lvalue (tracking)
 *     CxxMethodCall — `obj.method(args)` / `obj->method(args)`
 *
 *   Statements:
 *     CxxTry       — `try { } catch (T e) { } ...`
 *     CxxCatch     — a single catch clause (child of CxxTry)
 *
 *   Declarations:
 *     CxxClass     — class or struct with members and methods
 *     CxxMethod    — member function (possibly virtual, override, const)
 *     CxxNamespace — namespace { ... }
 *     CxxTemplate  — template<typename T> (wraps a class or function)
 *     CxxEnum      — enum class Foo { ... }
 *     CxxUsing     — using Foo = Bar;
 *
 * The mixed backend walks the existing codegen AST and promotes nodes
 * to C++ equivalents wherever evidence exists (RTTI info, vtable pointers,
 * mangled names, exception-handling structures).
 */

#ifndef RETDEC_CXX_BACKEND_CXX_AST_H
#define RETDEC_CXX_BACKEND_CXX_AST_H

#include "retdec/codegen/codegen.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace retdec {
namespace cxx_backend {

using CExpr = codegen::CExpr;
using CStmt = codegen::CStmt;
using CType = codegen::CType;

// ─── C++ cast kinds ───────────────────────────────────────────────────────────

enum class CxxCastKind : uint8_t {
    Static,         ///< static_cast<T>
    Reinterpret,    ///< reinterpret_cast<T>
    Dynamic,        ///< dynamic_cast<T>
    Const,          ///< const_cast<T>
};

std::string cxxCastKindStr(CxxCastKind k);

// ─── C++ type extension ───────────────────────────────────────────────────────

/**
 * @brief Extended type info for C++ types.
 *
 * Augments the existing CType with:
 *   - Reference (&, &&)
 *   - Template instantiation
 *   - Qualified names (Foo::Bar)
 */
struct CxxTypeInfo {
    enum class RefKind { None, LValue, RValue };

    RefKind     refKind  = RefKind::None;
    bool        isAuto   = false;
    bool        isConst  = false;

    /// Template arguments (when this is a template instantiation)
    std::vector<std::shared_ptr<CType>> templateArgs;

    /// Fully-qualified name prefix (e.g. "std::" for std::string)
    std::string qualifiedPrefix;
};

// ─── C++ expression nodes ─────────────────────────────────────────────────────

/**
 * @brief `new T(args)` or `new T[count]`
 */
struct CxxNewExpr {
    std::shared_ptr<CType>              allocType;   ///< T in new T
    std::vector<std::shared_ptr<CExpr>> args;        ///< constructor args
    std::shared_ptr<CExpr>             arraySize;    ///< non-null for new T[n]
    bool                               hasParens = false;
    bool                               isArray   = false;

    std::string toString() const;
};

/**
 * @brief `delete p` or `delete[] p`
 */
struct CxxDeleteExpr {
    std::shared_ptr<CExpr> ptr;
    bool                   isArray = false;

    std::string toString() const;
};

/**
 * @brief static_cast<T>(e) etc.
 */
struct CxxCastExpr {
    CxxCastKind            castKind = CxxCastKind::Static;
    std::shared_ptr<CType> targetType;
    std::shared_ptr<CExpr> expr;

    std::string toString() const;
};

/**
 * @brief `throw e` as an expression (also used in `throw;` re-throw)
 */
struct CxxThrowExpr {
    std::shared_ptr<CExpr> expr;  ///< null for bare `throw;`

    std::string toString() const;
};

/**
 * @brief `Namespace::Class::member` qualified name reference.
 */
struct CxxScopeExpr {
    std::vector<std::string> scopes;  ///< e.g. ["std","string","npos"]

    std::string toString() const;
};

/**
 * @brief `obj.method(args)` or `obj->method(args)` with C++-specific
 *        method resolution (virtual dispatch awareness).
 */
struct CxxMethodCallExpr {
    std::shared_ptr<CExpr>              object;
    std::string                         methodName;
    std::vector<std::shared_ptr<CExpr>> args;
    bool                                arrowAccess = false;
    bool                                isVirtual   = false;

    std::string toString() const;
};

/**
 * @brief `[captures](params) -> ret { body }` lambda (best-effort).
 */
struct CxxLambdaExpr {
    enum class CaptureKind { None, ByValue, ByRef, Mixed };
    CaptureKind captureDefault = CaptureKind::None;
    std::vector<std::string>   captureList;   ///< explicit captures
    std::vector<codegen::CParam> params;
    std::shared_ptr<CType>    returnType;
    std::shared_ptr<CStmt>    body;

    std::string toString() const;
};

// ─── C++ statement nodes ──────────────────────────────────────────────────────

/**
 * @brief A single catch clause: `catch (ExcType name) { body }`
 */
struct CxxCatchClause {
    std::shared_ptr<CType>  exceptionType;   ///< null = catch(...)
    std::string              varName;
    std::shared_ptr<CStmt>  body;
};

/**
 * @brief `try { tryBody } catch (...) { ... }`
 */
struct CxxTryStmt {
    std::shared_ptr<CStmt>          tryBody;
    std::vector<CxxCatchClause>     catches;

    std::string toString(int indent, int indentWidth) const;
};

// ─── C++ declaration nodes ────────────────────────────────────────────────────

/**
 * @brief C++ member function (method) declaration.
 */
struct CxxMethod {
    std::string                      name;
    std::shared_ptr<CType>           returnType;
    std::vector<codegen::CParam>     params;
    bool                             isVirtual     = false;
    bool                             isPureVirtual  = false;
    bool                             isOverride    = false;
    bool                             isFinal       = false;
    bool                             isConst       = false;
    bool                             isStatic      = false;
    bool                             isConstructor = false;
    bool                             isDestructor  = false;
    bool                             isInline      = false;
    bool                             isVariadic    = false;
    std::shared_ptr<CStmt>           body;          ///< null if declaration only

    std::string toString(int indent, int indentWidth) const;
};

/**
 * @brief C++ class or struct declaration.
 */
struct CxxClass {
    enum class Kind { Class, Struct };

    std::string             name;
    Kind                    kind = Kind::Class;

    struct BaseClass {
        std::string name;
        std::string access;  ///< "public", "protected", "private", ""
        bool        isVirtual = false;
    };
    std::vector<BaseClass>  bases;

    struct Field {
        std::string             name;
        std::shared_ptr<CType>  type;
        std::string             access;   ///< "public" / "protected" / "private"
        bool                    isStatic  = false;
        std::shared_ptr<CExpr>  initValue;
    };
    std::vector<Field>      fields;
    std::vector<CxxMethod>  methods;

    bool isAbstract() const;
    std::string toString(int indent, int indentWidth) const;
};

/**
 * @brief C++ namespace block.
 */
struct CxxNamespace {
    std::string  name;    ///< empty for anonymous namespace
    bool         isInline = false;
    std::string  contents;  ///< pre-formatted body

    std::string toString() const;
};

/**
 * @brief template<typename T, ...> decoration on a class or function.
 */
struct CxxTemplate {
    struct Param {
        enum class Kind { Typename, NonType };
        Kind        kind = Kind::Typename;
        std::string name;
        std::shared_ptr<CType> type;  ///< for NonType params
        bool        hasDefault = false;
        std::string defaultStr;
    };
    std::vector<Param> params;

    std::string paramStr() const;
};

/**
 * @brief `enum class Foo : underlying { ... }`
 */
struct CxxEnum {
    std::string              name;
    std::string              underlying;  ///< e.g. "uint8_t", "" for default
    bool                     isClass = true;
    struct Enumerator {
        std::string name;
        std::optional<int64_t> value;
    };
    std::vector<Enumerator> enumerators;

    std::string toString() const;
};

/**
 * @brief `using Alias = Type;` or `using namespace Foo;`
 */
struct CxxUsing {
    std::string name;
    std::string target;     ///< empty for `using namespace`
    bool        isNamespace = false;

    std::string toString() const;
};

// ─── C++ translation unit ─────────────────────────────────────────────────────

/**
 * @brief Full C++ translation unit with C++ declarations alongside
 *        the existing C-level functions.
 */
struct CxxUnit {
    std::string              filename;
    bool                     isCxx = true;    ///< .cpp vs .c output

    std::vector<std::string> includes;        ///< #include lines
    std::vector<std::string> systemIncludes;  ///< <system> includes
    std::vector<CxxUsing>    usings;
    std::vector<CxxNamespace> namespaces;
    std::vector<CxxTemplate>  templates;
    std::vector<CxxClass>     classes;
    std::vector<CxxEnum>      enums;
    std::vector<codegen::CFunction> functions; ///< standalone C functions
    std::string              globalDecls;
};

} // namespace cxx_backend
} // namespace retdec

#endif // RETDEC_CXX_BACKEND_CXX_AST_H
