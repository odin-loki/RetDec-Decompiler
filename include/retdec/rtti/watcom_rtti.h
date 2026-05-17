/**
 * @file include/retdec/rtti/watcom_rtti.h
 * @brief Open Watcom C++ RTTI reconstructor.
 *
 * ## Watcom vtable layout
 *
 *   vtable[-1]  pointer to __WatcomTypeInfo descriptor (or nullptr)
 *   vtable[0..] virtual function pointers (using Watcom register convention)
 *
 * ## __WatcomTypeInfo structure (Open Watcom runtime)
 *
 *   struct __WatcomTypeInfo {
 *       uint16_t    kind;       // 0 = class, 1 = fundamental
 *       const char* name;       // null-terminated decorated name
 *       __WatcomTypeInfo* base; // single-inheritance base (or nullptr)
 *       // For multiple inheritance there is an array terminated by nullptr.
 *   };
 *
 * ## Watcom register calling convention
 *
 * Watcom's distinctive "__watcall" (register) convention passes the first
 * four integer/pointer arguments in EAX, EDX, EBX, ECX (in that order)
 * instead of the stack.  The `this` pointer goes in ECX for member functions
 * (like MSVC's __thiscall), but using the full Watcom register sequence the
 * effective order is: this→EAX (or can be any), remaining in EDX/EBX/ECX.
 *
 * Detection heuristic: functions whose prologue does NOT start with
 * "push ebp; mov ebp, esp" but instead immediately use EAX/EDX as input
 * operands are likely Watcom-compiled.
 *
 * ## Scan strategy
 *
 * 1. Scan every readable non-executable section for pointer-sized words
 *    pointing into another data section (candidate __WatcomTypeInfo).
 * 2. Validate: candidate[0] is a small kind value (0 or 1).
 *    candidate[+4/+8] resolves to a readable string (name).
 * 3. The vtable containing this pointer at slot[-1] must have at least one
 *    executable function in slot[0..].
 */

#ifndef RETDEC_RTTI_WATCOM_RTTI_H
#define RETDEC_RTTI_WATCOM_RTTI_H

#include "retdec/rtti/rtti.h"
#include <unordered_set>

namespace retdec {
namespace rtti {

// Watcom type-info kind codes
enum class WatcomTypeKind : uint16_t {
    Class       = 0,
    Fundamental = 1,
    Pointer     = 2,
    Array       = 3,
    Function    = 4,
};

class WatcomRttiReconstructor : public VtableReconstructorBase {
public:
    WatcomRttiReconstructor() = default;

    bool reconstruct(
        const BinaryView&            view,
        const std::vector<uint64_t>& funcVmas,
        ClassHierarchyGraph&         out) override;

    std::string name() const override { return "WatcomRttiReconstructor"; }

    bool isValidTypeInfo(const BinaryView& view, uint64_t tiVma) const;
    std::string parseTypeInfo(const BinaryView& view, uint64_t tiVma,
                               ClassHierarchyGraph& out);

private:
    std::unordered_set<uint64_t> visited_;

    void scanVtables(const BinaryView&             view,
                     const std::unordered_set<uint64_t>& funcSet,
                     ClassHierarchyGraph&           out);
};

} // namespace rtti
} // namespace retdec

#endif // RETDEC_RTTI_WATCOM_RTTI_H
