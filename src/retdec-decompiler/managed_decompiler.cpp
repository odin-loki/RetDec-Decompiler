/**
 * @file src/retdec-decompiler/managed_decompiler.cpp
 * @brief Managed-language decompilation dispatcher.
 *
 * Handles Java .class / .jar, Android DEX / APK, Python .pyc, Lua .luac,
 * WebAssembly .wasm, and .NET CLI assemblies by routing them to their
 * respective parser + emitter pipelines, bypassing the LLVM IR pipeline
 * used for native binaries.
 *
 * Copyright (c) 2025 Odin Loch trading as Imortek. All rights reserved.
 */

#include "managed_decompiler.h"

// Java / DEX / JAR
#include "retdec/jvm_parser/jvm_class_parser.h"
#include "retdec/jvm_parser/jvm_jar_reader.h"
#include "retdec/dex_parser/dex_apk_reader.h"
#include "retdec/java_emitter/java_file_emitter.h"

// .NET CLI
#include "retdec/cli_parser/cli_reader.h"
#include "retdec/cli_parser/pe_reader.h"
#include "retdec/csharp_emitter/cs_file_emitter.h"
#include "retdec/cil_reconstruct/cil_reconstructor.h"

// Python
#include "retdec/pyc_parser/pyc_reader.h"
#include "retdec/pyc_parser/pyc_magic.h"
#include "retdec/py_reconstruct/py_cfg_builder.h"
#include "retdec/py_emitter/py_file_emitter.h"

// Lua
#include "retdec/lua_parser/lua_reader.h"
#include "retdec/lua_parser/lua_emitter.h"

// WASM
#include "retdec/wasm_parser/wasm_reader.h"
#include "retdec/wasm_parser/wat_emitter.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {

/// Read an entire file into a byte vector, or throw on error.
std::vector<uint8_t> slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("Cannot open file: " + path);
    auto size = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(size);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    return buf;
}

/// Write text to a file, or throw on error.
void writeText(const std::string& path, const std::string& text)
{
    std::ofstream f(path);
    if (!f)
        throw std::runtime_error("Cannot write file: " + path);
    f << text;
}

std::string extensionLower(const std::string& path)
{
    auto dot = path.rfind('.');
    if (dot == std::string::npos || dot + 1 >= path.size())
        return {};
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// ─── ZIP central-directory probe (detection only) ────────────────────────────

static constexpr uint32_t kZipEOCD    = 0x06054b50u;
static constexpr uint32_t kZipCentral = 0x02014b50u;

static uint16_t le16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

static bool isZipLocalHeader(const uint8_t* d, std::size_t size)
{
    return size >= 4 && d[0] == 0x50 && d[1] == 0x4B
        && (d[2] == 0x03 || d[2] == 0x05 || d[2] == 0x07)
        && (d[3] == 0x04 || d[3] == 0x06 || d[3] == 0x08);
}

static bool endsWith(const std::string& path, const char* suffix)
{
    const std::size_t n = std::strlen(suffix);
    return path.size() >= n && path.compare(path.size() - n, n, suffix) == 0;
}

static bool isClassesDexEntry(const std::string& name)
{
    if (name == "classes.dex")
        return true;
    if (name.size() > 10 && name.compare(0, 8, "classes") == 0
        && name.substr(name.size() - 4) == ".dex") {
        for (std::size_t i = 8; i + 4 < name.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(name[i])))
                return false;
        }
        return true;
    }
    return false;
}

enum class ZipManagedKind { None, JavaJar, Apk };

static ZipManagedKind probeZipManagedKind(const uint8_t* data, std::size_t size)
{
    if (size < 22)
        return ZipManagedKind::None;

    std::size_t eocdOffset = size - 22;
    while (eocdOffset > 0) {
        if (le32(data + eocdOffset) == kZipEOCD)
            break;
        --eocdOffset;
    }
    if (le32(data + eocdOffset) != kZipEOCD)
        return ZipManagedKind::None;

    const uint32_t cdOffset = le32(data + eocdOffset + 16);
    const uint16_t cdCount  = le16(data + eocdOffset + 10);

    bool hasClass = false;
    bool hasDex   = false;

    std::size_t pos = cdOffset;
    for (uint16_t i = 0; i < cdCount && pos + 46 <= size; ++i) {
        if (le32(data + pos) != kZipCentral)
            break;
        const uint16_t fnLen = le16(data + pos + 28);
        const uint16_t extraLen = le16(data + pos + 30);
        const uint16_t commentLen = le16(data + pos + 32);
        if (pos + 46 + fnLen <= size) {
            std::string name(reinterpret_cast<const char*>(data + pos + 46), fnLen);
            if (endsWith(name, ".class"))
                hasClass = true;
            if (isClassesDexEntry(name))
                hasDex = true;
        }
        pos += 46 + fnLen + extraLen + commentLen;
    }

    if (hasDex)
        return ZipManagedKind::Apk;
    if (hasClass)
        return ZipManagedKind::JavaJar;
    return ZipManagedKind::None;
}

static bool probeCliAssembly(const uint8_t* data, std::size_t size)
{
    retdec::cli_parser::PeReader reader;
    if (!reader.open(data, size))
        return false;
    return reader.hasCLI();
}

static bool isJavaClassMagic(const uint8_t* d, std::size_t size)
{
    if (size < 8 || d[0] != 0xCA || d[1] != 0xFE || d[2] != 0xBA || d[3] != 0xBE)
        return false;
    const uint16_t majorVer = uint16_t(uint16_t(d[6]) << 8 | d[7]);
    return majorVer >= 44 && majorVer <= 70;
}

} // anonymous namespace

// ─── Format detection ─────────────────────────────────────────────────────────

ManagedFormat detectManagedFormatFromBytes(const uint8_t* data, std::size_t size)
{
    if (!data || size < 4)
        return ManagedFormat::Unknown;

    const uint8_t* d = data;

    if (isJavaClassMagic(d, size))
        return ManagedFormat::JavaClass;

    if (d[0] == 0x64 && d[1] == 0x65 && d[2] == 0x78 && d[3] == 0x0A)
        return ManagedFormat::Dex;

    if (d[0] == 0x00 && d[1] == 0x61 && d[2] == 0x73 && d[3] == 0x6D)
        return ManagedFormat::Wasm;

    if (d[0] == 0x1B && d[1] == 0x4C && d[2] == 0x75 && d[3] == 0x61)
        return ManagedFormat::LuaBytecode;

    if (size >= 4 && d[2] == 0x0D && d[3] == 0x0A) {
        const uint16_t magicWord = uint16_t(d[0]) | uint16_t(uint16_t(d[1]) << 8);
        if (magicWord != 0)
            return ManagedFormat::PythonPyc;
    }

    if (isZipLocalHeader(d, size)) {
        switch (probeZipManagedKind(d, size)) {
            case ZipManagedKind::Apk:     return ManagedFormat::Apk;
            case ZipManagedKind::JavaJar: return ManagedFormat::JavaJar;
            default: break;
        }
    }

    if (size >= 2 && d[0] == 0x4D && d[1] == 0x5A && probeCliAssembly(d, size))
        return ManagedFormat::CliAssembly;

    return ManagedFormat::Unknown;
}

ManagedFormat detectManagedFormat(const std::string& path)
{
    std::vector<uint8_t> buf;
    try { buf = slurp(path); } catch (...) { return ManagedFormat::Unknown; }
    return detectManagedFormatFromBytes(buf.data(), buf.size());
}

const char* managedFormatName(ManagedFormat fmt)
{
    switch (fmt) {
        case ManagedFormat::JavaClass:   return "Java .class";
        case ManagedFormat::JavaJar:     return "Java JAR";
        case ManagedFormat::Dex:         return "Android DEX";
        case ManagedFormat::Apk:         return "Android APK";
        case ManagedFormat::PythonPyc:   return "Python .pyc";
        case ManagedFormat::LuaBytecode: return "Lua bytecode";
        case ManagedFormat::Wasm:        return "WebAssembly";
        case ManagedFormat::CliAssembly: return ".NET CLI assembly";
        default:                         return "unknown";
    }
}

const char* managedOutputLangHint(ManagedFormat fmt)
{
    switch (fmt) {
        case ManagedFormat::JavaClass:
        case ManagedFormat::JavaJar:
        case ManagedFormat::Dex:
        case ManagedFormat::Apk:
            return "java";
        case ManagedFormat::PythonPyc:
            return "python";
        case ManagedFormat::LuaBytecode:
            return "lua";
        case ManagedFormat::Wasm:
            return "wat";
        case ManagedFormat::CliAssembly:
            return "csharp";
        default:
            return nullptr;
    }
}

void logManagedFormatRoute(ManagedFormat fmt, const std::string& path,
                           std::ostream& os)
{
    os << "[managed] Detected format: " << managedFormatName(fmt)
       << " (" << path << ")\n";
    if (const char* lang = managedOutputLangHint(fmt))
        os << "[managed] Output language: " << lang
           << " (native --output-lang is ignored for managed inputs; "
              "use managed emitters directly)\n";
}

void logUnknownManagedFormat(const uint8_t* data, std::size_t size,
                             const std::string& path, std::ostream& os)
{
    const std::string ext = extensionLower(path);

    os << "[managed] No managed format detected for: " << path << "\n";

    if (!data || size < 4) {
        os << "[managed] Hint: file is empty or too small to identify.\n";
        return;
    }

    if (isZipLocalHeader(data, size)) {
        const auto kind = probeZipManagedKind(data, size);
        if (kind == ZipManagedKind::None) {
            os << "[managed] Hint: ZIP archive found but no .class or classes*.dex "
                  "entries — not a JAR/APK RetDec can decompile.\n";
            if (ext == ".jar")
                os << "[managed]       Expected a JAR with at least one .class entry.\n";
            else if (ext == ".apk")
                os << "[managed]       Expected an APK with classes.dex (or classesN.dex).\n";
        }
        return;
    }

    if (data[0] == 0x4D && data[1] == 0x5A) {
        retdec::cli_parser::PeReader pe;
        if (pe.open(data, size) && !pe.hasCLI()) {
            os << "[managed] Hint: PE image without CLI metadata — use the native "
                  "pipeline (omit --output-lang; default C/C++ emitters apply).\n";
            if (ext == ".dll" || ext == ".exe")
                os << "[managed]       For .NET assemblies ensure the file contains "
                      "a COM descriptor (#~ metadata).\n";
        } else if (!pe.open(data, size)) {
            os << "[managed] Hint: invalid or truncated PE header.\n";
        }
        return;
    }

    if (data[0] == 0xCA && data[1] == 0xFE && data[2] == 0xBA && data[3] == 0xBE) {
        os << "[managed] Hint: CAFEBABE header looks like Mach-O FAT, not Java "
              "(class major version out of range).\n";
        return;
    }

    if (ext == ".jar" || ext == ".apk" || ext == ".dex" || ext == ".class"
        || ext == ".pyc" || ext == ".luac" || ext == ".wasm" || ext == ".dll") {
        os << "[managed] Hint: extension suggests a managed target but content "
              "did not match — file may be corrupt, packed, or encrypted.\n";
        os << "[managed]       Try scripts/unpack_and_decompile first for packed "
              "native binaries; managed inputs must be valid bytecode.\n";
    } else {
        os << "[managed] Hint: supported managed inputs: .class, .jar, .dex, .apk, "
              ".pyc, .luac, .wasm, .NET PE (.dll/.exe with CLI metadata).\n";
        os << "[managed]       Native PE/ELF/Mach-O use the default pipeline; "
              "optional --output-lang selects C, C++, etc.\n";
    }
}

// ─── Java .class decompilation ────────────────────────────────────────────────

static int decompileJavaClass(const std::string& inputFile,
                               const std::string& outputFile)
{
    std::cerr << "[managed] Decompiling Java .class: " << inputFile << "\n";

    std::vector<uint8_t> bytes;
    try { bytes = slurp(inputFile); }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    auto parsed = retdec::jvm_parser::parseClassFile(bytes);
    if (!parsed.ok) {
        std::cerr << "Error parsing .class: " << parsed.error << "\n";
        return 1;
    }

    retdec::bc_module::BcModule module;
    module.addClass(parsed.cls);

    retdec::java_emitter::JavaFileEmitter emitter;
    auto result = emitter.emitModule(module);

    std::string combined;
    for (auto& fr : result.files) {
        if (!fr.source.empty())
            combined += fr.source + "\n";
    }

    if (combined.empty())
        std::cerr << "Warning: Java emitter produced no output.\n";

    try { writeText(outputFile, combined); }
    catch (const std::exception& e) {
        std::cerr << "Error writing output: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "[managed] Java decompilation written to: " << outputFile << "\n";
    return 0;
}

// ─── Java JAR decompilation ───────────────────────────────────────────────────

static int decompileJavaJar(const std::string& inputFile,
                             const std::string& outputFile)
{
    std::cerr << "[managed] Decompiling Java JAR: " << inputFile << "\n";

    std::vector<uint8_t> bytes;
    try { bytes = slurp(inputFile); }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    retdec::jvm_parser::JarReader reader;
    auto jarResult = reader.read(bytes);
    if (!jarResult.ok) {
        std::cerr << "Error parsing JAR: " << jarResult.error << "\n";
        return 1;
    }
    for (const auto& err : jarResult.errorList)
        std::cerr << "Warning: " << err << "\n";

    retdec::java_emitter::JavaFileEmitter emitter;
    auto emitResult = emitter.emitModule(jarResult.module);

    std::string combined;
    for (auto& fr : emitResult.files) {
        if (!fr.source.empty())
            combined += fr.source + "\n";
    }

    try { writeText(outputFile, combined); }
    catch (const std::exception& e) {
        std::cerr << "Error writing output: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "[managed] JAR decompilation written to: " << outputFile
              << " (" << jarResult.classesParsed << "/"
              << jarResult.classesFound << " classes)\n";
    return 0;
}

// ─── DEX decompilation ────────────────────────────────────────────────────────

static int decompileDex(const std::string& inputFile,
                         const std::string& outputFile)
{
    std::cerr << "[managed] Decompiling DEX: " << inputFile << "\n";

    std::vector<uint8_t> bytes;
    try { bytes = slurp(inputFile); }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    retdec::dex_parser::ApkReader reader;
    auto result = reader.readDex(bytes.data(), bytes.size(), "classes.dex");

    if (result.status == retdec::dex_parser::ApkReadResult::Error) {
        std::cerr << "Error parsing DEX: " << result.error << "\n";
        return 1;
    }
    if (!result.error.empty())
        std::cerr << "Warning: " << result.error << "\n";

    retdec::java_emitter::JavaFileEmitter emitter;
    auto emitResult = emitter.emitModule(result.module);

    std::string combined;
    for (auto& fr : emitResult.files) {
        if (!fr.source.empty())
            combined += fr.source + "\n";
    }

    try { writeText(outputFile, combined); }
    catch (const std::exception& e) {
        std::cerr << "Error writing output: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "[managed] DEX decompilation written to: " << outputFile << "\n";
    return 0;
}

// ─── APK decompilation ────────────────────────────────────────────────────────

static int decompileApk(const std::string& inputFile,
                        const std::string& outputFile)
{
    std::cerr << "[managed] Decompiling APK: " << inputFile << "\n";

    std::vector<uint8_t> bytes;
    try { bytes = slurp(inputFile); }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    retdec::dex_parser::ApkReader reader;
    auto result = reader.readApk(bytes);

    if (result.status == retdec::dex_parser::ApkReadResult::Error) {
        std::cerr << "Error parsing APK: " << result.error << "\n";
        return 1;
    }
    for (const auto& w : result.warnings)
        std::cerr << "Warning: " << w << "\n";

    retdec::java_emitter::JavaFileEmitter emitter;
    auto emitResult = emitter.emitModule(result.module);

    std::string combined;
    for (auto& fr : emitResult.files) {
        if (!fr.source.empty())
            combined += fr.source + "\n";
    }

    try { writeText(outputFile, combined); }
    catch (const std::exception& e) {
        std::cerr << "Error writing output: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "[managed] APK decompilation written to: " << outputFile << "\n";
    return 0;
}

// ─── .NET CLI decompilation ───────────────────────────────────────────────────

static int decompileCliAssembly(const std::string& inputFile,
                                 const std::string& outputFile)
{
    std::cerr << "[managed] Decompiling .NET CLI assembly: " << inputFile << "\n";

    retdec::cli_parser::CLIReader reader;
    auto cliResult = reader.readFile(inputFile);
    if (!cliResult.success) {
        std::cerr << "Error parsing CLI assembly: " << cliResult.error << "\n";
        return 1;
    }

    retdec::csharp_emitter::CsFileEmitter emitter;
    const std::unordered_map<std::string, retdec::cil_reconstruct::CilReconstructResult> noResults;
    auto emitResult = emitter.emitModule(cliResult.module, noResults);

    std::string combined;
    for (const auto& kv : emitResult.files) {
        if (!kv.second.empty())
            combined += kv.second + "\n";
    }

    if (combined.empty())
        std::cerr << "Warning: C# emitter produced no output.\n";

    try { writeText(outputFile, combined); }
    catch (const std::exception& e) {
        std::cerr << "Error writing output: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "[managed] C# decompilation written to: " << outputFile << "\n";
    return 0;
}

// ─── Python .pyc decompilation ────────────────────────────────────────────────

static int decompilePyc(const std::string& inputFile,
                         const std::string& outputFile)
{
    std::cerr << "[managed] Decompiling Python .pyc: " << inputFile << "\n";

    retdec::pyc_parser::PycReader reader;
    auto pycResult = reader.readFile(inputFile);

    if (!pycResult.success) {
        std::cerr << "Error parsing .pyc: " << pycResult.error << "\n";
        return 1;
    }
    if (!pycResult.root) {
        std::cerr << "Error: PycReader did not produce a root PyCodeObject.\n";
        return 1;
    }
    for (auto& w : pycResult.warnings)
        std::cerr << "Warning: " << w << "\n";

    retdec::py_reconstruct::PyReconstructor reconstructor;
    auto pyModule = reconstructor.reconstruct(
        *pycResult.root,
        pycResult.version.major,
        pycResult.version.minor,
        inputFile);

    for (auto& w : reconstructor.warnings())
        std::cerr << "Warning: " << w << "\n";

    retdec::py_emitter::PyEmitOptions emitOpts;
    emitOpts.pyMajor = pycResult.version.major;
    emitOpts.pyMinor = pycResult.version.minor;
    retdec::py_emitter::PyFileEmitter emitter(emitOpts);
    auto emitResult = emitter.emit(pyModule);

    for (auto& w : emitResult.warnings)
        std::cerr << "Warning: " << w << "\n";

    try { writeText(outputFile, emitResult.source); }
    catch (const std::exception& e) {
        std::cerr << "Error writing output: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "[managed] Python decompilation written to: " << outputFile << "\n";
    return 0;
}

// ─── Lua .luac decompilation ─────────────────────────────────────────────────

static int decompileLua(const std::string& inputFile,
                         const std::string& outputFile)
{
    std::cerr << "[managed] Decompiling Lua bytecode: " << inputFile << "\n";

    std::vector<uint8_t> bytes;
    try { bytes = slurp(inputFile); }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    retdec::lua_parser::LuaReader luaReader(bytes);
    auto luaResult = luaReader.read();

    if (!luaResult.ok) {
        std::cerr << "Error parsing .luac: " << luaResult.error << "\n";
        return 1;
    }

    retdec::lua_parser::LuaEmitter emitter;
    auto emitResult = emitter.emit(luaResult.module);

    try { writeText(outputFile, emitResult.source); }
    catch (const std::exception& e) {
        std::cerr << "Error writing output: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "[managed] Lua decompilation written to: " << outputFile << "\n";
    return 0;
}

// ─── WebAssembly .wasm decompilation ─────────────────────────────────────────

static int decompileWasm(const std::string& inputFile,
                          const std::string& outputFile)
{
    std::cerr << "[managed] Decompiling WebAssembly: " << inputFile << "\n";

    std::vector<uint8_t> bytes;
    try { bytes = slurp(inputFile); }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    retdec::wasm_parser::WasmReader wasmReader(bytes);
    auto wasmResult = wasmReader.read();

    if (!wasmResult.ok) {
        std::cerr << "Error parsing .wasm: " << wasmResult.error << "\n";
        return 1;
    }

    retdec::wasm_parser::WatEmitter emitter;
    auto emitResult = emitter.emit(wasmResult.module);

    std::string watOut = outputFile;
    {
        auto dot = watOut.rfind('.');
        if (dot != std::string::npos)
            watOut = watOut.substr(0, dot);
        watOut += ".wat";
    }

    try { writeText(watOut, emitResult.source); }
    catch (const std::exception& e) {
        std::cerr << "Error writing output: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "[managed] WASM (WAT) decompilation written to: " << watOut << "\n";
    return 0;
}

// ─── Public dispatcher ────────────────────────────────────────────────────────

int decompileManaged(ManagedFormat fmt,
                     const std::string& inputFile,
                     const std::string& outputFile)
{
    switch (fmt) {
        case ManagedFormat::JavaClass:
            return decompileJavaClass(inputFile, outputFile);
        case ManagedFormat::JavaJar:
            return decompileJavaJar(inputFile, outputFile);
        case ManagedFormat::Dex:
            return decompileDex(inputFile, outputFile);
        case ManagedFormat::Apk:
            return decompileApk(inputFile, outputFile);
        case ManagedFormat::CliAssembly:
            return decompileCliAssembly(inputFile, outputFile);
        case ManagedFormat::PythonPyc:
            return decompilePyc(inputFile, outputFile);
        case ManagedFormat::LuaBytecode:
            return decompileLua(inputFile, outputFile);
        case ManagedFormat::Wasm:
            return decompileWasm(inputFile, outputFile);
        default:
            std::cerr << "Error: unknown managed format.\n";
            return 1;
    }
}
