/**
 * @file src/rtti/borland_rtti.cpp
 * @brief Borland/Embarcadero C++Builder VCL VMT reconstructor.
 */

#include "retdec/rtti/borland_rtti.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace retdec {
namespace rtti {

// ─── readShortString ─────────────────────────────────────────────────────────

std::string BorlandRttiReconstructor::readShortString(
    const BinaryView& view, uint64_t vma)
{
    // Pascal ShortString: first byte = length, then that many chars.
    const BinarySection* sec = view.sectionAt(vma);
    if (!sec) return {};
    uint64_t off = vma - sec->vma;
    if (off >= sec->size) return {};

    uint8_t len = sec->data[off];
    if (len == 0 || len > 255) return {};
    if (off + 1 + len > sec->size) return {};

    std::string s;
    s.reserve(len);
    for (uint8_t i = 0; i < len; ++i) {
        char c = static_cast<char>(sec->data[off + 1 + i]);
        if (!std::isprint(static_cast<unsigned char>(c))) return {};
        s += c;
    }
    return s;
}

// ─── parseTTypeInfo ───────────────────────────────────────────────────────────

void BorlandRttiReconstructor::parseTTypeInfo(
    const BinaryView& view, uint64_t tiVma, BorlandTypeData& out) const
{
    if (!tiVma) return;
    const BinarySection* sec = view.sectionAt(tiVma);
    if (!sec) return;

    uint64_t off = tiVma - sec->vma;
    if (off >= sec->size) return;

    // TTypeInfo: Kind (1 byte) + Name (ShortString)
    TTypeKind kind = static_cast<TTypeKind>(sec->data[off]);
    if (kind != TTypeKind::tkClass) return;

    // Read the Name ShortString length byte
    uint8_t nameLen = (off + 1 < sec->size) ? sec->data[off + 1] : 0;
    if (nameLen == 0 || nameLen > 255) return;

    // TTypeData immediately follows TTypeInfo
    uint64_t tdVma = tiVma + 1 + 1 + nameLen; // Kind + len-byte + name chars

    // TTypeData.ClassType (4 bytes)
    out.classTypeVma = view.read32(tdVma);

    // TTypeData.ParentInfo (4 bytes)
    out.parentInfoVma = view.read32(tdVma + 4);

    // TTypeData.PropCount (2 bytes)
    uint16_t pc;
    {
        const BinarySection* s2 = view.sectionAt(tdVma + 8);
        if (s2) {
            uint64_t o2 = tdVma + 8 - s2->vma;
            if (o2 + 2 <= s2->size)
                std::memcpy(&pc, s2->data + o2, 2);
            else pc = 0;
        } else pc = 0;
    }
    out.propCount = static_cast<int16_t>(pc);

    // TTypeData.UnitName (ShortString after PropCount)
    out.unitName = readShortString(view, tdVma + 10);
}

// ─── isValidVmt ──────────────────────────────────────────────────────────────

bool BorlandRttiReconstructor::isValidVmt(
    const BinaryView& view, uint64_t vmtVma) const
{
    // vmtSelfPtr field is at vmtVma + kVmtSelfPtr (= vmtVma - 76).
    uint64_t selfPtrFieldVma = vmtVma + static_cast<uint64_t>(
        static_cast<int64_t>(kVmtSelfPtr)); // vmtVma - 76

    // The vmtSelfPtr field must contain `vmtVma` itself.
    uint32_t selfPtr = view.read32(selfPtrFieldVma);
    if (selfPtr != static_cast<uint32_t>(vmtVma)) return false;

    // vmtClassName must point to a valid Pascal ShortString.
    uint64_t classNameFieldVma = vmtVma + static_cast<uint64_t>(
        static_cast<int64_t>(kVmtClassName));
    uint32_t classNamePtr = view.read32(classNameFieldVma);
    if (!classNamePtr) return false;
    std::string cn = readShortString(view, classNamePtr);
    if (cn.empty()) return false;

    // vmtInstanceSize must be a plausible value (4..1M).
    uint64_t instSizeFieldVma = vmtVma + static_cast<uint64_t>(
        static_cast<int64_t>(kVmtInstanceSize));
    uint32_t instSize = view.read32(instSizeFieldVma);
    if (instSize < 4 || instSize > 0x100000) return false;

    // At least one virtual method must be in the executable section.
    uint32_t firstVFunc = view.read32(vmtVma);
    if (firstVFunc && !view.inExecutable(firstVFunc)) return false;

    return true;
}

// ─── parseVmt ────────────────────────────────────────────────────────────────

bool BorlandRttiReconstructor::parseVmt(
    const BinaryView& view, uint64_t vmtVma, BorlandVmtInfo& info) const
{
    if (!isValidVmt(view, vmtVma)) return false;

    info.vmtVma = vmtVma;
    info.selfPtrVma = vmtVma + static_cast<uint64_t>(static_cast<int64_t>(kVmtSelfPtr));

    // Class name
    uint32_t cnPtr = view.read32(
        vmtVma + static_cast<uint64_t>(static_cast<int64_t>(kVmtClassName)));
    info.className = readShortString(view, cnPtr);

    // Instance size
    info.instanceSize = view.read32(
        vmtVma + static_cast<uint64_t>(static_cast<int64_t>(kVmtInstanceSize)));

    // Parent VMT pointer (pointer to the parent's VMT pointer variable)
    uint32_t parentPtrField = view.read32(
        vmtVma + static_cast<uint64_t>(static_cast<int64_t>(kVmtParent)));
    if (parentPtrField) {
        // It's a pointer-to-pointer: *parentPtrField = parentVmtVma
        uint32_t parentVmt = view.read32(parentPtrField);
        info.parentVmtVma = parentVmt;
    }

    // TypeInfo
    uint32_t tiPtr = view.read32(
        vmtVma + static_cast<uint64_t>(static_cast<int64_t>(kVmtTypeInfo)));
    info.typeInfoVma = tiPtr;
    if (tiPtr) parseTTypeInfo(view, tiPtr, info.typeData);

    // Virtual method slots
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t slotVma = view.read32(vmtVma + i * 4);
        if (!slotVma) break;
        if (!view.inExecutable(slotVma)) break;
        info.virtualMethodVmas.push_back(slotVma);
    }

    return !info.className.empty();
}

// ─── scanVmts ────────────────────────────────────────────────────────────────

void BorlandRttiReconstructor::scanVmts(
    const BinaryView&             view,
    const std::unordered_set<uint64_t>& funcSet,
    ClassHierarchyGraph&          out)
{
    // Scan for vmtSelfPtr pattern: a 32-bit word that equals its own address
    // minus 76.  The word is at vmtVma + kVmtSelfPtr = vmtVma - 76.
    // So: *p == (uint32_t)(p + 76) where p is the vmtSelfPtr field VMA.

    for (const auto& sec : view.sections) {
        if (sec.executable || !sec.readable || !sec.data) continue;

        for (uint64_t off = 0; off + 4 <= sec.size; off += 4) {
            uint64_t fieldVma = sec.vma + off;
            uint32_t val      = view.read32(fieldVma);
            // vmtVma = fieldVma + 76 (vmtSelfPtr is at vmtVma - 76)
            uint64_t vmtVma   = fieldVma + 76;
            if (val != static_cast<uint32_t>(vmtVma)) continue;

            // Candidate VMT found.  Check it hasn't been processed.
            if (visited_.count(vmtVma)) continue;
            visited_.insert(vmtVma);

            BorlandVmtInfo info;
            if (!parseVmt(view, vmtVma, info)) continue;
            if (info.className.empty()) continue;

            // Build ClassNode
            ClassNode& node = out.classes[info.className];
            node.name         = info.className;
            node.isPolymorphic= true;

            // Vtable info
            VtableInfo vt;
            vt.vtableVma   = vmtVma;
            vt.offsetToTop = 0;
            for (uint64_t fvma : info.virtualMethodVmas) {
                VtableEntry e;
                e.slotVma   = 0; // positional only
                e.targetVma = fvma;
                vt.slots.push_back(e);
            }
            node.vtables.push_back(std::move(vt));

            // Inheritance edge via vmtParent
            if (info.parentVmtVma) {
                BorlandVmtInfo parentInfo;
                if (parseVmt(view, info.parentVmtVma, parentInfo) &&
                    !parentInfo.className.empty())
                {
                    InheritanceEdge e;
                    e.baseName   = parentInfo.className;
                    e.kind       = InheritanceKind::Direct;
                    e.byteOffset = 0;
                    node.bases.push_back(e);

                    // Ensure parent node exists
                    ClassNode& parentNode = out.classes[parentInfo.className];
                    if (parentNode.name.empty()) parentNode.name = parentInfo.className;
                }
            }
        }
    }
}

// ─── reconstruct ─────────────────────────────────────────────────────────────

bool BorlandRttiReconstructor::reconstruct(
    const BinaryView&            view,
    const std::vector<uint64_t>& funcVmas,
    ClassHierarchyGraph&         out)
{
    std::unordered_set<uint64_t> funcSet(funcVmas.begin(), funcVmas.end());
    scanVmts(view, funcSet, out);
    return !out.empty();
}

} // namespace rtti
} // namespace retdec
