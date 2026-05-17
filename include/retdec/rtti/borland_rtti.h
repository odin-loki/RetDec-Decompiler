/**
 * @file include/retdec/rtti/borland_rtti.h
 * @brief Borland/Embarcadero C++ Builder (VCL) RTTI reconstructor.
 *
 * ## Borland VMT layout (32-bit, Delphi 7 / C++Builder 6 era)
 *
 * The VMT is a block of data.  A pointer to it is stored as the first field
 * of every TObject-derived instance.  Negative offsets from the vtable
 * contain RTTI metadata; positive offsets are virtual method pointers.
 *
 *   vmtSelfPtr       = -76  pointer to the VMT itself (self-referential)
 *   vmtIntfTable     = -72  published interfaces (or 0)
 *   vmtAutoTable     = -68  Automation dispatch table (or 0)
 *   vmtInitTable     = -64  field initialisation table (or 0)
 *   vmtTypeInfo      = -60  TTypeInfo* (the RTTI blob)
 *   vmtFieldTable    = -56  field table (or 0)
 *   vmtMethodTable   = -52  published method table (or 0)
 *   vmtDynamicTable  = -48  dynamic method table (or 0)
 *   vmtClassName     = -44  PShortString (length-prefixed class name)
 *   vmtInstanceSize  = -40  Integer (instance byte size)
 *   vmtParent        = -36  ^TVMT (pointer to parent's VMT, or 0)
 *
 *   Virtual methods start at offset 0.  The first ~8 slots are the
 *   standard TObject virtuals (SafeCallException, AfterConstruction, …).
 *
 * ## TTypeInfo (tkClass, Kind = 7)
 *
 *   TTypeInfo  { Kind: Byte; Name: ShortString }
 *   TTypeData  { ClassType: TClass; ParentInfo: ^TTypeInfo;
 *                PropCount: SmallInt; UnitName: ShortString;
 *                PropData: TPropData (variable-length) }
 *
 * ## Scan strategy
 *
 * 1. Scan every readable data section for pointer-sized words where the
 *    value equals the address of the word itself minus 76 (vmtSelfPtr
 *    pattern).  This gives candidate VMT starts.
 * 2. For each candidate VMT: validate vmtParent chain terminates at 0 or
 *    a known TObject VMT; validate vmtClassName points to a valid Pascal
 *    short-string; validate at least one virtual method slot points to the
 *    executable section.
 * 3. Follow vmtParent recursively to build the inheritance chain.
 * 4. Parse vmtTypeInfo blob for extended property metadata (optional).
 */

#ifndef RETDEC_RTTI_BORLAND_RTTI_H
#define RETDEC_RTTI_BORLAND_RTTI_H

#include "retdec/rtti/rtti.h"
#include <unordered_set>

namespace retdec {
namespace rtti {

// Borland VMT negative offsets (32-bit / 4-byte pointer model)
static constexpr int32_t kVmtSelfPtr      = -76;
static constexpr int32_t kVmtIntfTable    = -72;
static constexpr int32_t kVmtAutoTable    = -68;
static constexpr int32_t kVmtInitTable    = -64;
static constexpr int32_t kVmtTypeInfo     = -60;
static constexpr int32_t kVmtFieldTable   = -56;
static constexpr int32_t kVmtMethodTable  = -52;
static constexpr int32_t kVmtDynamicTable = -48;
static constexpr int32_t kVmtClassName    = -44;
static constexpr int32_t kVmtInstanceSize = -40;
static constexpr int32_t kVmtParent       = -36;

// TTypeKind values (Delphi/BCB)
enum class TTypeKind : uint8_t {
    tkUnknown  = 0,
    tkInteger  = 1,
    tkChar     = 2,
    tkEnumeration = 3,
    tkFloat    = 4,
    tkString   = 5,
    tkSet      = 6,
    tkClass    = 7,
    tkMethod   = 8,
    tkWChar    = 9,
    tkLString  = 10,
    tkWString  = 11,
    tkVariant  = 12,
    tkArray    = 13,
    tkRecord   = 14,
    tkInterface= 15,
    tkInt64    = 16,
    tkDynArray = 17,
    tkUString  = 18,
    tkClassRef = 19,
    tkPointer  = 20,
    tkProcedure= 21,
};

struct BorlandTypeData {
    uint32_t    classTypeVma  = 0; ///< VMT VMA (= ClassType pointer)
    uint32_t    parentInfoVma = 0; ///< Parent TTypeInfo VMA (or 0)
    int16_t     propCount     = 0; ///< Published property count
    std::string unitName;          ///< Unit (translation-unit) name
};

struct BorlandVmtInfo {
    uint64_t        vmtVma        = 0; ///< VMT start (positive offsets)
    uint64_t        selfPtrVma    = 0; ///< vmtSelfPtr field VMA
    std::string     className;
    uint32_t        instanceSize  = 0;
    uint64_t        parentVmtVma  = 0; ///< 0 = TObject (root)
    uint64_t        typeInfoVma   = 0;
    BorlandTypeData typeData;
    std::vector<uint64_t> virtualMethodVmas;
};

class BorlandRttiReconstructor : public VtableReconstructorBase {
public:
    BorlandRttiReconstructor() = default;

    bool reconstruct(
        const BinaryView&            view,
        const std::vector<uint64_t>& funcVmas,
        ClassHierarchyGraph&         out) override;

    std::string name() const override { return "BorlandRttiReconstructor"; }

    // ── Public helpers ────────────────────────────────────────────────────────

    /// Return true if `vmtVma` looks like a valid Borland VMT.
    bool isValidVmt(const BinaryView& view, uint64_t vmtVma) const;

    /// Parse the VMT at `vmtVma`; populate info.  Returns false on failure.
    bool parseVmt(const BinaryView& view, uint64_t vmtVma,
                  BorlandVmtInfo& info) const;

    /// Read a Pascal ShortString at `vma`; returns up to 255 chars.
    static std::string readShortString(const BinaryView& view, uint64_t vma);

private:
    mutable std::unordered_set<uint64_t> visited_;

    void scanVmts(const BinaryView& view,
                  const std::unordered_set<uint64_t>& funcSet,
                  ClassHierarchyGraph& out);

    void parseTTypeInfo(const BinaryView& view, uint64_t tiVma,
                        BorlandTypeData& out) const;
};

} // namespace rtti
} // namespace retdec

#endif // RETDEC_RTTI_BORLAND_RTTI_H
