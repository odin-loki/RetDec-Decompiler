/**
 * @file include/retdec/cli_parser/cli_sig.h
 * @brief .NET CLI type signature decoder (ECMA-335 §II.23.2).
 *
 * ## Signature kinds
 *
 * Signatures live in the #Blob heap.  The first byte identifies the kind:
 *
 *   FieldSig:        0x06 Type
 *   LocalVarSig:     0x07 Count Type*
 *   MethodDefSig:    CallingConv RetType ParamCount Param*
 *   MethodRefSig:    CallingConv RetType ParamCount Param* [VarParam*]
 *   PropertySig:     0x08 ParamCount CustomMod* Type Param*
 *   TypeSpec:        Type
 *   MethodSpec:      0x0A GenArgCount Type*
 *
 * ## ELEMENT_TYPE_* values (§II.23.1.16)
 *
 *   0x00 END             0x01 VOID        0x02 BOOLEAN
 *   0x03 CHAR            0x04 I1          0x05 U1
 *   0x06 I2              0x07 U2          0x08 I4
 *   0x09 U4              0x0A I8          0x0B U8
 *   0x0C R4              0x0D R8          0x0E STRING
 *   0x0F PTR             0x10 BYREF       0x11 VALUETYPE
 *   0x12 CLASS           0x13 VAR         0x14 ARRAY
 *   0x15 GENERICINST     0x16 TYPEDBYREF  0x18 I
 *   0x19 U               0x1B FNPTR       0x1C OBJECT
 *   0x1D SZARRAY         0x1E MVAR        0x1F CMOD_REQD
 *   0x20 CMOD_OPT        0x21 INTERNAL    0x40 MODIFIER
 *   0x41 SENTINEL        0x45 PINNED
 */

#ifndef RETDEC_CLI_PARSER_CLI_SIG_H
#define RETDEC_CLI_PARSER_CLI_SIG_H

#include "retdec/bc_module/bc_type.h"
#include "retdec/cli_parser/cli_tables.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace retdec {
namespace cli_parser {

using namespace bc_module;

// ─── ELEMENT_TYPE constants ───────────────────────────────────────────────────

enum class ElementType : uint8_t {
    End          = 0x00,
    Void         = 0x01,
    Boolean      = 0x02,
    Char         = 0x03,
    I1           = 0x04,
    U1           = 0x05,
    I2           = 0x06,
    U2           = 0x07,
    I4           = 0x08,
    U4           = 0x09,
    I8           = 0x0A,
    U8           = 0x0B,
    R4           = 0x0C,
    R8           = 0x0D,
    String       = 0x0E,
    Ptr          = 0x0F,
    ByRef        = 0x10,
    ValueType    = 0x11,
    Class        = 0x12,
    Var          = 0x13,  ///< Generic type parameter of owning class
    Array        = 0x14,
    GenericInst  = 0x15,
    TypedByRef   = 0x16,
    I            = 0x18,  ///< System.IntPtr
    U            = 0x19,  ///< System.UIntPtr
    FnPtr        = 0x1B,
    Object       = 0x1C,
    SzArray      = 0x1D,  ///< Single-dimension 0-based array
    MVar         = 0x1E,  ///< Generic type parameter of owning method
    CModReqd     = 0x1F,
    CModOpt      = 0x20,
    Internal     = 0x21,
    Modifier     = 0x40,
    Sentinel     = 0x41,
    Pinned       = 0x45,
};

// ─── Calling conventions ─────────────────────────────────────────────────────

enum class CallingConvention : uint8_t {
    Default    = 0x00,
    C          = 0x01,
    StdCall    = 0x02,
    ThisCall   = 0x03,
    FastCall   = 0x04,
    VarArg     = 0x05,
    Unmanaged  = 0x09,
    Generic    = 0x10,
    HasThis    = 0x20,
    ExplicitThis = 0x40,
};

// ─── Decoded .NET type ────────────────────────────────────────────────────────

/**
 * @brief Decoded .NET type (extends BcType with .NET-specific info).
 *
 * Most .NET types map directly to BcType.  This struct wraps the BcType
 * and adds extra information only relevant to the CIL layer (modifiers,
 * pointer layers, custom mods).
 */
struct CliType {
    BcType  base;               ///< The canonical BcType equivalent

    bool    byRef   = false;    ///< &T (out / ref parameter)
    bool    pinned  = false;    ///< pinned (fixed statement)
    bool    isPtr   = false;    ///< unsafe T*
    bool    typedByRef = false; ///< System.TypedReference

    /// Custom modifiers (modreq / modopt) — just the TypeDefOrRef token.
    std::vector<MetadataToken> modreqs;
    std::vector<MetadataToken> modopts;

    /// For generic type parameters: index and owner kind.
    int     varIdx  = -1;       ///< ≥0 for Var/MVar
    bool    isMVar  = false;    ///< true = method param, false = class param
};

// ─── Decoded method signature ─────────────────────────────────────────────────

struct CliMethodSig {
    CallingConvention callingConv    = CallingConvention::Default;
    bool              hasThis        = false;
    bool              explicitThis   = false;
    bool              isGeneric      = false;
    uint32_t          genParamCount  = 0;
    CliType           retType;
    std::vector<CliType> params;
    bool              hasVarArg      = false;
    uint32_t          varArgStart    = 0;  ///< Index of first vararg param
};

// ─── Decoded local variable signature ────────────────────────────────────────

struct CliLocalVarSig {
    std::vector<CliType> locals;
};

// ─── Type name resolver callback ─────────────────────────────────────────────

/**
 * @brief Callback interface for resolving TypeDefOrRef tokens to names.
 *
 * The signature decoder needs to resolve TypeDef/TypeRef/TypeSpec tokens
 * to type names.  The CLIReader provides a concrete implementation.
 */
class ITypeNameResolver {
public:
    virtual ~ITypeNameResolver() = default;

    /// Return fully-qualified CLR name for a TypeDef row (1-based).
    virtual std::string typeDefName(uint32_t idx) const = 0;

    /// Return fully-qualified CLR name for a TypeRef row (1-based).
    virtual std::string typeRefName(uint32_t idx) const = 0;

    /// Return BcType for a TypeSpec row (decode its blob).
    virtual BcType typeSpecType(uint32_t idx) const = 0;
};

// ─── Signature decoder ────────────────────────────────────────────────────────

/**
 * @brief Decodes all .NET CLI signature blob types.
 *
 * Converts raw #Blob bytes into `CliType`, `CliMethodSig`, and
 * `CliLocalVarSig` values, and ultimately into `BcType` / `BcFuncType`.
 */
class CliSigDecoder {
public:
    /**
     * @param resolver   Optional: used to resolve TypeDefOrRef tokens.
     *                   If null, class names will be placeholder strings.
     */
    explicit CliSigDecoder(const ITypeNameResolver* resolver = nullptr);

    // ── Field signature ───────────────────────────────────────────────────

    /// Decode a FieldSig blob.  Returns the field's type.
    std::optional<CliType> decodeField(std::span<const uint8_t> blob) const;

    // ── Method signature ─────────────────────────────────────────────────

    /// Decode a MethodDefSig or MethodRefSig blob.
    std::optional<CliMethodSig> decodeMethod(std::span<const uint8_t> blob) const;

    /// Convert a CliMethodSig to a BcFuncType.
    BcFuncType toBcFuncType(const CliMethodSig& sig) const;

    // ── Local variable signature ─────────────────────────────────────────

    /// Decode a LocalVarSig blob.
    std::optional<CliLocalVarSig> decodeLocalVar(std::span<const uint8_t> blob) const;

    // ── Type spec ────────────────────────────────────────────────────────

    /// Decode a TypeSpec blob (just a Type encoding).
    std::optional<CliType> decodeTypeSpec(std::span<const uint8_t> blob) const;

    // ── Method spec ──────────────────────────────────────────────────────

    /// Decode a MethodSpec instantiation blob.
    std::vector<CliType> decodeMethodSpec(std::span<const uint8_t> blob) const;

    // ── BcType conversion ────────────────────────────────────────────────

    /// Convert a CliType to a BcType (drops modifiers).
    BcType toBcType(const CliType& ct) const;

    // ── CLR name to BcType ────────────────────────────────────────────────

    /// Produce a BcType for a well-known CLR type name.
    static BcType clrNameToType(const std::string& fqName);

private:
    const ITypeNameResolver* resolver_;

    // Core type decoder — advances pos past the decoded type.
    CliType decodeType(std::span<const uint8_t> blob, size_t& pos) const;

    // Decode custom modifiers (modreq/modopt) — advances pos.
    void decodeCustomMods(std::span<const uint8_t> blob, size_t& pos,
                           std::vector<MetadataToken>& reqs,
                           std::vector<MetadataToken>& opts) const;

    // Decode array shape: rank, sizes, lo-bounds.
    void decodeArrayShape(std::span<const uint8_t> blob, size_t& pos,
                           uint32_t& rank,
                           std::vector<int32_t>& sizes,
                           std::vector<int32_t>& loBounds) const;

    // Read a TypeDefOrRef coded token from a compressed uint.
    MetadataToken readTypeDefOrRef(std::span<const uint8_t> blob, size_t& pos) const;

    // Lookup the CLR name for a token.
    std::string tokenName(MetadataToken tok) const;

    // Map ElementType to BcPrimKind.
    static std::optional<BcPrimKind> elemToPrim(ElementType e);
};

} // namespace cli_parser
} // namespace retdec

#endif // RETDEC_CLI_PARSER_CLI_SIG_H
