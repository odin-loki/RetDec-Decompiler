/**
 * @file include/retdec/vbnet_emitter/vb_file_emitter.h
 * @brief Top-level VB.NET file emitter: BcModule → .vb source string.
 */

#ifndef RETDEC_VBNET_EMITTER_VB_FILE_EMITTER_H
#define RETDEC_VBNET_EMITTER_VB_FILE_EMITTER_H

#include "retdec/bc_module/bc_module.h"

#include <string>
#include <vector>

namespace retdec {
namespace vbnet_emitter {

using namespace bc_module;

struct VbEmitOptions {
    bool emitFileHeader      = true;
    bool sortImports         = true;
    int  blanksBetweenTypes  = 2;
};

struct VbEmitResult {
    std::string source;
    std::vector<std::string> warnings;
};

class VbFileEmitter {
public:
    explicit VbFileEmitter(VbEmitOptions opts = VbEmitOptions{});

    VbEmitResult emit(const BcModule& module) const;

private:
    VbEmitOptions opts_;

    std::string fileHeader(const BcModule& mod) const;
    std::vector<std::string> collectImports(const BcModule& mod) const;
    std::string namespaceName(const BcModule& mod) const;
};

} // namespace vbnet_emitter
} // namespace retdec

#endif // RETDEC_VBNET_EMITTER_VB_FILE_EMITTER_H
