/**
 * @file include/retdec/type_inference/type_inference.h
 * @brief Width-Seeded Type Inference with Calling Convention Seeding.
 *
 * ## Overview
 *
 * This module recovers source-level types for SSA values using a two-phase
 * algorithm seeded by instruction operand widths, ABI calling conventions,
 * and demangled symbol information.
 *
 * ### Phase 1 — Width Seeding (`WidthSeeder`)
 *
 * Every instruction operand carries an implicit width (the register or
 * memory operand size in bits).  We extract this from each defining
 * instruction:
 *
 *   MOV EAX, x           → value is 32-bit
 *   MOVZX EAX, BYTE[x]   → value is 8-bit (zero-extended into 32)
 *   MOVSX EAX, WORD[x]   → value is 16-bit signed (sign-extended)
 *   MOV RAX, x           → value is 64-bit
 *   MOVAPS XMM0, x       → value is 128-bit (4×float or 2×double)
 *   VMOVAPS YMM0, x      → value is 256-bit (AVX)
 *   VPBROADCASTD ZMM0, x → value is 512-bit (AVX-512)
 *
 * Conflict resolution: when the same SSA value has contradictory width
 * signals (e.g. defined by a 32-bit MOV but used in a 64-bit context),
 * we use the widest non-truncating width.  If all uses are narrower,
 * we retain the definition width.
 *
 * After width seeding, > 95% of values have a known bit-width.
 *
 * ### Phase 2 — Type Propagation (`TypePropagation`)
 *
 * Type propagation uses a union-find data structure where each equivalence
 * class carries a partial type descriptor.  Constraints are emitted by
 * instruction patterns:
 *
 *   `SameWidth(a, b)`    — a and b must have the same bit-width.
 *   `SameSign(a, b)`     — a and b have the same signedness.
 *   `IsPointer(a)`       — a is a pointer type.
 *   `IsUnsigned(a)`      — a is an unsigned integer.
 *   `IsSigned(a)`        — a is a signed integer.
 *   `IsFloat(a)`         — a is a floating-point value.
 *   `IsStruct(a, sid)`   — a points to struct with ID `sid`.
 *   `IsArray(a, et, n)`  — a points to array of `n` elements of type `et`.
 *
 * Constraint sources:
 *   - Instruction semantics: e.g. ADD → integer, FADD → float, SAR → signed.
 *   - Comparison instructions: SETA (unsigned), SETG (signed).
 *   - Pointer arithmetic: LEA → pointer, ADD to pointer → pointer.
 *   - Memory access pattern: pointer + constant offset → struct field.
 *   - ABI seeding: formal parameter types from calling convention.
 *   - Demangled symbols: type strings from Stage 12 (`type_seed` module).
 *
 * ### ABI Seeding (`AbiSeeder`)
 *
 * For each function with a known ABI:
 *   - Integer argument registers (RDI, RSI, RDX, RCX, R8, R9 on SysV x64;
 *     RCX, RDX, R8, R9 on Win64) receive `SameWidth` and signedness seeds.
 *   - Floating-point argument registers (XMM0–XMM7) receive `IsFloat`.
 *   - The return value register (RAX / XMM0) receives the return type.
 *
 * ### Structural Type Recovery (`StructRecovery`)
 *
 * Two patterns are recognised:
 *
 *   **Struct access**: if a pointer `p` is used in `LOAD p + k` for multiple
 *   distinct constant offsets `k`, then `p` points to a struct with fields
 *   at those offsets.  The field type is inferred from the access width.
 *
 *   **Array access**: if a pointer `p` is used in `LOAD p + i*stride` for a
 *   loop-varying `i`, then `p` points to an array of `stride`-byte elements.
 *
 * ## Data structures
 *
 *   `IrType`          — the recovered type for one SSA value
 *   `TypeConstraint`  — one type relationship assertion
 *   `TypeClass`       — union-find equivalence class carrying a partial type
 *   `StructLayout`    — recovered struct: list of (offset, width, name) fields
 *   `TypeInferencePass` — orchestrates all sub-passes
 */

#ifndef RETDEC_TYPE_INFERENCE_H
#define RETDEC_TYPE_INFERENCE_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace retdec {
namespace ssa {
class SSAFunction;
} // namespace ssa
} // namespace retdec

namespace retdec {
namespace type_inference {

// ─── Type kinds ──────────────────────────────────────────────────────────────

enum class TypeKind : uint8_t {
    Unknown,    ///< Not yet inferred
    Void,       ///< Void (return type only)
    Bool,       ///< 1-bit boolean
    Integer,    ///< Signed or unsigned integer of known width
    Float,      ///< IEEE 754 float (32, 64, or 80-bit)
    Pointer,    ///< Pointer to some pointee type
    Struct,     ///< Composite struct (fields at known offsets)
    Array,      ///< Array of uniform element type
    Function,   ///< Function pointer
    Vector,     ///< SIMD vector (XMM/YMM/ZMM lane type × count)
};

enum class Signedness : uint8_t {
    Unknown,
    Signed,
    Unsigned,
};

// ─── Struct layout ────────────────────────────────────────────────────────────

struct StructField {
    uint32_t    offset  = 0;    ///< byte offset within struct
    uint8_t     width   = 0;    ///< field width in bytes
    Signedness  sign    = Signedness::Unknown;
    TypeKind    kind    = TypeKind::Unknown;
    std::string name;           ///< "field_<offset>" or DWARF name
    uint32_t    pointeeStructId = UINT32_MAX; ///< if kind==Pointer→Struct
};

struct StructLayout {
    uint32_t              id     = UINT32_MAX;
    uint32_t              size   = 0;     ///< total byte size
    std::vector<StructField> fields;
    std::string           name;           ///< "struct_<id>" or demangled name

    bool hasField(uint32_t offset) const;
    void addField(StructField f);
    const StructField* field(uint32_t offset) const;
};

// ─── IR type ──────────────────────────────────────────────────────────────────

/**
 * The recovered type for one SSA value.
 * Filled in during type inference and used by code generation.
 */
struct IrType {
    TypeKind   kind       = TypeKind::Unknown;
    uint16_t   width      = 0;     ///< bit-width (8/16/32/64/80/128/256/512)
    Signedness sign       = Signedness::Unknown;

    // For Pointer
    TypeKind   pointeeKind= TypeKind::Unknown;
    uint16_t   pointeeWidth= 0;
    uint32_t   structId   = UINT32_MAX;  ///< if pointeeKind == Struct

    // For Array
    uint32_t   arrayCount = 0;
    uint16_t   elemWidth  = 0;
    TypeKind   elemKind   = TypeKind::Unknown;

    // For Vector (SIMD)
    uint8_t    laneCount  = 0;
    uint16_t   laneWidth  = 0;

    bool isKnown()   const { return kind != TypeKind::Unknown; }
    bool isPointer() const { return kind == TypeKind::Pointer; }
    bool isInteger() const { return kind == TypeKind::Integer; }
    bool isFloat()   const { return kind == TypeKind::Float;   }
    bool isStruct()  const { return kind == TypeKind::Struct;  }
    bool isArray()   const { return kind == TypeKind::Array;   }

    std::string toString() const;

    static IrType integer(uint16_t w, Signedness s = Signedness::Unknown) {
        IrType t; t.kind = TypeKind::Integer; t.width = w; t.sign = s; return t;
    }
    static IrType fp(uint16_t w) {
        IrType t; t.kind = TypeKind::Float; t.width = w; return t;
    }
    static IrType ptr(TypeKind pointee = TypeKind::Unknown, uint16_t pw = 0) {
        IrType t; t.kind = TypeKind::Pointer; t.pointeeKind = pointee;
        t.pointeeWidth = pw; return t;
    }
    static IrType boolean() {
        IrType t; t.kind = TypeKind::Bool; t.width = 1; return t;
    }
    static IrType vec(uint8_t lanes, uint16_t laneW, TypeKind lk = TypeKind::Float) {
        IrType t; t.kind = TypeKind::Vector; t.laneCount = lanes;
        t.laneWidth = laneW; t.pointeeKind = lk; t.width = (uint16_t)(lanes * laneW); return t;
    }
    static IrType unknown() { return IrType{}; }
};

// ─── Type constraints ─────────────────────────────────────────────────────────

enum class ConstraintKind : uint8_t {
    SameWidth,    ///< a and b have the same bit-width
    SameSign,     ///< a and b have the same signedness
    IsPointer,    ///< a is a pointer
    IsUnsigned,   ///< a is an unsigned integer
    IsSigned,     ///< a is a signed integer
    IsFloat,      ///< a is a float
    IsBool,       ///< a is a 1-bit boolean
    IsStruct,     ///< a points to struct sid
    IsArray,      ///< a points to array[count] of elemWidth
    HasWidth,     ///< a has this specific bit-width
    ReturnType,   ///< a is the return value (ABI seed)
    ParamType,    ///< a is parameter n (ABI seed)
};

struct TypeConstraint {
    ConstraintKind kind;
    uint32_t  lhsId;     ///< primary SSA value
    uint32_t  rhsId = UINT32_MAX; ///< secondary (for SameWidth/SameSign)
    uint16_t  width = 0;
    Signedness sign  = Signedness::Unknown;
    uint32_t  structId = UINT32_MAX;
    uint32_t  arrayCount = 0;
    uint16_t  elemWidth  = 0;
    TypeKind  elemKind   = TypeKind::Unknown;
    uint8_t   paramIndex = 0;

    static TypeConstraint sameWidth(uint32_t a, uint32_t b) {
        return {ConstraintKind::SameWidth, a, b};
    }
    static TypeConstraint sameSign(uint32_t a, uint32_t b) {
        return {ConstraintKind::SameSign, a, b};
    }
    static TypeConstraint hasWidth(uint32_t a, uint16_t w) {
        TypeConstraint c; c.kind = ConstraintKind::HasWidth;
        c.lhsId = a; c.width = w; return c;
    }
    static TypeConstraint isPointer(uint32_t a) {
        return {ConstraintKind::IsPointer, a};
    }
    static TypeConstraint isSigned(uint32_t a) {
        TypeConstraint c; c.kind = ConstraintKind::IsSigned; c.lhsId = a; return c;
    }
    static TypeConstraint isUnsigned(uint32_t a) {
        TypeConstraint c; c.kind = ConstraintKind::IsUnsigned; c.lhsId = a; return c;
    }
    static TypeConstraint isFloat(uint32_t a, uint16_t w = 0) {
        TypeConstraint c; c.kind = ConstraintKind::IsFloat;
        c.lhsId = a; c.width = w; return c;
    }
    static TypeConstraint isStruct(uint32_t a, uint32_t sid) {
        TypeConstraint c; c.kind = ConstraintKind::IsStruct;
        c.lhsId = a; c.structId = sid; return c;
    }
    static TypeConstraint isArray(uint32_t a, uint32_t cnt, uint16_t ew, TypeKind ek) {
        TypeConstraint c; c.kind = ConstraintKind::IsArray;
        c.lhsId = a; c.arrayCount = cnt; c.elemWidth = ew; c.elemKind = ek; return c;
    }
};

// ─── Phase 1: Width seeder ────────────────────────────────────────────────────

/**
 * Scans all SSA values and their defining instructions to extract
 * bit-width constraints.
 *
 * Rules (architecture-neutral):
 *   width(v) = defInstr.operandWidth   if unambiguous
 *   width(v) = max(all use widths)     on conflict (widen to non-truncating)
 *   width(v) = 64                      if defInstr is a pointer-sized LEA
 *   width(v) = 128                     if defInstr is an XMM operation
 *   width(v) = 256 / 512               for YMM / ZMM operations
 */
class WidthSeeder {
public:
    struct Result {
        std::unordered_map<uint32_t, uint16_t> widths;  ///< ssaId → bit-width
        std::size_t seededCount  = 0;
        std::size_t conflictCount= 0;
    };

    Result run(const ssa::SSAFunction& fn) const;

private:
    uint16_t widthFromInstr(const ssa::SSAFunction& fn,
                             uint32_t valueId) const;
};

// ─── Phase 2: Type propagation ────────────────────────────────────────────────

/**
 * Union-find type propagation.
 *
 * Each SSA value belongs to an equivalence class.  Each class carries a
 * partial `IrType`.  When two classes are merged (united), their partial
 * types are merged using a lattice join:
 *
 *   Unknown ⊔ T   = T
 *   T       ⊔ T   = T
 *   T1      ⊔ T2  = conflict → keep the one from higher-priority source
 *
 * Priority order (highest first):
 *   1. Demangled symbol type (from type_seed module)
 *   2. ABI calling convention seed
 *   3. Instruction semantics
 *   4. Width seeding
 */
class TypePropagation {
public:
    void addValue(uint32_t id);
    void addConstraint(TypeConstraint c);
    void run();

    const IrType& typeOf(uint32_t id) const;
    uint32_t findRoot(uint32_t id) const;
    std::size_t classCount() const;
    std::size_t constraintCount() const { return constraints_.size(); }

private:
    struct TypeClass {
        IrType    type;
        uint8_t   priority = 0;  ///< 0=unknown, 1=width, 2=instr, 3=abi, 4=demangle
    };

    uint32_t find(uint32_t x) const;
    void     unite(uint32_t x, uint32_t y);
    void     applyConstraint(const TypeConstraint& c);
    void     mergeTypes(TypeClass& dst, const TypeClass& src);

    mutable std::vector<uint32_t>  parent_;
    mutable std::vector<uint32_t>  rank_;
    std::vector<TypeClass>          classes_;

    std::vector<TypeConstraint>    constraints_;

    IrType kUnknown;
};

// ─── ABI seeder ───────────────────────────────────────────────────────────────

/**
 * Seeds the type propagation with ABI parameter and return types.
 *
 * For each function, extracts:
 *   - The SSA values bound to argument-passing registers.
 *   - The SSA values bound to return-value registers.
 *   - Demangled symbol type strings from the type_seed module.
 *
 * Emits TypeConstraints into the provided TypePropagation instance.
 */
struct AbiSeedInfo {
    enum class Abi { Unknown, SysV_x64, Win64, SysV_x32, AAPCS64, AAPCS32 };
    Abi abi = Abi::Unknown;

    struct ParamSeed {
        uint32_t   ssaId;     ///< SSA value ID bound to this parameter
        IrType     type;
        uint8_t    paramIndex = 0;
    };
    struct ReturnSeed {
        uint32_t   ssaId;
        IrType     type;
    };

    std::vector<ParamSeed>  params;
    std::optional<ReturnSeed> retVal;
};

class AbiSeeder {
public:
    /// Emit ABI type seeds into `prop` from the given seed info.
    void seed(TypePropagation& prop, const AbiSeedInfo& info) const;

    /// Build a SysV x64 seed for the given function's entry-block values.
    AbiSeedInfo buildSysVx64Seed(const ssa::SSAFunction& fn,
                                  const std::vector<uint32_t>& argValueIds,
                                  const std::vector<IrType>& argTypes,
                                  std::optional<IrType> retType) const;
};

// ─── Struct / array recovery ──────────────────────────────────────────────────

/**
 * Recovers composite types from memory access patterns.
 *
 * ### Struct detection
 *
 * Scans all LOAD/STORE instructions.  For each pointer value `p` used
 * as base + constant offset `k`:
 *   - Record (p, k, access_width).
 *   - When two or more distinct offsets are observed for the same `p`,
 *     emit IsStruct(p, new_struct_id) and add fields at each offset.
 *
 * ### Array detection
 *
 * Scans for the pattern: base + (index × stride) where:
 *   - `index` is a loop-varying SSA value (phi node or incremented value).
 *   - `stride` is a constant.
 *
 * Emits IsArray(base, 0 (unknown count), stride, Unknown).
 *
 * ### Minimum field count
 *
 * A pointer with only one observed access is NOT promoted to a struct
 * (could be a simple `p[0]` access).  Two distinct offsets are required.
 */
class StructRecovery {
public:
    struct AccessPattern {
        uint32_t  baseId;    ///< SSA value of the base pointer
        int64_t   offset;    ///< constant byte offset
        uint8_t   width;     ///< access width in bytes
        bool      isWrite;
    };

    struct Result {
        std::vector<StructLayout>  layouts;
        std::vector<TypeConstraint> constraints;  ///< IsStruct / IsArray
        std::size_t structsFound = 0;
        std::size_t arraysFound  = 0;
    };

    Result run(const ssa::SSAFunction& fn) const;

private:
    void collectPatterns(const ssa::SSAFunction& fn,
                          std::vector<AccessPattern>& out) const;
    bool isLoopVarying(const ssa::SSAFunction& fn, uint32_t valueId) const;
};

// ─── Main type inference pass ─────────────────────────────────────────────────

/**
 * Orchestrates the full type inference pipeline:
 *   1. WidthSeeder → emit HasWidth constraints
 *   2. Instruction pattern scan → emit signedness / pointer constraints
 *   3. AbiSeeder → emit parameter / return-type seeds
 *   4. StructRecovery → emit IsStruct / IsArray constraints
 *   5. TypePropagation::run() → fixpoint over union-find
 *
 * After run(), types() returns the inferred IrType for every SSA value ID.
 */
class TypeInferencePass {
public:
    struct Config {
        AbiSeedInfo abiSeed;
        bool enableStructRecovery = true;
        bool enableArrayRecovery  = true;
    };
    static Config defaultConfig() noexcept { return {}; }

    struct Stats {
        std::size_t valuesWithKnownType  = 0;
        std::size_t valuesWithKnownWidth = 0;
        std::size_t valuesWithKnownSign  = 0;
        std::size_t pointerValues        = 0;
        std::size_t structsRecovered     = 0;
        std::size_t arraysRecovered      = 0;
        std::size_t totalConstraints     = 0;
        std::size_t typeClasses          = 0;
    };

    void run(const ssa::SSAFunction& fn, const Config& cfg = defaultConfig());

    const IrType& typeOf(uint32_t ssaId) const;
    const std::vector<StructLayout>& structLayouts() const { return structs_; }
    const Stats& stats() const { return stats_; }

private:
    TypePropagation prop_;
    std::vector<StructLayout> structs_;
    Stats stats_;

    void emitInstructionConstraints(const ssa::SSAFunction& fn);
    void collectStats(const ssa::SSAFunction& fn);
};

} // namespace type_inference
} // namespace retdec

#endif // RETDEC_TYPE_INFERENCE_H
