/**
 * @file src/rtti/watcom_rtti.cpp
 * @brief Open Watcom C++ RTTI reconstructor.
 */

#include "retdec/rtti/watcom_rtti.h"

#include <cctype>
#include <cstring>

namespace retdec {
namespace rtti {

// ─── isValidTypeInfo ─────────────────────────────────────────────────────────

bool WatcomRttiReconstructor::isValidTypeInfo(
    const BinaryView& view, uint64_t tiVma) const
{
    if (!tiVma) return false;
    uint32_t ps = view.ptrSize();

    // __WatcomTypeInfo: [uint16 kind][pad?][const char* name][__WatcomTypeInfo* base]
    // On 32-bit: [2-byte kind][2-byte pad][4-byte name ptr][4-byte base ptr]
    // kind must be 0..4
    uint16_t kind;
    {
        const BinarySection* s = view.sectionAt(tiVma);
        if (!s) return false;
        uint64_t off = tiVma - s->vma;
        if (off + 2 > s->size) return false;
        std::memcpy(&kind, s->data + off, 2);
    }
    if (kind > 4) return false;

    // name pointer (at offset 4 on 32-bit, aligns to pointer size)
    uint64_t namePtrFieldVma = (tiVma + ps) & ~uint64_t(ps - 1);
    // Actually on 32-bit Watcom it's at offset 4 (after 2-byte kind + 2-byte pad):
    namePtrFieldVma = tiVma + 4;
    uint64_t namePtr = view.readPtr(namePtrFieldVma);
    if (!namePtr) return false;
    if (!view.inData(namePtr) && !view.inReadOnly(namePtr)) return false;

    std::string name = view.readCStr(namePtr, 256);
    if (name.empty()) return false;
    for (char c : name)
        if (!std::isprint(static_cast<unsigned char>(c))) return false;

    return true;
}

// ─── parseTypeInfo ───────────────────────────────────────────────────────────

std::string WatcomRttiReconstructor::parseTypeInfo(
    const BinaryView& view, uint64_t tiVma, ClassHierarchyGraph& out)
{
    if (!tiVma || visited_.count(tiVma)) {
        const ClassNode* n = out.byTypeInfoVma(tiVma);
        return n ? n->name : "";
    }
    visited_.insert(tiVma);

    if (!isValidTypeInfo(view, tiVma)) return {};

    // kind at offset 0 (16-bit)
    uint16_t kind = 0;
    {
        const BinarySection* s = view.sectionAt(tiVma);
        if (s) {
            uint64_t off = tiVma - s->vma;
            if (off + 2 <= s->size) std::memcpy(&kind, s->data + off, 2);
        }
    }

    uint64_t namePtrFieldVma = tiVma + 4;
    uint64_t namePtr = view.readPtr(namePtrFieldVma);
    std::string className = view.readCStr(namePtr, 256);
    if (className.empty()) return {};

    ClassNode& node = out.classes[className];
    node.name        = className;
    node.typeInfoVma = tiVma;
    node.isPolymorphic = (kind == static_cast<uint16_t>(WatcomTypeKind::Class));

    // base pointer at tiVma + 4 + ptrSize
    uint32_t ps = view.ptrSize();
    uint64_t basePtrVma = namePtrFieldVma + ps;
    uint64_t basePtr = view.readPtr(basePtrVma);
    if (basePtr && isValidTypeInfo(view, basePtr)) {
        std::string baseName = parseTypeInfo(view, basePtr, out);
        if (!baseName.empty() && baseName != className) {
            InheritanceEdge e;
            e.baseName   = baseName;
            e.kind       = InheritanceKind::Direct;
            e.byteOffset = 0;
            node.bases.push_back(e);
        }
    }

    return className;
}

// ─── scanVtables ─────────────────────────────────────────────────────────────

void WatcomRttiReconstructor::scanVtables(
    const BinaryView&             view,
    const std::unordered_set<uint64_t>& funcSet,
    ClassHierarchyGraph&          out)
{
    uint32_t ps = view.ptrSize();

    for (const auto& sec : view.sections) {
        if (sec.executable || !sec.readable || !sec.data) continue;

        uint64_t vma = sec.vma;
        uint64_t end = sec.vma + sec.size;
        uint64_t p   = (vma + 3) & ~uint64_t(3); // 4-byte align

        while (p + ps <= end) {
            uint64_t candidate = view.readPtr(p);

            if (isValidTypeInfo(view, candidate)) {
                uint64_t vtableVma = p + ps;
                uint64_t firstSlot = view.readPtr(vtableVma);
                if (firstSlot && view.inExecutable(firstSlot)) {
                    std::string className = parseTypeInfo(view, candidate, out);
                    if (!className.empty()) {
                        VtableInfo vt;
                        vt.vtableVma = vtableVma;
                        for (uint32_t i = 0; i < 256; ++i) {
                            uint64_t slotVma = vtableVma + uint64_t(i) * ps;
                            uint64_t target  = view.readPtr(slotVma);
                            if (!target || !view.inExecutable(target)) break;
                            VtableEntry e;
                            e.slotVma   = slotVma;
                            e.targetVma = target;
                            vt.slots.push_back(e);
                        }
                        out.classes[className].vtables.push_back(std::move(vt));
                    }
                }
            }

            p += 4; // Watcom type-info is 4-byte aligned
        }
    }
}

// ─── reconstruct ─────────────────────────────────────────────────────────────

bool WatcomRttiReconstructor::reconstruct(
    const BinaryView&            view,
    const std::vector<uint64_t>& funcVmas,
    ClassHierarchyGraph&         out)
{
    std::unordered_set<uint64_t> funcSet(funcVmas.begin(), funcVmas.end());
    scanVtables(view, funcSet, out);
    return !out.empty();
}

} // namespace rtti
} // namespace retdec
