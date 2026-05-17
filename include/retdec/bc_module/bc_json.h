/**
 * @file include/retdec/bc_module/bc_json.h
 * @brief JSON serialisation / deserialisation for BcModule.
 *
 * Used for:
 *   - Debug dumps of the IR.
 *   - Golden-file testing (BcModule → JSON → re-parse → compare).
 *   - Pipeline checkpointing (save partial analysis results).
 *
 * The JSON format is self-describing and versioned.  Unknown fields are
 * silently ignored on deserialisation to support forward compatibility.
 *
 * ## Top-level schema
 * ```json
 * {
 *   "bcModuleVersion": 1,
 *   "name":        "HelloWorld",
 *   "sourceLang":  "Java",
 *   "stringPool":  ["Hello", "World", ...],
 *   "classes":     [ { BcClass JSON } ],
 *   "externalRefs": { "java/io/PrintStream": "import java.io.PrintStream;" }
 * }
 * ```
 */

#ifndef RETDEC_BC_MODULE_BC_JSON_H
#define RETDEC_BC_MODULE_BC_JSON_H

#include "retdec/bc_module/bc_module.h"

#include <string>
#include <sstream>

namespace retdec {
namespace bc_module {
namespace json {

static constexpr int kVersion = 1;

// ─── Serialise ────────────────────────────────────────────────────────────────

/// Serialise a BcType to a JSON string fragment.
std::string serialiseType(const BcType& t);

/// Serialise a BcInstruction to a JSON string fragment.
std::string serialiseInstr(const BcInstruction& i, int indent = 0);

/// Serialise a BcBasicBlock to a JSON string fragment.
std::string serialiseBlock(const BcBasicBlock& b, int indent = 0);

/// Serialise a BcCFG to a JSON string fragment.
std::string serialiseCFG(const BcCFG& cfg, int indent = 0);

/// Serialise a BcMethod to a JSON string fragment.
std::string serialiseMethod(const BcMethod& m, int indent = 0);

/// Serialise a BcField to a JSON string fragment.
std::string serialiseField(const BcField& f, int indent = 0);

/// Serialise a BcClass to a JSON string fragment.
std::string serialiseClass(const BcClass& c, int indent = 0);

/// Serialise a complete BcModule to a JSON string.
std::string serialiseModule(const BcModule& mod, int indent = 0);

// ─── Deserialise ─────────────────────────────────────────────────────────────

struct ParseResult {
    bool        ok     = false;
    std::string error;
    BcModule    module{"", SourceLang::Unknown};
};

/// Deserialise a JSON string into a BcModule.
/// Returns ParseResult::ok == false and a diagnostic on failure.
ParseResult deserialiseModule(const std::string& json);

// ─── Round-trip helper ────────────────────────────────────────────────────────

/**
 * Serialise then immediately re-deserialise a BcModule.
 * Returns true if the result compares equal (used in tests).
 */
bool roundTripEquals(const BcModule& original, std::string* diffOut = nullptr);

} // namespace json
} // namespace bc_module
} // namespace retdec

#endif // RETDEC_BC_MODULE_BC_JSON_H
