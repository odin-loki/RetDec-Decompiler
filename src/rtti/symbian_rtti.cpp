/**
 * @file src/rtti/symbian_rtti.cpp
 * @brief Symbian OS / EPOC C++ RTTI reconstructor.
 */

#include "retdec/rtti/symbian_rtti.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace retdec {
namespace rtti {

// ─── isSymbianClassName ──────────────────────────────────────────────────────

bool SymbianRttiReconstructor::isSymbianClassName(const std::string& name) {
    if (name.size() < 2) return false;
    // Must start with C, T, R, M, E followed by uppercase
    if (name[0] != 'C' && name[0] != 'T' && name[0] != 'R' &&
        name[0] != 'M' && name[0] != 'E') return false;
    if (!std::isupper(static_cast<unsigned char>(name[1]))) return false;
    // Remaining must be alphanumeric or underscore
    for (std::size_t i = 2; i < name.size(); ++i) {
        char c = name[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return true;
}

// ─── isValidTMetaClass ───────────────────────────────────────────────────────

bool SymbianRttiReconstructor::isValidTMetaClass(
    const BinaryView& view, uint64_t vma) const
{
    if (!vma) return false;
    uint32_t ps = view.ptrSize();

    // TMetaClass: [iSize (uint32)][iOffset (uint32)][iName (ptr)][iParent (ptr)]
    uint32_t iSize   = view.read32(vma);
    uint32_t iOffset = view.read32(vma + 4);

    // iSize must be reasonable (4..64 KB)
    if (iSize < 4 || iSize > 0x10000) return false;
    // iOffset must be 0 or small non-negative
    if (iOffset > 0x1000) return false;

    // iName pointer
    uint64_t namePtr = view.readPtr(vma + 8);
    if (!namePtr) return false;
    if (!view.inData(namePtr) && !view.inReadOnly(namePtr)) return false;

    std::string name = view.readCStr(namePtr, 64);
    if (!isSymbianClassName(name)) return false;

    // iParent: 0 (CBase) or another TMetaClass pointer
    uint64_t parentPtr = view.readPtr(vma + 8 + ps);
    if (parentPtr && !view.inData(parentPtr) && !view.inReadOnly(parentPtr))
        return false;

    return true;
}

// ─── parseTMetaClass ─────────────────────────────────────────────────────────

bool SymbianRttiReconstructor::parseTMetaClass(
    const BinaryView& view, uint64_t vma, TMetaClassInfo& info) const
{
    if (!isValidTMetaClass(view, vma)) return false;
    uint32_t ps = view.ptrSize();

    info.vma     = vma;
    info.iSize   = view.read32(vma);
    info.iOffset = view.read32(vma + 4);

    uint64_t namePtr = view.readPtr(vma + 8);
    info.iName       = view.readCStr(namePtr, 64);
    info.iParentVma  = view.readPtr(vma + 8 + ps);

    return !info.iName.empty();
}

// ─── scanTMetaClass ──────────────────────────────────────────────────────────

void SymbianRttiReconstructor::scanTMetaClass(
    const BinaryView& view, ClassHierarchyGraph& out)
{
    uint32_t ps = view.ptrSize();

    for (const auto& sec : view.sections) {
        if (sec.executable || !sec.readable || !sec.data) continue;

        uint64_t vma = sec.vma;
        uint64_t end = sec.vma + sec.size;
        uint64_t p   = (vma + 3) & ~uint64_t(3);

        while (p + 8 + 2 * ps <= end) {
            TMetaClassInfo info;
            if (parseTMetaClass(view, p, info)) {
                if (visited_.count(p)) { p += 4; continue; }
                visited_.insert(p);

                ClassNode& node = out.classes[info.iName];
                node.name        = info.iName;
                node.isPolymorphic = (info.iName[0] == 'C' || info.iName[0] == 'M');

                // Parent edge
                if (info.iParentVma) {
                    TMetaClassInfo parentInfo;
                    if (parseTMetaClass(view, info.iParentVma, parentInfo) &&
                        !parentInfo.iName.empty()) {
                        InheritanceEdge e;
                        e.baseName   = parentInfo.iName;
                        e.kind       = InheritanceKind::Direct;
                        e.byteOffset = static_cast<int32_t>(info.iOffset);
                        node.bases.push_back(e);

                        ClassNode& parentNode = out.classes[parentInfo.iName];
                        if (parentNode.name.empty()) parentNode.name = parentInfo.iName;
                    }
                }

                p += 4 + 4 + 2 * ps; // skip past this TMetaClass
                continue;
            }
            p += 4;
        }
    }
}

// ─── scanNamePattern ─────────────────────────────────────────────────────────

void SymbianRttiReconstructor::scanNamePattern(
    const BinaryView&             view,
    const std::unordered_set<uint64_t>& funcSet,
    ClassHierarchyGraph&          out)
{
    // Fallback: scan read-only sections for Symbian-style class name strings
    // (C*/M*) that appear near vtable-like structures.
    uint32_t ps = view.ptrSize();

    for (const auto& sec : view.sections) {
        if (sec.executable || !sec.readable || !sec.data) continue;

        for (uint64_t off = 0; off + 2 < sec.size; ++off) {
            char first = static_cast<char>(sec.data[off]);
            if (first != 'C' && first != 'M') continue;
            char second = static_cast<char>(sec.data[off + 1]);
            if (!std::isupper(static_cast<unsigned char>(second))) continue;

            // Read the full name
            std::string name;
            for (uint64_t k = off; k < std::min(sec.size, off + 64); ++k) {
                char c = static_cast<char>(sec.data[k]);
                if (c == '\0') break;
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') break;
                name += c;
            }
            if (name.size() < 3 || !isSymbianClassName(name)) continue;

            // Check if there's a nearby vtable in data pointing to exec
            uint64_t nameVma = sec.vma + off;
            for (const auto& dsec : view.sections) {
                if (!dsec.readable || !dsec.data) continue;
                for (uint64_t doff = 0; doff + ps <= dsec.size; doff += ps) {
                    uint64_t ptr = view.readPtr(dsec.vma + doff);
                    if (ptr == nameVma) {
                        // Found a pointer to this name; check if it's near a vtable
                        uint64_t vtableCandidate = dsec.vma + doff + ps;
                        if (view.readPtr(vtableCandidate) &&
                            view.inExecutable(view.readPtr(vtableCandidate))) {
                            ClassNode& node = out.classes[name];
                            if (node.name.empty()) {
                                node.name = name;
                                node.isPolymorphic = (first == 'C' || first == 'M');
                            }
                        }
                    }
                }
            }
        }
    }
}

// ─── reconstruct ─────────────────────────────────────────────────────────────

bool SymbianRttiReconstructor::reconstruct(
    const BinaryView&            view,
    const std::vector<uint64_t>& funcVmas,
    ClassHierarchyGraph&         out)
{
    std::unordered_set<uint64_t> funcSet(funcVmas.begin(), funcVmas.end());

    // Primary: TMetaClass scan
    scanTMetaClass(view, out);

    // Fallback if nothing found
    if (out.empty()) {
        scanNamePattern(view, funcSet, out);
    }

    return !out.empty();
}

} // namespace rtti
} // namespace retdec
