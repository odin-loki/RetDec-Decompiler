/**
 * @file include/retdec/jvm_parser/jvm_class_parser.h
 * @brief JVM ClassFile parser: magic + version + constant pool + fields +
 *        methods + attributes → BcClass.
 *
 * Supports class-file versions 45 (Java 1.1) through 65 (Java 21).
 * Unknown attributes are stored as RawAttr and do not cause parse errors.
 */

#ifndef RETDEC_JVM_PARSER_JVM_CLASS_PARSER_H
#define RETDEC_JVM_PARSER_JVM_CLASS_PARSER_H

#include "retdec/jvm_parser/jvm_attr.h"
#include "retdec/jvm_parser/jvm_signature.h"
#include "retdec/bc_module/bc_module.h"

#include <cstdint>
#include <string>
#include <vector>

namespace retdec {
namespace jvm_parser {

using BcClass  = bc_module::BcClass;
using BcField  = bc_module::BcField;
using BcMethod = bc_module::BcMethod;
using BcModule = bc_module::BcModule;

// ─── Parse options ────────────────────────────────────────────────────────────

struct JvmParseOptions {
    bool parseBytecode    = true;   ///< Lift Code attribute → BcCFG
    bool parseAnnotations = true;   ///< Decode RuntimeVisible* attributes
    bool parseDebugInfo   = true;   ///< LVT/LVTT/LineNumberTable
    bool strictVersion    = false;  ///< Fail on unknown class-file version
};

// ─── Parse result ─────────────────────────────────────────────────────────────

struct JvmParseResult {
    bool        ok    = false;
    std::string error;

    BcClass     cls;
    uint16_t    majorVersion = 0;
    uint16_t    minorVersion = 0;

    /// Resolved constant pool (kept for cross-class reference resolution).
    ConstPool   pool;

    /// Bootstrap methods (for invokedynamic / lambda resolution).
    BootstrapMethodsAttr bootstrap;
};

// ─── ClassFile parser ─────────────────────────────────────────────────────────

/**
 * @brief Parse a JVM .class file from raw bytes.
 *
 * @param data    Pointer to .class file bytes.
 * @param size    Number of bytes.
 * @param opts    Parse options.
 */
JvmParseResult parseClassFile(const uint8_t* data, size_t size,
                               const JvmParseOptions& opts = JvmParseOptions{});

/**
 * @brief Parse a JVM .class file from a vector of bytes.
 */
JvmParseResult parseClassFile(const std::vector<uint8_t>& data,
                               const JvmParseOptions& opts = JvmParseOptions{});

// ─── Java version helpers ─────────────────────────────────────────────────────

/// Return the Java release number for a class-file major version (e.g. 52→8).
int javaRelease(uint16_t majorVersion) noexcept;

/// Human-readable version string (e.g. "Java 17 (class version 61.0)").
std::string javaVersionString(uint16_t major, uint16_t minor);

} // namespace jvm_parser
} // namespace retdec

#endif // RETDEC_JVM_PARSER_JVM_CLASS_PARSER_H
