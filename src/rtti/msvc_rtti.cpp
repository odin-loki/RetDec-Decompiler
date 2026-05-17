/**
 * @file src/rtti/msvc_rtti.cpp
 * @brief MSVC ABI RTTI and vtable reconstructor implementation.
 *
 * Algorithm overview:
 *
 * scanVtables():
 *   Walk every readable non-executable section.  For each pointer-aligned
 *   word that:
 *     1. Points into a readable non-executable section (COL candidate)
 *     2. The COL candidate has signature == 0 or 1 (MSVC COL signature)
 *   → parse the COL to get the class name + hierarchy.
 *
 * parseCol():
 *   Reads _RTTICompleteObjectLocator fields.  On 32-bit: absolute pointers.
 *   On 64-bit: RVAs relative to the module base (imageBase).
 *   Follows: COL → TypeDescriptor (mangled name) → ClassHierarchyDescriptor
 *   → BaseClassArray → each BaseClassDescriptor.
 *
 * parseClassHierarchy():
 *   Reads _RTTIClassHierarchyDescriptor: signature, attributes, numBaseClasses,
 *   pBaseClassArray.  Iterates each BaseClassDescriptor and reads PMD fields.
 *   First entry in the array is always the class itself (skip it).
 *   Remaining entries are base classes in declaration order.
 *
 * Demangling:
 *   MSVC names start with '?'.  The demangler strips leading '?' prefix,
 *   reverses '@'-separated components, and returns "Outer::Inner" form.
 */

#include "retdec/rtti/msvc_rtti.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace retdec {
namespace rtti {

// ─── Minimal MSVC demangler ───────────────────────────────────────────────────

std::string MsvcRttiReconstructor::demangle(const std::string& mangled) {
    // MSVC type descriptor names start with '.' or '?' after the two null bytes.
    // The raw name stored in TypeDescriptor::name[] is like ".?AVFoo@@" or
    // ".?AVFoo@Bar@@".
    // Strip the leading '.', '?', 'A', 'V'/'U'/'T' (class/struct/union marker).
    const std::string& s = mangled;
    if (s.empty()) return s;

    std::size_t start = 0;
    while (start < s.size() && (s[start] == '.' || s[start] == '?'))
        ++start;
    // Skip type tag: AV (class), AU (struct), AT (union)
    if (start + 1 < s.size() && s[start] == 'A' &&
        (s[start+1] == 'V' || s[start+1] == 'U' || s[start+1] == 'T')) {
        start += 2;
    }
    if (start >= s.size()) return s;

    // Now we have something like "Foo@@" or "Foo@Bar@@".
    // Split by '@', filter empty, reverse for namespace order.
    std::vector<std::string> parts;
    std::size_t pos = start;
    while (pos < s.size()) {
        std::size_t at = s.find('@', pos);
        if (at == std::string::npos) {
            std::string part = s.substr(pos);
            if (!part.empty()) parts.push_back(part);
            break;
        }
        std::string part = s.substr(pos, at - pos);
        if (!part.empty()) parts.push_back(part);
        pos = at + 1;
    }

    if (parts.empty()) return s;

    // Reverse: last part is outer namespace, first part is class name.
    // The MSVC mangling has class first, then outer scopes.
    // So: "Foo@Bar@Baz@@" → parts = ["Foo", "Bar", "Baz"] → "Baz::Bar::Foo"
    std::reverse(parts.begin(), parts.end());
    // Drop trailing empty markers like "@"
    std::string result;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) result += "::";
        result += parts[i];
    }
    return result;
}

// ─── Read type descriptor name ────────────────────────────────────────────────

std::string MsvcRttiReconstructor::readTdName(
    const BinaryView& view, uint64_t tdVma) const
{
    uint32_t ps = view.ptrSize();
    // _RTTITypeDescriptor: [vtable*][spare*][name[]]
    uint64_t nameVma = tdVma + 2 * ps;
    return view.readCStr(nameVma, 256);
}

// ─── readPtrOrRva ─────────────────────────────────────────────────────────────

uint64_t MsvcRttiReconstructor::readPtrOrRva(
    const BinaryView& view,
    uint64_t          fieldVma,
    uint64_t          imageBase) const
{
    if (view.is64bit) {
        // 64-bit MSVC stores RVAs (4 bytes)
        uint32_t rva = view.read32(fieldVma);
        return imageBase + rva;
    }
    return view.readPtr(fieldVma); // 32-bit: absolute pointer
}

// ─── isValidCol ──────────────────────────────────────────────────────────────

bool MsvcRttiReconstructor::isValidCol(const BinaryView& view, uint64_t colVma) const {
    if (!view.inData(colVma) && !view.inReadOnly(colVma)) return false;

    // _RTTICompleteObjectLocator::signature must be 0 (32-bit) or 1 (64-bit)
    uint32_t sig = view.read32(colVma);
    if (sig != 0 && sig != 1) return false;

    uint32_t ps = view.ptrSize();
    uint64_t imageBase = view.imageBase;

    // offset (field 1) — must be small non-negative
    uint32_t offset = view.read32(colVma + 4);
    if (offset > 0x100000) return false;

    // cdOffset (field 2) — constructor displacement, often 0
    // pTypeDescriptor (field 3)
    uint64_t tdVma;
    if (view.is64bit) {
        // 64-bit: RVA at offset 12
        uint32_t tdRva = view.read32(colVma + 12);
        tdVma = imageBase + tdRva;
    } else {
        tdVma = view.readPtr(colVma + 12);
    }

    if (!tdVma) return false;
    if (!view.inData(tdVma) && !view.inReadOnly(tdVma)) return false;

    // Type descriptor starts with [vtable ptr][spare].
    // The name immediately after should start with ".?" (MSVC type tag).
    std::string name = readTdName(view, tdVma);
    if (name.empty()) return false;
    if (name[0] != '.' && name[0] != '?') return false;

    return true;
}

// ─── parseClassHierarchy ─────────────────────────────────────────────────────

bool MsvcRttiReconstructor::parseClassHierarchy(
    const BinaryView&  view,
    uint64_t           hierVma,
    uint64_t           imageBase,
    ClassNode&         node,
    ClassHierarchyGraph& out)
{
    if (!hierVma) return false;
    if (!view.inData(hierVma) && !view.inReadOnly(hierVma)) return false;

    // _RTTIClassHierarchyDescriptor:
    //   uint32 signature  (0)
    //   uint32 attributes (bit0=MI, bit1=virtual)
    //   uint32 numBaseClasses
    //   ptr/rva to BaseClassArray

    uint32_t sig   = view.read32(hierVma);
    if (sig != 0) return false;

    uint32_t attrs = view.read32(hierVma + 4);
    uint32_t count = view.read32(hierVma + 8);
    if (count > 256) count = 256;

    uint64_t bcaVma; // BaseClassArray
    if (view.is64bit) {
        uint32_t bcaRva = view.read32(hierVma + 12);
        bcaVma = imageBase + bcaRva;
    } else {
        bcaVma = view.readPtr(hierVma + 12);
    }

    if (!bcaVma) return false;
    if (!view.inData(bcaVma) && !view.inReadOnly(bcaVma)) return false;

    uint32_t ps = view.ptrSize();

    // Iterate each BaseClassDescriptor pointer (or RVA).
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t bcdVma = readPtrOrRva(view, bcaVma + uint64_t(i) * (view.is64bit ? 4 : ps), imageBase);
        if (!bcdVma) continue;
        if (!view.inData(bcdVma) && !view.inReadOnly(bcdVma)) continue;

        // _RTTIBaseClassDescriptor:
        //   ptr/rva  pTypeDescriptor
        //   uint32   numContainedBases
        //   int32    PMD.mdisp
        //   int32    PMD.pdisp
        //   int32    PMD.vdisp
        //   uint32   attributes

        uint64_t tdVma = readPtrOrRva(view, bcdVma, imageBase);
        uint32_t numContained = view.read32(bcdVma + (view.is64bit ? 4 : ps));
        uint64_t pmdBase = bcdVma + (view.is64bit ? 8 : ps + 4);
        int32_t  mdisp   = view.readI32(pmdBase);
        int32_t  pdisp   = view.readI32(pmdBase + 4);
        int32_t  vdisp   = view.readI32(pmdBase + 8);
        uint32_t battr   = view.read32(pmdBase + 12);

        // Skip first entry (the class itself)
        if (i == 0) continue;

        if (!tdVma) continue;
        std::string mangled  = readTdName(view, tdVma);
        std::string baseName = demangle(mangled);
        if (baseName.empty()) baseName = mangled;

        // Ensure base class node exists
        ClassNode& baseNode = out.classes[baseName];
        if (baseNode.name.empty()) baseNode.name = baseName;

        InheritanceEdge e;
        e.baseName      = baseName;
        e.byteOffset    = mdisp;
        e.vbaseOffset   = pdisp;
        e.vdispOffset   = vdisp;
        e.numContained  = numContained;
        // virtual if pdisp != -1
        e.kind = (pdisp != -1) ? InheritanceKind::Virtual : InheritanceKind::Direct;

        // Avoid duplicate edges
        bool dup = false;
        for (const auto& eb : node.bases)
            if (eb.baseName == baseName && eb.byteOffset == mdisp) { dup=true; break; }
        if (!dup) node.bases.push_back(e);
    }

    return true;
}

// ─── parseCol ────────────────────────────────────────────────────────────────

std::string MsvcRttiReconstructor::parseCol(
    const BinaryView&  view,
    uint64_t           colVma,
    ClassHierarchyGraph& out)
{
    if (!colVma || visitedCol_.count(colVma)) {
        const ClassNode* n = out.byColVma(colVma);
        return n ? n->name : "";
    }
    visitedCol_.insert(colVma);

    uint64_t imageBase = view.imageBase;

    // COL fields
    // sig=0/1, offset, cdOffset, pTypeDescriptor (ptr or RVA), pClassDescriptor
    uint64_t tdVma;
    uint64_t hierVma;
    if (view.is64bit) {
        uint32_t tdRva   = view.read32(colVma + 12);
        uint32_t hierRva = view.read32(colVma + 16);
        tdVma   = imageBase + tdRva;
        hierVma = imageBase + hierRva;
    } else {
        tdVma   = view.readPtr(colVma + 12);
        hierVma = view.readPtr(colVma + 16);
    }

    std::string mangled  = readTdName(view, tdVma);
    std::string className= demangle(mangled);
    if (className.empty()) className = mangled;
    if (className.empty()) return {};

    ClassNode& node = out.classes[className];
    node.name        = className;
    node.mangledName = mangled;
    node.colVma      = colVma;
    node.isPolymorphic = true;

    parseClassHierarchy(view, hierVma, imageBase, node, out);

    return className;
}

// ─── parseVtableSlots ────────────────────────────────────────────────────────

VtableInfo MsvcRttiReconstructor::parseVtableSlots(
    const BinaryView&             view,
    const std::unordered_set<uint64_t>& funcSet,
    uint64_t                      vtableVma) const
{
    VtableInfo info;
    info.vtableVma = vtableVma;
    uint32_t ps = view.ptrSize();

    for (std::size_t i = 0; i < 256; ++i) {
        uint64_t slotVma = vtableVma + uint64_t(i) * ps;
        uint64_t target  = view.readPtr(slotVma);
        if (!target) break;
        if (!view.inExecutable(target) && target != 0) {
            // Could be pure virtual thunk; stop.
            break;
        }
        VtableEntry e;
        e.slotVma   = slotVma;
        e.targetVma = target;
        e.isNull    = (target == 0);
        info.slots.push_back(e);
    }
    return info;
}

// ─── scanVtables ─────────────────────────────────────────────────────────────

void MsvcRttiReconstructor::scanVtables(
    const BinaryView&             view,
    const std::unordered_set<uint64_t>& funcSet,
    ClassHierarchyGraph&          out)
{
    uint32_t ps = view.ptrSize();
    uint64_t imageBase = view.imageBase;

    for (const auto& sec : view.sections) {
        if (sec.executable || !sec.readable || !sec.data) continue;

        uint64_t vma = sec.vma;
        uint64_t end = sec.vma + sec.size;
        uint64_t p   = (vma + ps - 1) & ~uint64_t(ps - 1);

        while (p + ps <= end) {
            uint64_t candidateColVma;
            if (view.is64bit) {
                // In 64-bit MSVC, the vtable[-1] field is a 4-byte RVA to the COL.
                uint32_t rva = view.read32(p);
                candidateColVma = imageBase + rva;
            } else {
                candidateColVma = view.readPtr(p);
            }

            if (isValidCol(view, candidateColVma)) {
                // p is the vtable[-1] field, so the vtable starts at p + ps.
                uint64_t vtableVma = p + ps;
                uint64_t firstSlot = view.readPtr(vtableVma);
                if (firstSlot && view.inExecutable(firstSlot)) {
                    std::string className = parseCol(view, candidateColVma, out);
                    if (!className.empty()) {
                        VtableInfo vt = parseVtableSlots(view, funcSet, vtableVma);
                        out.classes[className].vtables.push_back(std::move(vt));
                    }
                }
            }

            p += ps;
        }
    }
}

// ─── reconstruct ─────────────────────────────────────────────────────────────

bool MsvcRttiReconstructor::reconstruct(
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
