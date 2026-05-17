/**
 * @file include/retdec/pyc_parser/pyc_reader.h
 * @brief Top-level Python .pyc file parser → BcModule.
 *
 * ## Pipeline
 *
 * ```
 * .pyc bytes
 *     │
 *     ▼
 * PycReader::read()
 *     │  1. Parse header (magic, bit_field, mtime/hash, size)
 *     │  2. Detect PythonVersion
 *     │  3. MarshalReader::readObject() → PyCodeObject (root)
 *     │  4. Recursively visit nested code objects (co_consts)
 *     │  5. Build BcModule:
 *     │       - One BcClass per Python module (filename)
 *     │       - One BcMethod per code object (co_name / co_qualname)
 *     │       - BcLocalVar entries from co_varnames
 *     │       - BcCFG with one BcBasicBlock per basic block
 *     │         (instructions as BcOpcode::PYTHON_* entries)
 *     │       - Annotations: decorator list from co_consts heuristics
 *     ▼
 * BcModule
 * ```
 *
 * ## BcOpcode mapping
 *
 * Python bytecode instructions are mapped to `BcOpcode::PYTHON_*` values
 * defined in `bc_instr.h`.  The mapping uses the logical opcode name, not
 * the raw byte value, so version differences are normalised.
 *
 * ## BcCFG construction
 *
 * Basic block boundaries are identified by:
 *   - Jump targets (absolute and relative)
 *   - Jump instructions themselves (end of block)
 *   - RETURN_VALUE / RAISE_VARARGS / RERAISE
 *   - Exception table entries (Python 3.11+)
 *
 * ## Output quality
 *
 * All co_varnames, co_freevars, co_cellvars are mapped to BcLocalVar.
 * Parameters are identified by position (first co_argcount entries of
 * co_varnames).  Line numbers are resolved via the decoded line table.
 */

#ifndef RETDEC_PYC_PARSER_PYC_READER_H
#define RETDEC_PYC_PARSER_PYC_READER_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/pyc_parser/py_code_object.h"
#include "retdec/pyc_parser/pyc_magic.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace retdec { namespace py_reconstruct { class PyModuleReconstructor; } }

namespace retdec {
namespace pyc_parser {

using namespace bc_module;

// ─── PycReadOptions ───────────────────────────────────────────────────────────

struct PycReadOptions {
    bool buildCFG        = true;   ///< Build BcCFG for each method
    bool decodeLinetable = true;   ///< Decode line number tables
    bool recurseNested   = true;   ///< Parse nested code objects (lambdas, etc.)
    bool skipCompGenerated = true; ///< Skip <genexpr>, <listcomp>, <dictcomp>, <setcomp>
    bool addRawBytecodeAnnotation = false; ///< Add raw bytecode hex as annotation
};

// ─── PycReadResult ────────────────────────────────────────────────────────────

struct PycReadResult {
    BcModule module;

    bool   success   = false;
    std::string error;

    PythonVersion version;

    int  totalCodeObjects  = 0;
    int  parsedMethods     = 0;
    int  skippedMethods    = 0;
    int  totalInstructions = 0;

    std::vector<std::string> warnings;

    /// Root PyCodeObject — populated after a successful read.
    /// Provides access to the full code tree for py_reconstruct.
    std::shared_ptr<PyCodeObject> root;
};

// ─── PycReader ────────────────────────────────────────────────────────────────

/**
 * @brief Top-level Python .pyc file parser.
 */
class PycReader {
public:
    explicit PycReader(PycReadOptions opts = PycReadOptions{});

    /**
     * @brief Parse a .pyc file from a byte buffer.
     *
     * @param data   Pointer to .pyc file contents.
     * @param size   File size in bytes.
     * @param srcPath Optional original filename (for BcModule naming).
     */
    PycReadResult read(const uint8_t* data, size_t size,
                       const std::string& srcPath = "");

    /**
     * @brief Parse a .pyc file from disk.
     */
    PycReadResult readFile(const std::string& path);

private:
    PycReadOptions opts_;

    // ── Module building ───────────────────────────────────────────────────────

    void buildModule(const PyCodeObject& root,
                     const std::string& moduleName,
                     PycReadResult& result);

    /// Recursively emit all code objects under root into the BcClass.
    void emitCodeObject(const PyCodeObject& code,
                        BcClass& cls,
                        PycReadResult& result,
                        const std::string& parentQual = "");

    // ── BcMethod construction ─────────────────────────────────────────────────

    BcMethod buildMethod(const PyCodeObject& code,
                         const std::string& qualName) const;

    // ── BcCFG construction ────────────────────────────────────────────────────

    void buildCFG(const PyCodeObject& code, BcMethod& method) const;

    /// Identify basic block leaders (entry points) by scanning jump targets.
    std::vector<uint32_t> findLeaders(const PyCodeObject& code) const;

    /// Map raw Python opcode + arg → BcOpcode + BcOperand.
    void liftInstruction(uint8_t rawOp, int32_t arg,
                         uint32_t offset,
                         const PyCodeObject& code,
                         BcInstruction& out) const;

    // ── Type mapping ──────────────────────────────────────────────────────────

    BcType constType(const PyCodeObject::Const& c) const;
    BcType nameType(const std::string& name) const;
};

} // namespace pyc_parser
} // namespace retdec

#endif // RETDEC_PYC_PARSER_PYC_READER_H
