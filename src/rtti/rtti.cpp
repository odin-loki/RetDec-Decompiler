/**
 * @file src/rtti/rtti.cpp
 * @brief BinaryView helpers and ClassHierarchyGraph lookup implementations.
 */

#include "retdec/rtti/rtti.h"

#include <algorithm>
#include <cstring>

namespace retdec {
namespace rtti {

// ─── BinaryView ───────────────────────────────────────────────────────────────

const BinarySection* BinaryView::sectionAt(uint64_t vma) const noexcept {
    for (const auto& s : sections) {
        if (vma >= s.vma && vma < s.vma + s.size)
            return &s;
    }
    return nullptr;
}

bool BinaryView::inExecutable(uint64_t vma) const noexcept {
    const BinarySection* s = sectionAt(vma);
    return s && s->executable;
}

bool BinaryView::inData(uint64_t vma) const noexcept {
    const BinarySection* s = sectionAt(vma);
    return s && s->writable && !s->executable;
}

bool BinaryView::inReadOnly(uint64_t vma) const noexcept {
    const BinarySection* s = sectionAt(vma);
    return s && s->readable && !s->writable && !s->executable;
}

uint32_t BinaryView::read32(uint64_t vma) const noexcept {
    const BinarySection* s = sectionAt(vma);
    if (!s) return 0;
    uint64_t off = vma - s->vma;
    if (off + 4 > s->size) return 0;
    uint32_t v;
    std::memcpy(&v, s->data + off, 4);
    return v;
}

int32_t BinaryView::readI32(uint64_t vma) const noexcept {
    return static_cast<int32_t>(read32(vma));
}

uint64_t BinaryView::readPtr(uint64_t vma) const noexcept {
    if (is64bit) {
        const BinarySection* s = sectionAt(vma);
        if (!s) return 0;
        uint64_t off = vma - s->vma;
        if (off + 8 > s->size) return 0;
        uint64_t v;
        std::memcpy(&v, s->data + off, 8);
        return v;
    }
    return read32(vma);
}

int64_t BinaryView::readIPtr(uint64_t vma) const noexcept {
    if (is64bit) return static_cast<int64_t>(readPtr(vma));
    return static_cast<int64_t>(static_cast<int32_t>(read32(vma)));
}

std::string BinaryView::readCStr(uint64_t vma, std::size_t maxLen) const {
    const BinarySection* s = sectionAt(vma);
    if (!s) return {};
    uint64_t off = vma - s->vma;
    std::string result;
    result.reserve(32);
    for (std::size_t i = 0; i < maxLen && off + i < s->size; ++i) {
        char c = static_cast<char>(s->data[off + i]);
        if (c == '\0') break;
        result += c;
    }
    return result;
}

// ─── ClassHierarchyGraph ──────────────────────────────────────────────────────

const ClassNode* ClassHierarchyGraph::byName(const std::string& name) const noexcept {
    auto it = classes.find(name);
    return it != classes.end() ? &it->second : nullptr;
}

const ClassNode* ClassHierarchyGraph::byTypeInfoVma(uint64_t vma) const noexcept {
    for (const auto& kv : classes) {
        if (kv.second.typeInfoVma == vma)
            return &kv.second;
    }
    return nullptr;
}

const ClassNode* ClassHierarchyGraph::byColVma(uint64_t vma) const noexcept {
    for (const auto& kv : classes) {
        if (kv.second.colVma == vma)
            return &kv.second;
    }
    return nullptr;
}

std::vector<const ClassNode*>
ClassHierarchyGraph::directDerivedFrom(const std::string& baseName) const {
    std::vector<const ClassNode*> result;
    for (const auto& kv : classes) {
        for (const auto& e : kv.second.bases) {
            if (e.baseName == baseName) {
                result.push_back(&kv.second);
                break;
            }
        }
    }
    return result;
}

} // namespace rtti
} // namespace retdec
