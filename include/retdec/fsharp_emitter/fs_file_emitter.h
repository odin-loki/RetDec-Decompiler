/**
 * @file include/retdec/fsharp_emitter/fs_file_emitter.h
 * @brief Top-level F# file emitter: BcModule → .fs source string.
 *
 * Output format:
 *   - `module` or `namespace` declaration
 *   - `open` directives (sorted, deduped)
 *   - Type declarations in dependency order
 *   - Auto-generated file header comment
 */

#ifndef RETDEC_FSHARP_EMITTER_FS_FILE_EMITTER_H
#define RETDEC_FSHARP_EMITTER_FS_FILE_EMITTER_H

#include "retdec/bc_module/bc_module.h"

#include <string>
#include <vector>

namespace retdec {
namespace fsharp_emitter {

using namespace bc_module;

struct FsEmitOptions {
    bool emitFileHeader = true;
    bool sortOpenDecls  = true;
    bool useNamespace   = false;  ///< true → namespace Foo, false → module Foo
    int  blanksBetweenTypes = 2;
};

struct FsEmitResult {
    std::string source;
    std::vector<std::string> warnings;
};

class FsFileEmitter {
public:
    explicit FsFileEmitter(FsEmitOptions opts = FsEmitOptions{});

    FsEmitResult emit(const BcModule& module) const;

private:
    FsEmitOptions opts_;

    std::string fileHeader(const BcModule& mod) const;
    std::vector<std::string> collectOpenDecls(const BcModule& mod) const;
    std::string topLevelDecl(const BcModule& mod) const;
};

} // namespace fsharp_emitter
} // namespace retdec

#endif // RETDEC_FSHARP_EMITTER_FS_FILE_EMITTER_H
