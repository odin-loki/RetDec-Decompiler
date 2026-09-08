/**
 * @file include/retdec/bin2llvmir/optimizations/syscalls/syscalls.h
 * @brief Implement syscall identification and fixing pass @c SyscallFixer.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_BIN2LLVMIR_OPTIMIZATIONS_SYSCALLS_SYSCALLS_H
#define RETDEC_BIN2LLVMIR_OPTIMIZATIONS_SYSCALLS_SYSCALLS_H

#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#include "retdec/bin2llvmir/providers/abi/abi.h"
#include "retdec/bin2llvmir/providers/config.h"
#include "retdec/bin2llvmir/providers/fileimage.h"
#include "retdec/bin2llvmir/providers/lti.h"
#include "retdec/bin2llvmir/utils/debug.h"

// The LOG switch is deliberately NOT defined here. This header used to open with
// `const bool debug_enabled = false;` at global scope, and so did
// decoder_debug.h, which decoder.h and jump_targets.h pulled in -- so the two
// were mutually exclusive: any translation unit including both is rejected
// outright, with "redefinition of 'const bool debug_enabled'". Nothing in the
// tree included both, which is the only reason it built; a pass wanting a
// syscall's decoded target would have hit it immediately.
//
// LOG expands the name at its use site, so the flag belongs to the translation
// unit, which is how the other fifteen bin2llvmir passes already spell it:
// `#define debug_enabled false` in the .cpp. The five syscalls sources and the
// five decoder sources do the same now, and decoder_debug.h -- which existed
// only to hold that definition -- is gone.

namespace retdec {
namespace bin2llvmir {

class AsmInstruction;

class SyscallFixer : public llvm::ModulePass {
public:
	static char ID;
	SyscallFixer();
	virtual bool runOnModule(llvm::Module& M) override;
	bool runOnModuleCustom(llvm::Module& M, Config* c, FileImage* img, Lti* lti, Abi* abi);

private:
	bool run();
	bool transform(AsmInstruction ai, uint64_t code, const std::map<uint64_t, std::string>& codeMap);

	bool runArm();
	bool runArm_linux_32();
	bool runArm_linux_32(AsmInstruction ai);

	bool runArm64();
	bool runArm64_linux_64();
	bool runArm64_linux_64(AsmInstruction ai);

	bool runMips();
	bool runMips_linux();
	bool runMips_linux(AsmInstruction ai);

	bool runX86();
	bool runX86_linux_32();
	bool runX86_linux_32(AsmInstruction ai);

private:
	llvm::Module* _module = nullptr;
	Config* _config = nullptr;
	FileImage* _image = nullptr;
	Lti* _lti = nullptr;
	Abi* _abi = nullptr;
};

} // namespace bin2llvmir
} // namespace retdec

#endif
