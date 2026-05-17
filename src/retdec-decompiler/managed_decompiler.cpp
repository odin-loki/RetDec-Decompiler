/**
 * @file src/retdec-decompiler/managed_decompiler.cpp
 * @brief Managed-language decompilation dispatcher.
 *
 * Handles Java .class, DEX, Python .pyc, Lua .luac, and WebAssembly .wasm
 * files by routing them to their respective parser + emitter pipelines,
 * completely bypassing the LLVM IR pipeline used for native binaries.
 *
 * Copyright (c) 2025 Odin Loch trading as Imortek. All rights reserved.
 */

#include "managed_decompiler.h"

// Java / DEX
#include "retdec/jvm_parser/jvm_class_parser.h"
#include "retdec/dex_parser/dex_apk_reader.h"
#include "retdec/java_emitter/java_file_emitter.h"

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

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
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

} // anonymous namespace

// ─── Format detection ─────────────────────────────────────────────────────────

ManagedFormat detectManagedFormat(const std::string& path)
{
    std::vector<uint8_t> buf;
    try { buf = slurp(path); } catch (...) { return ManagedFormat::Unknown; }
    if (buf.size() < 4) return ManagedFormat::Unknown;

    const uint8_t* d = buf.data();

    // Java .class: CAFEBABE (big-endian)
    if (d[0] == 0xCA && d[1] == 0xFE && d[2] == 0xBA && d[3] == 0xBE) {
        // Distinguish Mach-O FAT binary (also starts with CAFEBABE).
        // A real .class file has minor/major version in bytes 4-7 (big-endian uint16).
        // FAT binaries have a large slice count in bytes 4-7.
        if (buf.size() >= 8) {
            uint32_t maybeNArch = (uint32_t(d[4]) << 24) | (uint32_t(d[5]) << 16)
                                | (uint32_t(d[6]) << 8)  |  uint32_t(d[7]);
            // Class file major versions: 45 (Java 1)..65 (Java 21)
            // FAT slice counts are typically in the millions range
            uint16_t minorVer = uint16_t(uint16_t(d[4]) << 8 | d[5]);
            uint16_t majorVer = uint16_t(uint16_t(d[6]) << 8 | d[7]);
            if (majorVer >= 44 && majorVer <= 70)
                return ManagedFormat::JavaClass;
            (void)maybeNArch;
            (void)minorVer;
        }
        return ManagedFormat::Unknown; // Mach-O FAT
    }

    // DEX: "dex\n" at offset 0
    if (d[0] == 0x64 && d[1] == 0x65 && d[2] == 0x78 && d[3] == 0x0A)
        return ManagedFormat::Dex;

    // WebAssembly: "\0asm"
    if (d[0] == 0x00 && d[1] == 0x61 && d[2] == 0x73 && d[3] == 0x6D)
        return ManagedFormat::Wasm;

    // Lua compiled bytecode: "\x1bLua"
    if (d[0] == 0x1B && d[1] == 0x4C && d[2] == 0x75 && d[3] == 0x61)
        return ManagedFormat::LuaBytecode;

    // Python .pyc: upper 16 bits of the 4-byte magic == 0x0D0A (stored LE).
    // Bytes [2] and [3] must be 0x0D and 0x0A respectively.
    if (buf.size() >= 4 && d[2] == 0x0D && d[3] == 0x0A) {
        // Lower 16 bits (bytes 0-1 in LE) are the version-specific magic.
        uint16_t magicWord = uint16_t(d[0]) | uint16_t(uint16_t(d[1]) << 8);
        // Reject clearly invalid magic words (0 or suspiciously large).
        if (magicWord != 0)
            return ManagedFormat::PythonPyc;
    }

    return ManagedFormat::Unknown;
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

    // Reconstruct structured Python AST.
    retdec::py_reconstruct::PyReconstructor reconstructor;
    auto pyModule = reconstructor.reconstruct(
        *pycResult.root,
        pycResult.version.major,
        pycResult.version.minor,
        inputFile);

    for (auto& w : reconstructor.warnings())
        std::cerr << "Warning: " << w << "\n";

    // Emit Python source.
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

    // Use .wat extension for WAT output alongside the requested output path.
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
        case ManagedFormat::Dex:
            return decompileDex(inputFile, outputFile);
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
