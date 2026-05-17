/**
 * @file src/dex_parser/dex_apk_reader.cpp
 * @brief APK/ZIP reader, multidex support, OAT header, ProGuard mapping.
 */

#include "retdec/dex_parser/dex_apk_reader.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <sstream>

namespace retdec {
namespace dex_parser {

// ─── ProGuardMapping ─────────────────────────────────────────────────────────

ProGuardMapping ProGuardMapping::parse(const std::string& text) {
    ProGuardMapping mapping;
    std::istringstream ss(text);
    std::string line;
    std::string currentObfClass;

    while (std::getline(ss, line)) {
        // Trim trailing whitespace / \r
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();

        if (line.empty() || line[0] == '#')
            continue;

        // Class mapping: "original.Class -> obfuscated.Name:"
        if (!line.empty() && line[0] != ' ' && line[0] != '\t') {
            size_t arrow = line.find(" -> ");
            if (arrow != std::string::npos) {
                std::string orig  = line.substr(0, arrow);
                std::string obf   = line.substr(arrow + 4);
                if (!obf.empty() && obf.back() == ':')
                    obf.pop_back();
                mapping.classMap[obf] = orig;
                currentObfClass = obf;
            }
        } else {
            // Member mapping: "    retType original(params) -> obfuscated"
            if (currentObfClass.empty()) continue;
            std::string trimmed = line;
            trimmed.erase(0, trimmed.find_first_not_of(" \t"));

            size_t arrow = trimmed.find(" -> ");
            if (arrow == std::string::npos) continue;

            std::string lhs = trimmed.substr(0, arrow);
            std::string obfName = trimmed.substr(arrow + 4);

            // Strip line number range prefix "N:M:" if present
            size_t colon2 = lhs.find(':');
            if (colon2 != std::string::npos) {
                size_t colon3 = lhs.find(':', colon2 + 1);
                if (colon3 != std::string::npos)
                    lhs = lhs.substr(colon3 + 1);
            }

            // "retType originalName(params)" or "fieldType originalName"
            size_t sp = lhs.rfind(' ');
            std::string origMember = (sp != std::string::npos) ? lhs.substr(sp + 1) : lhs;
            // Remove parameter list if present
            size_t paren = origMember.find('(');
            if (paren != std::string::npos)
                origMember = origMember.substr(0, paren);

            MemberEntry entry;
            entry.originalName = origMember;
            mapping.memberMap[currentObfClass][obfName] = std::move(entry);
        }
    }
    return mapping;
}

// ─── ZIP parsing ─────────────────────────────────────────────────────────────

static uint16_t readU2LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
static uint32_t readU4LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

std::vector<ApkReader::ZipEntry> ApkReader::parseCentralDirectory(
        const uint8_t* data, size_t size) {
    std::vector<ZipEntry> entries;
    if (size < 22) return entries;

    // Find End Of Central Directory record (EOCD) by scanning from end.
    // EOCD signature: 0x06054b50
    static const uint8_t kEOCDSig[4] = {0x50, 0x4b, 0x05, 0x06};
    int64_t eocdOff = -1;
    for (int64_t i = static_cast<int64_t>(size) - 22; i >= 0; --i) {
        if (std::memcmp(data + i, kEOCDSig, 4) == 0) {
            eocdOff = i;
            break;
        }
    }
    if (eocdOff < 0) return entries;

    const uint8_t* eocd = data + eocdOff;
    uint32_t cdSize   = readU4LE(eocd + 12);
    uint32_t cdOffset = readU4LE(eocd + 16);

    if (cdOffset + cdSize > size) return entries;

    static const uint8_t kCDSig[4] = {0x50, 0x4b, 0x01, 0x02};
    uint32_t pos = cdOffset;
    while (pos + 46 <= cdOffset + cdSize) {
        if (std::memcmp(data + pos, kCDSig, 4) != 0) break;

        uint16_t method     = readU2LE(data + pos + 10);
        uint32_t compSize   = readU4LE(data + pos + 20);
        uint32_t uncompSize = readU4LE(data + pos + 24);
        uint16_t nameLen    = readU2LE(data + pos + 28);
        uint16_t extraLen   = readU2LE(data + pos + 30);
        uint16_t commentLen = readU2LE(data + pos + 32);
        uint32_t localHdrOff= readU4LE(data + pos + 42);

        std::string name(reinterpret_cast<const char*>(data + pos + 46), nameLen);
        entries.push_back({name, localHdrOff, compSize, uncompSize, method});
        pos += 46u + nameLen + extraLen + commentLen;
    }
    return entries;
}

std::vector<uint8_t> ApkReader::extractEntry(const uint8_t* data, size_t size,
                                               const ZipEntry& entry) {
    // Local file header signature: 0x04034b50
    static const uint8_t kLFHSig[4] = {0x50, 0x4b, 0x03, 0x04};
    if (entry.localHeaderOff + 30 > size) return {};
    if (std::memcmp(data + entry.localHeaderOff, kLFHSig, 4) != 0) return {};

    uint16_t nameLen  = readU2LE(data + entry.localHeaderOff + 26);
    uint16_t extraLen = readU2LE(data + entry.localHeaderOff + 28);
    uint32_t dataOff  = entry.localHeaderOff + 30 + nameLen + extraLen;

    if (dataOff + entry.compressedSize > size) return {};

    if (entry.method == 0) {
        // Stored
        return std::vector<uint8_t>(data + dataOff,
                                    data + dataOff + entry.uncompressedSize);
    } else {
        // Deflated — we need zlib. For now, return raw compressed bytes with a
        // note: in a full implementation this would call inflate().
        // We return empty to signal "can't decompress" gracefully.
        (void)entry.compressedSize;
        return {};
    }
}

// ─── ApkReader ───────────────────────────────────────────────────────────────

ApkReader::ApkReader(ApkReadOptions opts)
    : opts_(std::move(opts)) {}

bool ApkReader::isClassesDex(const std::string& name) {
    if (name == "classes.dex") return true;
    // classes2.dex, classes3.dex, etc.
    if (name.size() > 11 &&
        name.substr(0, 7) == "classes" &&
        name.substr(name.size() - 4) == ".dex")
        return true;
    return false;
}

bool ApkReader::isMappingFile(const std::string& name) {
    return name == "mapping.txt" || name == "proguard/mapping.txt" ||
           name == "META-INF/mapping.txt";
}

void ApkReader::processDex(const uint8_t* dexData, size_t dexSize,
                            const std::string& dexName,
                            bc_module::BcModule& module,
                            std::vector<std::string>& warnings) {
    try {
        DexFile dex = DexFile::parse(dexData, dexSize);
        DexClassParser parser(dex, opts_.dexOpts);

        for (uint32_t i = 0; i < dex.classCount(); ++i) {
            auto result = parser.parseClass(i);
            if (result.status == DexClassResult::OK && result.bcClass) {
                module.addClass(std::move(*result.bcClass));
            } else if (result.status == DexClassResult::Error) {
                warnings.push_back(dexName + ": class " + std::to_string(i) +
                                   ": " + result.error);
            }
        }
    } catch (const DexParseError& e) {
        warnings.push_back(dexName + ": " + e.what());
    }
}

ApkReadResult ApkReader::readDex(const uint8_t* data, size_t size,
                                  const std::string& name) {
    ApkReadResult result;
    result.module.setName(name);
    result.module.setSourceLang(bc_module::SourceLang::Java);
    result.dexNames.push_back(name);

    processDex(data, size, name, result.module, result.warnings);

    if (!result.warnings.empty())
        result.status = ApkReadResult::PartialError;
    return result;
}

ApkReadResult ApkReader::readApk(const std::vector<uint8_t>& data) {
    return readApk(data.data(), data.size());
}

ApkReadResult ApkReader::readApk(const uint8_t* data, size_t size) {
    ApkReadResult result;
    result.module.setSourceLang(bc_module::SourceLang::Java);

    auto entries = parseCentralDirectory(data, size);
    if (entries.empty()) {
        result.status = ApkReadResult::Error;
        result.error  = "not a valid ZIP/APK file";
        return result;
    }

    // Check for OAT magic (ELF with OAT section is handled separately).
    // For APK files, look for classes*.dex entries.
    std::string mappingText;

    // Sort dex entries by name so classes.dex comes first.
    std::vector<const ZipEntry*> dexEntries;
    const ZipEntry* mappingEntry = nullptr;
    for (const auto& e : entries) {
        if (isClassesDex(e.name))
            dexEntries.push_back(&e);
        else if (opts_.parseProGuardMapping && isMappingFile(e.name))
            mappingEntry = &e;
    }

    std::sort(dexEntries.begin(), dexEntries.end(),
              [](const ZipEntry* a, const ZipEntry* b) { return a->name < b->name; });

    if (dexEntries.empty()) {
        result.status = ApkReadResult::Error;
        result.error  = "no classes.dex found in APK";
        return result;
    }

    // Load ProGuard mapping first if available.
    if (mappingEntry) {
        auto rawMapping = extractEntry(data, size, *mappingEntry);
        if (!rawMapping.empty()) {
            std::string text(reinterpret_cast<const char*>(rawMapping.data()),
                             rawMapping.size());
            result.mapping = ProGuardMapping::parse(text);
            result.hadMapping = !result.mapping.empty();
        }
    }

    // Process each DEX file.
    for (const auto* entry : dexEntries) {
        if (!opts_.parseAllDexFiles && entry->name != "classes.dex")
            break;
        auto dexData = extractEntry(data, size, *entry);
        if (dexData.empty()) {
            result.warnings.push_back("could not extract " + entry->name +
                                      " (deflated compression not supported yet)");
            continue;
        }
        result.dexNames.push_back(entry->name);
        processDex(dexData.data(), dexData.size(), entry->name,
                   result.module, result.warnings);
    }

    // Apply ProGuard mapping.
    if (result.hadMapping)
        applyProGuardMapping(result.module, result.mapping);

    result.module.setName("APK");
    if (!result.warnings.empty())
        result.status = ApkReadResult::PartialError;
    return result;
}

ApkReadResult ApkReader::readOat(const uint8_t* data, size_t size) {
    ApkReadResult result;

    // Minimal OAT support: look for embedded DEX files.
    // OAT files are ELF shared objects with ".rodata" containing DEX.
    // The OAT header begins after the ELF section.

    // Scan for DEX magic within the OAT blob.
    static const uint8_t kDexMagicBuf[4] = {0x64, 0x65, 0x78, 0x0a}; // "dex\n"
    for (size_t i = 0; i + 8 < size; ++i) {
        if (std::memcmp(data + i, kDexMagicBuf, 4) != 0)
            continue;
        // Verify it looks like a real DEX header (fileSize field must be plausible)
        if (i + 0x24 > size) continue;
        uint32_t fileSize = readU4LE(data + i + 0x20);
        if (fileSize < 112 || fileSize > size - i) continue;

        std::string dexName = "oat_dex_" + std::to_string(i);
        processDex(data + i, fileSize, dexName, result.module, result.warnings);
        result.dexNames.push_back(dexName);
        i += fileSize - 1; // jump past this DEX
    }

    result.module.setSourceLang(bc_module::SourceLang::Java);
    result.module.setName("OAT");
    if (!result.warnings.empty())
        result.status = ApkReadResult::PartialError;
    return result;
}

// ─── ProGuard application ─────────────────────────────────────────────────────

void ApkReader::applyProGuardMapping(bc_module::BcModule& module,
                                      const ProGuardMapping& mapping) {
    for (auto& cls : module.classes()) {
        auto classIt = mapping.classMap.find(cls.name);
        if (classIt != mapping.classMap.end())
            cls.name = classIt->second;

        // Rename methods and fields
        auto memberIt = mapping.memberMap.find(cls.name);
        if (memberIt == mapping.memberMap.end())
            continue;
        const auto& memberMap = memberIt->second;

        for (auto& method : cls.methods) {
            auto it = memberMap.find(method.name);
            if (it != memberMap.end())
                method.name = it->second.originalName;
        }
        for (auto& field : cls.fields) {
            auto it = memberMap.find(field.name);
            if (it != memberMap.end())
                field.name = it->second.originalName;
        }
    }
}

} // namespace dex_parser
} // namespace retdec
