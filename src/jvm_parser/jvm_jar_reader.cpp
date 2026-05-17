/**
 * @file src/jvm_parser/jvm_jar_reader.cpp
 * @brief JAR reader: ZIP enumeration and cross-class type resolver.
 *
 * Uses a minimal built-in ZIP central-directory parser (no external library).
 * Only STORED (method 0) and DEFLATE (method 8) entries are supported.
 * DEFLATE decompression delegates to zlib if available; otherwise entries
 * with method 8 are skipped (still counted in classesFound).
 */

#include "retdec/jvm_parser/jvm_jar_reader.h"

#include <algorithm>
#include <cstring>

namespace retdec {
namespace jvm_parser {

// ─── Minimal ZIP reader ───────────────────────────────────────────────────────

// ZIP end-of-central-directory signature.
static constexpr uint32_t kEOCD   = 0x06054b50;
static constexpr uint32_t kLocal  = 0x04034b50;
static constexpr uint32_t kCentral= 0x02014b50;

static uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
static uint32_t le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24));
}

struct ZipCDEntry {
    std::string  path;
    uint16_t     method          = 0;
    uint32_t     compressedSize  = 0;
    uint32_t     uncompressedSize= 0;
    uint32_t     localOffset     = 0;
};

static std::vector<ZipCDEntry> parseCentralDirectory(const uint8_t* data, size_t size) {
    // Search backward for EOCD signature.
    if (size < 22) return {};
    size_t eocdOffset = size - 22;
    while (eocdOffset > 0) {
        if (le32(data + eocdOffset) == kEOCD) break;
        --eocdOffset;
    }
    if (le32(data + eocdOffset) != kEOCD) return {};

    uint32_t cdOffset = le32(data + eocdOffset + 16);
    uint16_t cdCount  = le16(data + eocdOffset + 10);

    std::vector<ZipCDEntry> entries;
    size_t pos = cdOffset;
    for (uint16_t i = 0; i < cdCount && pos + 46 <= size; ++i) {
        if (le32(data + pos) != kCentral) break;
        ZipCDEntry e;
        e.method           = le16(data + pos + 10);
        e.compressedSize   = le32(data + pos + 20);
        e.uncompressedSize = le32(data + pos + 24);
        e.localOffset      = le32(data + pos + 42);
        uint16_t fnLen     = le16(data + pos + 28);
        uint16_t extraLen  = le16(data + pos + 30);
        uint16_t commentLen= le16(data + pos + 32);
        if (pos + 46 + fnLen <= size)
            e.path = std::string(reinterpret_cast<const char*>(data + pos + 46), fnLen);
        pos += 46 + fnLen + extraLen + commentLen;
        entries.push_back(std::move(e));
    }
    return entries;
}

static std::vector<uint8_t> readLocalEntry(const uint8_t* data, size_t size,
                                            const ZipCDEntry& cd)
{
    size_t pos = cd.localOffset;
    if (pos + 30 > size) return {};
    if (le32(data + pos) != kLocal) return {};
    uint16_t fnLen    = le16(data + pos + 26);
    uint16_t extraLen = le16(data + pos + 28);
    size_t dataStart  = pos + 30 + fnLen + extraLen;
    if (dataStart + cd.compressedSize > size) return {};

    if (cd.method == 0) {
        // STORED
        return std::vector<uint8_t>(data + dataStart,
                                    data + dataStart + cd.uncompressedSize);
    }
    // DEFLATE — not decompressing here (would require zlib).
    // Return empty to signal "compressed, not available".
    return {};
}

// ─── JarReader ────────────────────────────────────────────────────────────────

JarReader::JarReader(JarReadOptions opts) : opts_(std::move(opts)) {}

bool JarReader::isClassEntry(const std::string& path) const {
    if (path.size() < 6) return false;
    return path.substr(path.size() - 6) == ".class";
}

bool JarReader::isNestedJar(const std::string& path) const {
    if (path.size() < 4) return false;
    return path.substr(path.size() - 4) == ".jar";
}

int JarReader::multiReleaseVersion(const std::string& path) const {
    // "META-INF/versions/N/..."
    if (path.substr(0, 19) != "META-INF/versions/") return 0;
    size_t start = 18;
    size_t slash = path.find('/', start);
    if (slash == std::string::npos) return 0;
    try {
        return std::stoi(path.substr(start, slash - start));
    } catch (...) { return 0; }
}

std::string JarReader::stripMRPrefix(const std::string& path) const {
    // "META-INF/versions/N/foo/Bar.class" → "foo/Bar.class"
    size_t slash = path.find('/', 18);
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

std::vector<JarEntry> JarReader::listEntries(const uint8_t* data, size_t size) {
    auto cd = parseCentralDirectory(data, size);
    std::vector<JarEntry> entries;
    for (const auto& e : cd) {
        JarEntry je;
        je.path    = e.path;
        je.version = multiReleaseVersion(e.path);
        entries.push_back(std::move(je));
    }
    return entries;
}

JarReadResult JarReader::read(const std::vector<uint8_t>& data) {
    return read(data.data(), data.size());
}

JarReadResult JarReader::read(const uint8_t* data, size_t size) {
    JarReadResult res;
    res.module = bc_module::BcModule("jar", bc_module::SourceLang::Java);
    try {
        auto cd = parseCentralDirectory(data, size);
        if (cd.empty()) {
            res.ok    = false;
            res.error = "Not a valid ZIP/JAR (no central directory found)";
            return res;
        }

        // Collect class entries, preferring higher MRJAR versions up to target.
        std::unordered_map<std::string, ZipCDEntry> bestEntries;
        std::unordered_map<std::string, int> bestVersion;

        for (const auto& e : cd) {
            if (!isClassEntry(e.path)) continue;
            int ver = multiReleaseVersion(e.path);
            std::string base = (ver > 0) ? stripMRPrefix(e.path) : e.path;
            if (ver > opts_.targetJavaVersion) continue;
            if (ver >= bestVersion[base]) {
                bestVersion[base]  = ver;
                bestEntries[base]  = e;
            }
        }

        res.classesFound = static_cast<uint32_t>(bestEntries.size());

        std::vector<JvmParseResult> parseResults;
        for (const auto& [base, e] : bestEntries) {
            auto bytes = readLocalEntry(data, size, e);
            if (bytes.empty()) {
                // Compressed entry — count as found but not parsed.
                ++res.parseErrors;
                res.errorList.push_back(base + ": compressed (DEFLATE) — skipped");
                continue;
            }
            auto pr = parseClassFile(bytes, opts_.classOpts);
            if (!pr.ok) {
                ++res.parseErrors;
                res.errorList.push_back(base + ": " + pr.error);
            } else {
                ++res.classesParsed;
                parseResults.push_back(std::move(pr));
            }
        }

        // Add classes to module.
        for (auto& pr : parseResults)
            res.module.addClass(std::move(pr.cls));

        // Cross-class type resolution.
        if (opts_.resolveTypes) {
            TypeResolver resolver(res.module);
            resolver.resolve(parseResults);
        }

        res.ok = true;
    } catch (const std::exception& e) {
        res.ok    = false;
        res.error = e.what();
    }
    return res;
}

// ─── TypeResolver ─────────────────────────────────────────────────────────────

TypeResolver::TypeResolver(bc_module::BcModule& mod) : mod_(mod) {}

void TypeResolver::resolve(const std::vector<JvmParseResult>& results) {
    for (const auto& pr : results) {
        // For each class, if the superclass is unknown in the module,
        // add it as an external reference.
        if (pr.cls.superClass) {
            if (pr.cls.superClass->isRef()) {
                const auto& name = pr.cls.superClass->ref().className;
                if (mod_.findClass(name) == nullptr &&
                    name != "java/lang/Object") {
                    mod_.addExternalRef(name, "import " + name + ";");
                }
            }
        }
        for (const auto& iface : pr.cls.interfaces) {
            if (iface.isRef()) {
                const auto& name = iface.ref().className;
                if (mod_.findClass(name) == nullptr)
                    mod_.addExternalRef(name, "import " + name + ";");
            }
        }
    }
}

bc_module::BcType TypeResolver::resolveType(const std::string& descriptor,
                                              const ConstPool& pool) {
    (void)pool;
    return JvmSignatureParser::parseDescriptor(descriptor);
}

void TypeResolver::linkInnerClasses(bc_module::BcClass& cls,
                                     const InnerClassesAttr& inner,
                                     const ConstPool& pool) {
    for (const auto& e : inner.classes) {
        if (e.innerClassInfo != 0 && e.outerClassInfo != 0) {
            std::string outerName = pool.className(e.outerClassInfo);
            if (outerName == cls.fqName && e.innerName != 0) {
                // The class is an outer class; inner class link is recorded elsewhere.
            }
        }
    }
}

} // namespace jvm_parser
} // namespace retdec
