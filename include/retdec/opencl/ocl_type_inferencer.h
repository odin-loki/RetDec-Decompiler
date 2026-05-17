/**
 * @file include/retdec/opencl/ocl_type_inferencer.h
 * @brief Parallel per-function type constraint propagation via OpenCL.
 *
 * Usage:
 *   OCLTypeInferencer inf(&ctx);
 *
 *   // Describe one function: 5 type slots, 3 constraints, 4 operand hints.
 *   FunctionTypeData f;
 *   f.numSlots = 5;
 *   f.constraints = {{0,1}, {1,2}, {3,4}};
 *   f.operandHints.push_back({0, 4, TypeSign::Unsigned, false});
 *
 *   std::vector<TypeSlot> result = inf.infer({f});
 */

#ifndef RETDEC_OPENCL_OCL_TYPE_INFERENCER_H
#define RETDEC_OPENCL_OCL_TYPE_INFERENCER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace retdec {
namespace opencl {

class OCLContext;

// ─── Type attributes per slot ─────────────────────────────────────────────────

enum class TypeSign : std::uint8_t {
    Unknown  = 0,
    Signed   = 1,
    Unsigned = 2,
};

struct TypeSlot {
    std::uint8_t widthBytes  = 0;  ///< 0=unknown, 1=i8, 2=i16, 4=i32, 8=i64
    TypeSign     sign        = TypeSign::Unknown;
    bool         isPointer   = false;
};

// ─── Per-function input descriptor ───────────────────────────────────────────

struct OperandHint {
    std::uint32_t slot;        ///< Global slot index within the function
    std::uint8_t  widthBytes;  ///< 0=unknown
    TypeSign      sign;
    bool          isPointer;
};

struct TypeConstraint {
    std::uint32_t slotA;  ///< Slots must have the same type
    std::uint32_t slotB;
};

struct FunctionTypeData {
    std::uint32_t                numSlots    = 0;
    std::vector<TypeConstraint>  constraints;
    std::vector<OperandHint>     operandHints;
};

// ─── OCLTypeInferencer ────────────────────────────────────────────────────────

class OCLTypeInferencer {
public:
    static constexpr std::uint32_t kMaxIterations = 128;

    /// @param ctx  Initialized OCLContext.  Falls back to CPU if null/not ready.
    explicit OCLTypeInferencer(OCLContext* ctx = nullptr);
    ~OCLTypeInferencer();

    OCLTypeInferencer(OCLTypeInferencer&&) noexcept;
    OCLTypeInferencer& operator=(OCLTypeInferencer&&) noexcept;

    OCLTypeInferencer(const OCLTypeInferencer&)            = delete;
    OCLTypeInferencer& operator=(const OCLTypeInferencer&) = delete;

    /// Run type inference on the given functions.
    ///
    /// @param functions   Per-function type data (slots are per-function local IDs;
    ///                    the inferencer translates them to global indices internally).
    /// @returns Flat vector of TypeSlot results: functions[0]'s slots first,
    ///          then functions[1]'s slots, etc.  Size == sum of all numSlots.
    std::vector<TypeSlot> infer(const std::vector<FunctionTypeData>& functions);

    bool usesGPU() const noexcept;
    const std::string& lastError() const noexcept;

    /// Number of fixpoint iterations used in the last infer() call.
    std::uint32_t lastIterations() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace opencl
} // namespace retdec

#endif // RETDEC_OPENCL_OCL_TYPE_INFERENCER_H
