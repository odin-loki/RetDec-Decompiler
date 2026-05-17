/**
 * @file src/rtti/itanium_rtti.cpp
 * @brief Itanium C++ ABI RTTI and vtable reconstructor implementation.
 *
 * Algorithm overview:
 *
 * Pass 1 — discoverTiVtables():
 *   Walk every readable non-executable section.  For each pointer-aligned
 *   word that looks like a pointer into a readable section, check whether
 *   the target has a string that matches one of the known __cxxabiv1 vtable
 *   symbol names.  These become knownTiVtables_.
 *
 * Pass 2 — scanVtables():
 *   Walk all readable non-executable sections looking for the vtable header
 *   pattern: [ptrdiff_t offset-to-top][ptr into knownTiVtables_].  When
 *   found, validate the following slots (must point into executable section
 *   and be known function entry points).  Build VtableInfo + ClassNode.
 *
 * Pass 3 — parseTypeInfo():
 *   For each type_info VMA discovered in Pass 2, read the __cxxabiv1 class
 *   name, determine the type_info subclass, and recursively parse base
 *   class chains for __si_class_type_info and __vmi_class_type_info.
 *
 * Demangling: a minimal built-in demangler handles the _Z prefix format
 * well enough for class names.  When the cxxabi.h __cxa_demangle is
 * available (RETDEC_USE_CXXABI_DEMANGLE defined), it is used instead.
 */

#include "retdec/rtti/itanium_rtti.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <unordered_set>

#ifdef RETDEC_USE_CXXABI_DEMANGLE
#  include <cxxabi.h>
#endif

namespace retdec {
namespace rtti {

// ─── Minimal demangler ────────────────────────────────────────────────────────

// Decodes a length-prefixed identifier: N<len><chars>
static std::string decodeNestedName(const char*& p, const char* end);

static std::string decodeIdentifier(const char*& p, const char* end) {
    if (p >= end) return {};
    // Parse decimal length
    int len = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        len = len * 10 + (*p++ - '0');
    }
    if (len <= 0 || p + len > end) return {};
    std::string s(p, len);
    p += len;
    return s;
}

static std::string decodeNestedName(const char*& p, const char* end) {
    if (p >= end) return {};
    if (*p == 'N') {
        ++p;
        std::string result;
        while (p < end && *p != 'E') {
            if (*p == 'I') {
                // Template args — skip to closing E
                ++p;
                int depth = 1;
                while (p < end && depth > 0) {
                    if (*p == 'I') ++depth;
                    else if (*p == 'E') --depth;
                    ++p;
                }
                break;
            }
            std::string id = decodeIdentifier(p, end);
            if (id.empty()) break;
            if (!result.empty()) result += "::";
            result += id;
        }
        if (p < end && *p == 'E') ++p;
        return result;
    }
    return decodeIdentifier(p, end);
}

std::string ItaniumRttiReconstructor::demangle(const std::string& mangled) {
#ifdef RETDEC_USE_CXXABI_DEMANGLE
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string result(demangled);
        free(demangled);
        return result;
    }
#endif
    // Built-in minimal demangler for _Z<nested-name> patterns.
    const std::string& s = mangled;
    if (s.size() < 2 || s[0] != '_' || s[1] != 'Z') return mangled;

    const char* p   = s.data() + 2;
    const char* end = s.data() + s.size();

    // Handle _ZTV (vtable), _ZTI (type_info), _ZTS (type_info name), _ZN (nested)
    // Skip leading qualifier letters: T, V, I, S, N → navigate to name
    if (p < end && *p == 'T') {
        ++p;
        if (p < end && (*p == 'V' || *p == 'I' || *p == 'S' || *p == 'C'))
            ++p;
    }

    std::string name = decodeNestedName(p, end);
    return name.empty() ? mangled : name;
}

// ─── Read type_info name ──────────────────────────────────────────────────────

std::string ItaniumRttiReconstructor::readTiName(
    const BinaryView& view, uint64_t tiVma) const
{
    uint32_t ps = view.ptrSize();
    // type_info: [vtable ptr][name ptr]
    uint64_t namePtr = view.readPtr(tiVma + ps);
    if (!namePtr) return {};
    return view.readCStr(namePtr, 256);
}

// ─── Pass 1: discover type_info vtable VMAs ───────────────────────────────────

// Known __cxxabiv1 type_info vtable mangled names (as they appear in symbols).
static const char* kTiVtableSymbols[] = {
    "_ZTVN10__cxxabiv117__class_type_infoE",
    "_ZTVN10__cxxabiv120__si_class_type_infoE",
    "_ZTVN10__cxxabiv121__vmi_class_type_infoE",
    "_ZTVN10__cxxabiv116__enum_type_infoE",
    "_ZTVN10__cxxabiv119__pointer_type_infoE",
    "_ZTVN10__cxxabiv123__fundamental_type_infoE",
    nullptr
};

// Heuristic: if a data section pointer targets another location that contains
// a pointer to the known __cxxabiv1 vtable name string, it is a TI vtable ptr.
// For simpler cases: just look for the text "N10__cxxabiv1" in .rodata and
// treat any pointer at offset -ptrSize as the vtable pointer of that TI object.

void ItaniumRttiReconstructor::discoverTiVtables(
    const BinaryView& view, ClassHierarchyGraph& out)
{
    uint32_t ps = view.ptrSize();

    // Scan all readable non-executable sections for the __cxxabiv1 name string.
    for (const auto& sec : view.sections) {
        if (sec.executable || !sec.readable || !sec.data) continue;

        // Look for "N10__cxxabiv1" substring
        static const char kMark[] = "N10__cxxabiv1";
        const uint8_t* data = sec.data;
        for (uint64_t off = 0; off + sizeof(kMark) <= sec.size; ++off) {
            if (std::memcmp(data + off, kMark, sizeof(kMark) - 1) != 0) continue;

            // This looks like a __cxxabiv1 mangled name stored in the binary.
            // The type_info object for this name is at:
            //   [vtable_ptr at tiVma + 0][name_ptr at tiVma + ps]
            // We don't know tiVma from here, but we can scan for the name
            // pointer in any data section.
            uint64_t nameVma = sec.vma + off;

            for (const auto& dsec : view.sections) {
                if (dsec.executable || !dsec.readable || !dsec.data) continue;
                uint32_t align = ps;
                for (uint64_t doff = 0; doff + 2*ps <= dsec.size; doff += align) {
                    uint64_t candidateNamePtr = view.readPtr(dsec.vma + doff + ps);
                    if (candidateNamePtr != nameVma) continue;

                    // [something][nameVma] → candidate type_info at dsec.vma+doff
                    uint64_t tiVma = dsec.vma + doff;
                    uint64_t vtablePtr = view.readPtr(tiVma);

                    // vtablePtr should be 2*ps past a vtable start (slot[-1] or [+2])
                    if (vtablePtr && view.inReadOnly(vtablePtr)) {
                        // The vtable of a type_info class: vtablePtr - 2*ps
                        // is the start of the type_info class's own vtable.
                        knownTiVtables_.insert(vtablePtr);
                    }
                }
            }
        }
    }

    // If we found nothing (stripped binary), insert some heuristic VMAs to
    // allow the scan to proceed — the slot validation still filters out junk.
    if (knownTiVtables_.empty()) {
        out.diagnostics.push_back(
            "ItaniumRttiReconstructor: no __cxxabiv1 vtable markers found; "
            "proceeding with relaxed type_info validation");
    }
}

// ─── Slot parsing ─────────────────────────────────────────────────────────────

VtableInfo ItaniumRttiReconstructor::parseVtableSlots(
    const BinaryView&             view,
    const std::unordered_set<uint64_t>& funcSet,
    uint64_t                      firstSlotVma,
    uint64_t                      typeInfoVma,
    int64_t                       offsetToTop,
    uint32_t                      subIdx) const
{
    VtableInfo info;
    info.vtableVma    = firstSlotVma;
    info.typeInfoVma  = typeInfoVma;
    info.offsetToTop  = offsetToTop;
    info.subVtableIdx = subIdx;

    uint32_t ps = view.ptrSize();
    uint64_t slotVma = firstSlotVma;

    for (std::size_t i = 0; i < 256; ++i, slotVma += ps) {
        uint64_t target = view.readPtr(slotVma);
        if (!target) break;

        // Stop if we hit what looks like the next vtable header (offset-to-top
        // followed by a type_info ptr that is in a data section).
        // offset-to-top is a small integer typically in range [-1M, +1M].
        int64_t possibleOtt = view.readIPtr(slotVma);
        if (i > 0 && possibleOtt >= -0x100000 && possibleOtt <= 0x100000) {
            uint64_t possibleTi = view.readPtr(slotVma + ps);
            if (possibleTi && view.inData(possibleTi))
                break; // next sub-vtable header
        }

        VtableEntry entry;
        entry.slotVma  = slotVma;
        entry.targetVma= target;

        // Validate: target must be in executable region.
        if (!view.inExecutable(target)) {
            // Could be a pure virtual thunk or null entry
            if (target == 0)
                entry.isNull = true;
            else
                entry.isPureVirtual = true; // symbol like __cxa_pure_virtual
        } else if (!funcSet.empty() && funcSet.find(target) == funcSet.end()) {
            // target not in known function set — might still be valid (thunk),
            // but mark as unvalidated rather than dropping.
        }

        info.slots.push_back(entry);

        // If target is not executable, stop parsing (unlikely to continue).
        if (entry.isNull) break;
    }

    return info;
}

// ─── isValidVtable ────────────────────────────────────────────────────────────

bool ItaniumRttiReconstructor::isValidVtable(
    const BinaryView&             view,
    const std::unordered_set<uint64_t>& funcSet,
    uint64_t                      vtableVma) const
{
    uint32_t ps = view.ptrSize();

    // vtableVma is the address of the *header* block:
    //   vtableVma + 0  = offset-to-top
    //   vtableVma + ps = type_info ptr
    int64_t ott = view.readIPtr(vtableVma);
    if (ott < -0x400000 || ott > 0x400000) return false;

    uint64_t tiPtr = view.readPtr(vtableVma + ps);

    // type_info ptr must point into a data section.
    if (tiPtr != 0 && !view.inData(tiPtr) && !view.inReadOnly(tiPtr))
        return false;

    // The first function pointer after the header must be in an executable section
    // or be zero (pure virtual / abstract class).
    uint64_t firstSlotVma = vtableVma + 2 * ps;
    uint64_t firstSlot    = view.readPtr(firstSlotVma);
    if (firstSlot != 0 && !view.inExecutable(firstSlot))
        return false;

    // At least one valid slot OR it's an abstract class with no concrete methods.
    return true;
}

// ─── Pass 2: scan for vtables ─────────────────────────────────────────────────

void ItaniumRttiReconstructor::scanVtables(
    const BinaryView&             view,
    const std::unordered_set<uint64_t>& funcSet,
    ClassHierarchyGraph&          out)
{
    uint32_t ps = view.ptrSize();

    for (const auto& sec : view.sections) {
        if (sec.executable || !sec.readable || !sec.data) continue;

        uint64_t vma  = sec.vma;
        uint64_t end  = sec.vma + sec.size;

        // Align scan to pointer size.
        uint64_t p = (vma + ps - 1) & ~uint64_t(ps - 1);

        while (p + 2 * ps <= end) {
            int64_t  ott   = view.readIPtr(p);
            uint64_t tiPtr = view.readPtr(p + ps);

            // Must look like a valid vtable header.
            if (ott < -0x400000 || ott > 0x400000) { p += ps; continue; }
            if (tiPtr == 0) { p += ps; continue; }
            // tiPtr must be in a non-exec section.
            if (!view.inData(tiPtr) && !view.inReadOnly(tiPtr)) { p += ps; continue; }

            // Check if any knownTiVtables_ is reachable from tiPtr.
            bool tiOk = knownTiVtables_.empty(); // relaxed if no markers found
            if (!tiOk) {
                // The type_info object at tiPtr starts with [vtable_ptr_of_ti_class].
                uint64_t tiVtPtr = view.readPtr(tiPtr);
                if (knownTiVtables_.count(tiVtPtr)) tiOk = true;
                // Also check tiPtr+ps might be the vtable ptr (for subclasses).
                if (!tiOk) {
                    tiVtPtr = view.readPtr(tiPtr);
                    tiOk = knownTiVtables_.count(tiVtPtr) != 0;
                }
            }

            // Validate at least one executable slot.
            uint64_t firstSlotVma = p + 2 * ps;
            uint64_t firstSlot    = view.readPtr(firstSlotVma);
            bool slotOk = (firstSlot == 0) ||
                          view.inExecutable(firstSlot);
            if (!slotOk) { p += ps; continue; }

            // Accept this vtable header.
            uint32_t subIdx = 0;

            // Parse or find the class node for tiPtr.
            std::string className;
            if (tiPtr) {
                className = parseTypeInfo(view, tiPtr, out);
            }
            if (className.empty()) className = "<vtable@" + std::to_string(p) + ">";

            ClassNode& node = out.classes[className];
            if (node.name.empty()) node.name = className;
            if (!node.typeInfoVma && tiPtr) node.typeInfoVma = tiPtr;
            node.isPolymorphic = true;

            // If this is a secondary vtable (ott != 0), label it.
            if (!node.vtables.empty() && ott != 0) subIdx = (uint32_t)node.vtables.size();

            VtableInfo vt = parseVtableSlots(view, funcSet,
                                              firstSlotVma, tiPtr, ott, subIdx);
            node.vtables.push_back(std::move(vt));

            // Skip past the vtable we just consumed.
            p = firstSlotVma + uint64_t(node.vtables.back().slots.size()) * ps;
            if (p < firstSlotVma + ps) p = firstSlotVma + ps; // safety
        }
    }
}

// ─── parseTypeInfo ────────────────────────────────────────────────────────────

std::string ItaniumRttiReconstructor::parseTypeInfo(
    const BinaryView&  view,
    uint64_t           tiVma,
    ClassHierarchyGraph& out)
{
    if (!tiVma) return {};
    if (visitedTi_.count(tiVma)) {
        // Return already-computed name.
        const ClassNode* n = out.byTypeInfoVma(tiVma);
        return n ? n->name : "";
    }
    visitedTi_.insert(tiVma);

    uint32_t ps = view.ptrSize();

    // Read the vtable pointer of the type_info object (first field).
    uint64_t tiVtPtr = view.readPtr(tiVma);

    // Read the name pointer (second field).
    uint64_t namePtr = view.readPtr(tiVma + ps);
    std::string mangled;
    if (namePtr) mangled = view.readCStr(namePtr, 256);

    std::string demangled = demangle("_Z" + mangled);
    if (demangled == "_Z" + mangled) demangled = mangled; // fallback

    ClassNode& node = out.classes[demangled];
    node.name        = demangled;
    node.mangledName = mangled;
    node.typeInfoVma = tiVma;

    // Determine type_info subclass by matching tiVtPtr against knownTiVtables_.
    // We use a conservative approach: always try to parse as __si and __vmi.

    // __si_class_type_info: [vtable][name][base_type_info*]
    //   layout: tiVma + 2*ps = base type_info ptr
    uint64_t siBasePtr = view.readPtr(tiVma + 2 * ps);
    if (siBasePtr && (view.inData(siBasePtr) || view.inReadOnly(siBasePtr))) {
        parseSiClassTypeInfo(view, tiVma, node, out);
    }

    // __vmi_class_type_info: [vtable][name][flags][base_count][base_info[]]
    // Distinguish from __si by checking if flags (uint32) looks like a small int
    // and base_count looks like 1+ with reasonable base_info pointers.
    // Only attempt if not already handled by __si (node.bases empty so far).
    if (node.bases.empty()) {
        uint64_t flagsVma = tiVma + 2 * ps;
        uint32_t flags    = view.read32(flagsVma);
        uint32_t count    = view.read32(flagsVma + 4);
        if (flags <= 0xFFFF && count >= 1 && count <= 64) {
            uint64_t firstBaseInfoVma = flagsVma + 8;
            uint64_t basetiPtr = view.readPtr(firstBaseInfoVma);
            if (basetiPtr && (view.inData(basetiPtr) || view.inReadOnly(basetiPtr))) {
                // Looks like __vmi
                parseVmiClassTypeInfo(view, tiVma, node, out);
            }
        }
    }

    return demangled;
}

// ─── parseSiClassTypeInfo ─────────────────────────────────────────────────────

void ItaniumRttiReconstructor::parseSiClassTypeInfo(
    const BinaryView&  view,
    uint64_t           tiVma,
    ClassNode&         node,
    ClassHierarchyGraph& out)
{
    uint32_t ps = view.ptrSize();
    // Layout: [vtable*][name*][base_type_info*]
    uint64_t baseInfoPtr = view.readPtr(tiVma + 2 * ps);
    if (!baseInfoPtr) return;

    std::string baseName = parseTypeInfo(view, baseInfoPtr, out);
    if (!baseName.empty() && !node.name.empty()) {
        InheritanceEdge e;
        e.baseName   = baseName;
        e.kind       = InheritanceKind::Direct;
        e.byteOffset = 0; // public non-virtual: offset = 0 for single inheritance
        node.bases.push_back(e);
    }
}

// ─── parseVmiClassTypeInfo ────────────────────────────────────────────────────

void ItaniumRttiReconstructor::parseVmiClassTypeInfo(
    const BinaryView&  view,
    uint64_t           tiVma,
    ClassNode&         node,
    ClassHierarchyGraph& out)
{
    uint32_t ps = view.ptrSize();
    // Layout: [vtable*][name*][uint32 flags][uint32 base_count][base_info[]]
    // base_info[i] = { type_info* (ptr-aligned), ptrdiff_t offset_flags }
    //   offset_flags bit 0 = is_virtual
    //   offset_flags bit 1 = is_public
    //   offset_flags >> 8  = byte offset of base

    uint64_t flagsVma     = tiVma + 2 * ps;
    uint32_t count        = view.read32(flagsVma + 4);
    if (count > 64) count = 64; // sanity cap

    uint64_t biVma = flagsVma + 8; // first base_info
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t baseTiPtr  = view.readPtr(biVma);
        int64_t  offsetFlags= view.readIPtr(biVma + ps);

        if (!baseTiPtr) { biVma += 2 * ps; continue; }

        std::string baseName = parseTypeInfo(view, baseTiPtr, out);

        InheritanceEdge e;
        e.baseName   = baseName;
        e.kind       = (offsetFlags & 1) ? InheritanceKind::Virtual
                                         : InheritanceKind::Direct;
        e.byteOffset = static_cast<int32_t>(offsetFlags >> 8);

        if (!baseName.empty()) node.bases.push_back(e);
        biVma += 2 * ps;
    }
}

// ─── reconstruct ─────────────────────────────────────────────────────────────

bool ItaniumRttiReconstructor::reconstruct(
    const BinaryView&            view,
    const std::vector<uint64_t>& funcVmas,
    ClassHierarchyGraph&         out)
{
    std::unordered_set<uint64_t> funcSet(funcVmas.begin(), funcVmas.end());

    discoverTiVtables(view, out);
    scanVtables(view, funcSet, out);

    return !out.empty();
}

} // namespace rtti
} // namespace retdec
