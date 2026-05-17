/**
 * @file src/retdec-decompiler/managed_decompiler.h
 * @brief Managed-language decompilation dispatcher interface.
 *
 * Copyright (c) 2025 Odin Loch trading as Imortek. All rights reserved.
 */

#pragma once

#include <string>

/// Identifies a managed / bytecode file format.
enum class ManagedFormat {
    Unknown,
    JavaClass,   ///< JVM .class file (magic CAFEBABE)
    Dex,         ///< Android DEX / APK bytecode
    PythonPyc,   ///< CPython compiled bytecode (.pyc)
    LuaBytecode, ///< Lua compiled bytecode (.luac)
    Wasm,        ///< WebAssembly binary (.wasm)
};

/**
 * @brief Probe the first few bytes of @p path and return its managed format.
 *
 * Returns ManagedFormat::Unknown if the file does not match any known
 * managed-language magic sequence.
 */
ManagedFormat detectManagedFormat(const std::string& path);

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
