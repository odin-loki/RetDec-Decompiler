/**
 * @file include/retdec/dex_parser/dex_apk_reader.h
 * @brief APK / ZIP reader, multidex support, ART OAT header, ProGuard mapping.
 *
 * Handles:
 *   - Standard APK: classes.dex, classes2.dex, …, classesN.dex
 *   - ART OAT files: OAT header → embedded DEX files
 *   - ProGuard / R8 mapping.txt: name-reversal applied to all BcClass/BcMethod/BcField
 */

#ifndef RETDEC_DEX_PARSER_DEX_APK_READER_H
#define RETDEC_DEX_PARSER_DEX_APK_READER_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/dex_parser/dex_header.h"
#include "retdec/dex_parser/dex_class_parser.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace dex_parser {

// ─── ProGuard mapping ─────────────────────────────────────────────────────────

/**
 * @brief Parsed ProGuard / R8 mapping file.
 *
 * Format:
 *   original.Class -> obfuscated.Name:
 *       returnType originalMethod(params) -> obfuscatedName
 *       fieldType originalField -> obfuscatedField
 */
struct ProGuardMapping {
    /// Obfuscated class name → original class name.
    std::unordered_map<std::string, std::string> classMap;

    struct MemberEntry {
        std::string originalName;
        std::string originalDesc; ///< JVM/DEX descriptor or empty
    };

    /// Obfuscated class name → (obfuscated member name → MemberEntry).
    std::unordered_map<std::string,
        std::unordered_map<std::string, MemberEntry>> memberMap;

    static ProGuardMapping parse(const std::string& mappingText);
    bool empty() const { return classMap.empty(); }
};

// ─── APK read options ─────────────────────────────────────────────────────────

struct ApkReadOptions {
    DexParseOptions dexOpts;
    bool parseProGuardMapping = true;   ///< Attempt to load mapping.txt
    bool parseAllDexFiles     = true;   ///< Parse classes2..N.dex
    bool parseOat             = false;  ///< Try to parse OAT if magic matches
    int  targetSdkVersion     = -1;     ///< -1 = autodetect from manifest
};

// ─── APK read result ─────────────────────────────────────────────────────────

struct ApkReadResult {
    enum Status { OK, PartialError, Error };
    Status      status = OK;
    std::string error;

    bc_module::BcModule           module;
    std::vector<std::string>      warnings;
    std::vector<std::string>      dexNames;   ///< e.g. {"classes.dex","classes2.dex"}
    bool                          hadMapping = false;
    ProGuardMapping               mapping;
};

// ─── OAT header constants ─────────────────────────────────────────────────────

static constexpr uint8_t kOatMagic[4]   = {0x6f,0x61,0x74,0x0a}; // "oat\n"
static constexpr uint32_t kOatMinVersion = 064u;

struct OatHeader {
    uint8_t  magic[4];           // "oat\n"
    uint8_t  version[4];         // 3-digit decimal + '\0'
    uint32_t adler32Checksum;
    uint32_t instructionSet;
    uint32_t instructionSetFeatures;
    uint32_t dexFileCount;
    uint32_t oatDexFilesOffset;
    uint32_t executableOffset;
    uint32_t interpreterToInterpreterBridgeOffset;
    uint32_t interpreterToCompiledCodeBridgeOffset;
    uint32_t jniDlsymLookupOffset;
    uint32_t quickGenericJniTrampolineOffset;
    uint32_t quickImtConflictTrampolineOffset;
    uint32_t quickResolutionTrampolineOffset;
    uint32_t quickToInterpreterBridgeOffset;
};

// ─── APK / OAT reader ────────────────────────────────────────────────────────

class ApkReader {
public:
    explicit ApkReader(ApkReadOptions opts = ApkReadOptions{});

    /// Read an APK file (ZIP) from raw bytes.
    ApkReadResult readApk(const uint8_t* data, size_t size);
    ApkReadResult readApk(const std::vector<uint8_t>& data);

    /// Read a bare DEX file (not wrapped in APK).
    ApkReadResult readDex(const uint8_t* data, size_t size,
                          const std::string& name = "classes.dex");

    /// Read an ART OAT file.
    ApkReadResult readOat(const uint8_t* data, size_t size);

private:
    ApkReadOptions opts_;

    // ZIP central directory entry.
    struct ZipEntry {
        std::string          name;
        uint32_t             localHeaderOff;
        uint32_t             compressedSize;
        uint32_t             uncompressedSize;
        uint16_t             method;           ///< 0=stored, 8=deflated
    };

    std::vector<ZipEntry> parseCentralDirectory(const uint8_t* data, size_t size);
    std::vector<uint8_t>  extractEntry(const uint8_t* data, size_t size,
                                       const ZipEntry& entry);

    void processDex(const uint8_t* dexData, size_t dexSize,
                    const std::string& dexName,
                    bc_module::BcModule& module,
                    std::vector<std::string>& warnings);

    void applyProGuardMapping(bc_module::BcModule& module,
                              const ProGuardMapping& mapping);

    static bool isClassesDex(const std::string& name);
    static bool isMappingFile(const std::string& name);
    static std::string parseProGuardFromZip(const uint8_t* data, size_t size,
                                             const std::vector<ZipEntry>& entries,
                                             ApkReader& reader);
};

} // namespace dex_parser
} // namespace retdec

#endif // RETDEC_DEX_PARSER_DEX_APK_READER_H
