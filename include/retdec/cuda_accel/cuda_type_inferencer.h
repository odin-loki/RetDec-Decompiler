/**
 * @file include/retdec/cuda_accel/cuda_type_inferencer.h
 * @brief CUDA-accelerated type propagation — replaces OCLTypeInferencer.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace retdec::cuda_accel {

class CUDAContext;

enum class TypeSign : std::uint8_t {
    Unknown  = 0,
    Signed   = 1,
    Unsigned = 2,
};

struct TypeSlot {
    std::uint8_t widthBytes{0};  ///< 0=unknown, 1/2/4/8/16
    TypeSign     sign{TypeSign::Unknown};
    bool         isPointer{false};
};

struct OperandHint {
    std::uint32_t slot;
    std::uint8_t  widthBytes{0};
    TypeSign      sign{TypeSign::Unknown};
    bool          isPointer{false};
};

struct TypeConstraint {
    std::uint32_t slotA;
    std::uint32_t slotB;
};

struct FunctionTypeData {
    std::uint32_t               numSlots{0};
    std::vector<TypeConstraint> constraints;
    std::vector<OperandHint>    operandHints;
};

class CUDATypeInferencer {
public:
    static constexpr std::uint32_t kMaxIterations = 128;

    explicit CUDATypeInferencer(CUDAContext* ctx = nullptr);
    ~CUDATypeInferencer();

    std::vector<TypeSlot> infer(const std::vector<FunctionTypeData>& functions);

    bool               usesGPU()        const noexcept { return ctx_ && gpuReady_; }
    const std::string& lastError()      const noexcept { return lastError_; }
    std::uint32_t      lastIterations() const noexcept { return lastIter_; }

private:
    std::vector<TypeSlot> inferCPU(const std::vector<FunctionTypeData>& functions);

    CUDAContext*  ctx_{nullptr};
    bool          gpuReady_{false};
    std::string   lastError_;
    std::uint32_t lastIter_{0};
};

} // namespace retdec::cuda_accel
