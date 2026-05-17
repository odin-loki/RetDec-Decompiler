#pragma once
#include <llvm/IR/Instructions.h>
#include <stack>
namespace retdec { namespace bin2llvmir {
bool simpleTypesFpExt(llvm::Instruction* user, std::stack<llvm::Value*>& toProcess);
} }
