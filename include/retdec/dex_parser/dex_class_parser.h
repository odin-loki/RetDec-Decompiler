/**
 * @file include/retdec/dex_parser/dex_class_parser.h
 * @brief DEX class_def_item → BcClass converter.
 *
 * Reads annotation_directory_item, encoded_array_item (static values),
 * class_data_item (fields/methods), resolves generic signatures from the
 * dalvik.annotation.Signature system annotation, and lifts each method's
 * code_item to a BcCFG via DexLifter.
 */

#ifndef RETDEC_DEX_PARSER_DEX_CLASS_PARSER_H
#define RETDEC_DEX_PARSER_DEX_CLASS_PARSER_H

#include "retdec/bc_module/bc_module.h"
#include "retdec/dex_parser/dex_header.h"
#include "retdec/dex_parser/dex_lifter.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace retdec {
namespace dex_parser {

struct DexParseOptions {
    bool parseBytecode   = true;
    bool parseAnnotations = true;
    bool resolveGenerics  = true;  ///< Parse dalvik.annotation.Signature
    bool strict          = false;  ///< Reject unknown DEX versions
};

/**
 * @brief Result of parsing a single class_def_item.
 */
struct DexClassResult {
    enum Status { OK, Skipped, Error };
    Status                          status = OK;
    std::string                     error;
    std::shared_ptr<bc_module::BcClass> bcClass;
};

/**
 * @brief Converts a single DEX class_def_item to a BcClass.
 */
class DexClassParser {
public:
    DexClassParser(const DexFile& dexFile, DexParseOptions opts = DexParseOptions{});

    DexClassResult parseClass(uint32_t classDefIdx);

private:
    const DexFile&   dex_;
    DexParseOptions  opts_;
    DexLifter        lifter_;

    bc_module::BcAccess convertAccessFlags(uint32_t flags) const;

    void parseFields(bc_module::BcClass& cls,
                     const std::vector<EncodedField>& fields,
                     bool isStatic);

    void parseMethods(bc_module::BcClass& cls,
                      const std::vector<EncodedMethod>& methods);

    void parseAnnotations(bc_module::BcClass& cls,
                          uint32_t annotationsOff);

    /// Parse dalvik.annotation.Signature → generic descriptor string.
    std::string resolveGenericSignature(uint32_t annotationsOff,
                                        uint32_t memberIdx,
                                        bool isMethod) const;

    bc_module::BcType descriptorToType(const std::string& desc) const;
};

} // namespace dex_parser
} // namespace retdec

#endif // RETDEC_DEX_PARSER_DEX_CLASS_PARSER_H
