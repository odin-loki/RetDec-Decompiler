/**
 * @file src/rtti/dmc_rtti.cpp
 * @brief Digital Mars C++ (DMC) RTTI reconstructor.
 */

#include "retdec/rtti/dmc_rtti.h"

#include <cctype>
#include <cstring>

namespace retdec {
namespace rtti {

// ─── isValidTypeInfo ─────────────────────────────────────────────────────────

bool DmcRttiReconstructor::isValidTypeInfo(
    const BinaryView& view, uint64_t tiVma) const
{
    if (!tiVma) return false;
    // __typeinfo: [vptr (data)][name_ptr (data)]
    uint32_t ps = view.ptrSize();

    uint64_t vptr    = view.readPtr(tiVma);
    uint64_t namePtr = view.readPtr(tiVma + ps);

    // vptr must point into a data section (the __typeinfo class vtable)
    if (!vptr) return false;
    if (!view.inData(vptr) && !view.inReadOnly(vptr)) return false;

    // name_ptr must point to a readable string
    if (!namePtr) return false;
    if (!view.inData(namePtr) && !view.inReadOnly(namePtr)) return false;

    std::string name = view.readCStr(namePtr, 256);
    if (name.empty()) return false;
    // Must be printable ASCII
    for (char c : name)
        if (!std::isprint(static_cast<unsigned char>(c))) return false;

    return true;
}

// ─── parseTypeInfo ───────────────────────────────────────────────────────────

std::string DmcRttiReconstructor::parseTypeInfo(
    const BinaryView& view, uint64_t tiVma, ClassHierarchyGraph& out)
{
    if (!tiVma || visited_.count(tiVma)) {
        const ClassNode* n = out.byTypeInfoVma(tiVma);
        return n ? n->name : "";
    }
    visited_.insert(tiVma);

    if (!isValidTypeInfo(view, tiVma)) return {};

    uint32_t ps = view.ptrSize();

    // name
    uint64_t namePtr = view.readPtr(tiVma + ps);
    std::string className = view.readCStr(namePtr, 256);
    if (className.empty()) return {};

    ClassNode& node = out.classes[className];
    node.name        = className;
    node.typeInfoVma = tiVma;
    node.isPolymorphic = true;

    // __DMCclassinfo has: [vptr][name][size][base_ptr]
    // Try to read base pointer at tiVma + 2*ps + 4 (after size field)
    uint64_t sizeFieldVma = tiVma + 2 * ps;
    uint32_t classSize = view.read32(sizeFieldVma);
    if (classSize > 0 && classSize < 0x100000) {
        // Looks like __DMCclassinfo
        uint64_t basePtr = view.readPtr(sizeFieldVma + 4);
        if (basePtr && isValidTypeInfo(view, basePtr)) {
            std::string baseName = parseTypeInfo(view, basePtr, out);
            if (!baseName.empty()) {
                InheritanceEdge e;
                e.baseName   = baseName;
                e.kind       = InheritanceKind::Direct;
                e.byteOffset = 0;
                node.bases.push_back(e);
            }
        }
    }

    return className;
}

// ─── scanVtables ─────────────────────────────────────────────────────────────

void DmcRttiReconstructor::scanVtables(
    const BinaryView&             view,
    const std::unordered_set<uint64_t>& funcSet,
    ClassHierarchyGraph&          out)
{
    uint32_t ps = view.ptrSize();

    for (const auto& sec : view.sections) {
        if (sec.executable || !sec.readable || !sec.data) continue;

        uint64_t vma = sec.vma;
        uint64_t end = sec.vma + sec.size;
        uint64_t p   = (vma + ps - 1) & ~uint64_t(ps - 1);

        while (p + ps <= end) {
            uint64_t candidate = view.readPtr(p);

            if (isValidTypeInfo(view, candidate)) {
                // p is a vtable[-1] slot; vtable[0] is at p + ps
                uint64_t vtableVma = p + ps;
                uint64_t firstSlot = view.readPtr(vtableVma);

                if (firstSlot && view.inExecutable(firstSlot)) {
                    std::string className = parseTypeInfo(view, candidate, out);
                    if (!className.empty()) {
                        // Parse vtable slots
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

            p += ps;
        }
    }
}

// ─── reconstruct ─────────────────────────────────────────────────────────────

bool DmcRttiReconstructor::reconstruct(
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
