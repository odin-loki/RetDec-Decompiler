/**
 * @file src/retdec-decompiler/managed_decompiler.h
 * @brief Managed-language decompilation dispatcher interface.
 *
 * Copyright (c) 2025 Odin Loch trading as Imortek. All rights reserved.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

/// Identifies a managed / bytecode file format.
enum class ManagedFormat {
    Unknown,
    JavaClass,   ///< JVM .class file (magic CAFEBABE)
    JavaJar,     ///< JAR archive (ZIP containing .class entries)
    Dex,         ///< Android DEX bytecode (bare .dex)
    Apk,         ///< Android APK (ZIP containing classes*.dex)
    PythonPyc,   ///< CPython compiled bytecode (.pyc)
    LuaBytecode, ///< Lua compiled bytecode (.luac)
    Wasm,        ///< WebAssembly binary (.wasm)
    CliAssembly, ///< .NET PE with CLI metadata (.dll / .exe)
};

/**
 * @brief Probe @p data and return its managed format.
 *
 * Returns ManagedFormat::Unknown if the buffer does not match any known
 * managed-language magic sequence or container signature.
 */
ManagedFormat detectManagedFormatFromBytes(const uint8_t* data, std::size_t size);

/**
 * @brief Probe the first few bytes of @p path and return its managed format.
 *
 * Returns ManagedFormat::Unknown if the file does not match any known
 * managed-language magic sequence.
 */
ManagedFormat detectManagedFormat(const std::string& path);

/** Human-readable format label for logs (e.g. "Java JAR"). */
const char* managedFormatName(ManagedFormat fmt);

/**
 * @brief Suggested `--output-lang` value for @p fmt, or nullptr if N/A.
 *
 * Managed formats bypass the native LLVM pipeline; this hint documents the
 * language the managed emitter produces.
 */
const char* managedOutputLangHint(ManagedFormat fmt);

/** Log detected format and `--output-lang` hint to @p os. */
void logManagedFormatRoute(ManagedFormat fmt, const std::string& path,
                           std::ostream& os);

/**
 * @brief Log guidance when no managed format matched.
 *
 * Uses @p data (when non-empty) and the file extension of @p path to suggest
 * likely causes (empty ZIP, native PE, unsupported container, etc.).
 */
void logUnknownManagedFormat(const uint8_t* data, std::size_t size,
                             const std::string& path, std::ostream& os);

/**
 * @brief Decompile a managed-language binary.
 *
 * @param fmt        The format (must not be Unknown).
 * @param inputFile  Path to the input binary.
 * @param outputFile Path for the decompiled source output.
 * @return 0 on success, non-zero on failure.
 */
int decompileManaged(ManagedFormat fmt,
                     const std::string& inputFile,
                     const std::string& outputFile);
