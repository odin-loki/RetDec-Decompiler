/**
 * @file include/retdec/rtti/dmc_rtti.h
 * @brief Digital Mars C++ (DMC) RTTI reconstructor.
 *
 * ## DMC vtable layout
 *
 *   vtable[-1]  pointer to __typeinfo object (or nullptr if no RTTI)
 *   vtable[0..] virtual function pointers
 *
 * ## __typeinfo structure (DMC internal)
 *
 *   struct __typeinfo {
 *       void*        vptr;   // pointer into the __typeinfo class's vtable
 *       const char*  name;   // null-terminated decorated class name
 *   };
 *
 * For derived classes DMC emits a __DMCclassinfo descriptor that extends
 * __typeinfo and adds the base class pointer:
 *
 *   struct __DMCclassinfo : __typeinfo {
 *       unsigned     size;      // sizeof(class)
 *       __typeinfo*  base;      // immediate base (single inheritance only)
 *       // MI is stored differently; treat as opaque for now
 *   };
 *
 * ## Scan strategy
 *
 * 1. Scan every readable non-executable section for pointer-sized words
 *    where `ptr → data_section` (candidate vtable[-1] slots).
 * 2. Candidate __typeinfo: `[vptr_into_data][name_ptr_into_data]`.
 *    The name pointer must resolve to a non-empty string starting with a
 *    printable ASCII character.
 * 3. Validate: the word at `candidate[-ps]` (vtable[-1]) pointing to this
 *    __typeinfo is itself preceded by at least one executable function ptr.
 * 4. Parse name: DMC stores undecorated (already readable) C++ class names
 *    in most builds, so demangling is usually a pass-through.
 */

#ifndef RETDEC_RTTI_DMC_RTTI_H
#define RETDEC_RTTI_DMC_RTTI_H

#include "retdec/rtti/rtti.h"
#include <unordered_set>

namespace retdec {
namespace rtti {

class DmcRttiReconstructor : public VtableReconstructorBase {
public:
    DmcRttiReconstructor() = default;

    bool reconstruct(
        const BinaryView&            view,
        const std::vector<uint64_t>& funcVmas,
        ClassHierarchyGraph&         out) override;

    std::string name() const override { return "DmcRttiReconstructor"; }

    /// Return true if `tiVma` looks like a valid DMC __typeinfo object.
    bool isValidTypeInfo(const BinaryView& view, uint64_t tiVma) const;

    /// Parse a __typeinfo at `tiVma`; returns the class name or "".
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

#endif // RETDEC_RTTI_DMC_RTTI_H
